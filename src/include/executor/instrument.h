/*-------------------------------------------------------------------------
 *
 * instrument.h
 *	  definitions for run-time statistics collection
 *
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * src/include/executor/instrument.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include "lib/ilist.h"
#include "nodes/pg_list.h"
#include "portability/instr_time.h"


/*
 * BufferUsage and WalUsage counters keep being incremented infinitely,
 * i.e., must never be reset to zero, so that we can calculate how much
 * the counters are incremented in an arbitrary period.
 */
typedef struct BufferUsage
{
	int64		shared_blks_hit;	/* # of shared buffer hits */
	int64		shared_blks_read;	/* # of shared disk blocks read */
	int64		shared_blks_dirtied;	/* # of shared blocks dirtied */
	int64		shared_blks_written;	/* # of shared disk blocks written */
	int64		local_blks_hit; /* # of local buffer hits */
	int64		local_blks_read;	/* # of local disk blocks read */
	int64		local_blks_dirtied; /* # of local blocks dirtied */
	int64		local_blks_written; /* # of local disk blocks written */
	int64		temp_blks_read; /* # of temp blocks read */
	int64		temp_blks_written;	/* # of temp blocks written */
	instr_time	shared_blk_read_time;	/* time spent reading shared blocks */
	instr_time	shared_blk_write_time;	/* time spent writing shared blocks */
	instr_time	local_blk_read_time;	/* time spent reading local blocks */
	instr_time	local_blk_write_time;	/* time spent writing local blocks */
	instr_time	temp_blk_read_time; /* time spent reading temp blocks */
	instr_time	temp_blk_write_time;	/* time spent writing temp blocks */
} BufferUsage;

/*
 * WalUsage tracks only WAL activity like WAL records generation that
 * can be measured per query and is displayed by EXPLAIN command,
 * pg_stat_statements extension, etc. It does not track other WAL activity
 * like WAL writes that it's not worth measuring per query. That's tracked
 * by WAL global statistics counters in WalStats, instead.
 */
typedef struct WalUsage
{
	int64		wal_records;	/* # of WAL records produced */
	int64		wal_fpi;		/* # of WAL full page images produced */
	uint64		wal_bytes;		/* size of WAL records produced */
	uint64		wal_fpi_bytes;	/* size of WAL full page images produced */
	int64		wal_buffers_full;	/* # of times the WAL buffers became full */
} WalUsage;

/* Flag bits included in the instrument_options bitmask */
typedef enum InstrumentOption
{
	INSTRUMENT_TIMER = 1 << 0,	/* needs timer (and row counts) */
	INSTRUMENT_BUFFERS = 1 << 1,	/* needs buffer usage */
	INSTRUMENT_ROWS = 1 << 2,	/* needs row count */
	INSTRUMENT_WAL = 1 << 3,	/* needs WAL usage */
	INSTRUMENT_IO = 1 << 4,		/* needs IO usage */
	INSTRUMENT_ALL = PG_INT32_MAX
} InstrumentOption;

/*
 * Instrumentation base class for capturing time and WAL/buffer usage
 *
 * If used directly:
 * - Allocate on the stack and zero initialize the struct
 * - Call InstrInitOptions to set instrumentation options
 * - Call InstrStart before the activity you want to measure
 * - Call InstrStop / InstrStopFinalize after the activity to capture totals
 *
 * InstrStart/InstrStop may be called multiple times. The last stop call must
 * be to InstrStopFinalize to ensure parent stack entries get the accumulated
 * totals. If there is risk of transaction aborts you must call
 * InstrStopFinalize in a PG_TRY/PG_FINALLY block to avoid corrupting the
 * instrumentation stack.
 *
 * In a query context use QueryInstrumentation instead, which handles aborts
 * using the resource owner logic.
 */
typedef enum InstrKind
{
	INSTR_KIND_PLAIN = 0,		/* standalone entry, e.g. around a utility
								 * statement or planning */
	INSTR_KIND_QUERY,			/* QueryInstrumentation */
	INSTR_KIND_NODE,			/* NodeInstrumentation of a plan node */
	INSTR_KIND_NODE_DETAIL,		/* part of a plan node, e.g. the table access
								 * of an index scan */
	INSTR_KIND_TRIGGER,			/* TriggerInstrumentation */
} InstrKind;

typedef struct Instrumentation
{
	/* Parameters set at creation: */
	bool		need_timer;		/* true if we need timer data */
	bool		need_stack;		/* true if we need WAL/buffer usage data, i.e.
								 * the entry goes on the stack while running
								 * (always set for query entries) */
	/* Internal state keeping: */
	bool		on_stack;		/* true if currently on instr_stack */
	bool		finalized;		/* true once accumulated to a parent via
								 * InstrFinalizeChild */
	InstrKind	kind;			/* what this entry describes */
	instr_time	starttime;		/* start time of last InstrStart */
	/* Accumulated statistics: */
	instr_time	total;			/* total runtime */
	BufferUsage bufusage;		/* total buffer usage */
	WalUsage	walusage;		/* total WAL usage */
	/* Abort handling: link in parent QueryInstrumentation's unfinalized list */
	dlist_node	unfinalized_entry;
} Instrumentation;

/*
 * Statistics about a statement run while another statement was executing,
 * e.g. through SPI from a function or a trigger, as collected by the
 * enclosing QueryInstrumentation for EXPLAIN (see InstrQueryTrackNested).
 * Executions of the same statement from the same origin are aggregated.
 */
typedef struct NestedStatementInstrumentation
{
	int64		queryId;		/* query identifier, if computed */
	char	   *query_text;		/* source text of the statement */
	/* Where in the enclosing statement it was run from: */
	int			origin_node_id;	/* plan_node_id of the running plan node, or
								 * -1 if unknown */
	char	   *origin_tgname;	/* trigger that ran it, or NULL */
	char	   *origin_relname; /* relation of that trigger */
	/* Aggregated over all executions: */
	int64		calls;			/* number of executions */
	instr_time	total;			/* total runtime */
	BufferUsage bufusage;		/* total buffer usage, including workers */
	WalUsage	walusage;		/* total WAL usage, including workers */
	List	   *nested;			/* statements it ran itself, same type */
} NestedStatementInstrumentation;

/*
 * Query-related instrumentation tracking.
 *
 * Usage:
 * - Allocate on the heap using InstrQueryAlloc (required for abort handling)
 * - Call InstrQueryStart before the activity you want to measure
 * - Call InstrQueryStop / InstrQueryStopFinalize afterwards to capture totals
 *
 * InstrQueryStart/InstrQueryStop may be called multiple times. The last stop
 * call must be to InstrQueryStopFinalize to ensure parent stack entries get
 * the accumulated totals.
 *
 * Uses resource owner mechanism for handling aborts: the first InstrQueryStart
 * registers the entry with the current resource owner, and it stays registered
 * (also across InstrQueryStop / InstrQueryStart pairs) until
 * InstrQueryStopFinalize. In the case of an abort, logic equivalent to
 * InstrQueryStopFinalize is run by the resource owner cleanup, so the caller
 * must not release that resource owner without an abort before having called
 * InstrQueryStopFinalize.
 *
 * Query entries are always on the instrumentation stack while started, so
 * that a statement run while another one executes (a nested statement) can
 * find its enclosing statement.  When finalized, a query entry accumulates to
 * the nearest enclosing query or plain entry rather than to the plan node or
 * trigger that ran it, so that per-node stats only describe the node's own
 * activity.  An enclosing entry that tracks nested statements (see
 * InstrQueryTrackNested) additionally receives a NestedStatementInstrumentation
 * record, which EXPLAIN lists separately.
 *
 * Utility statements use entries that live on the C stack of whoever asked
 * for them (see InstrQueryInitLocal and UtilityStmtDesc), which are neither
 * tied to a resource owner nor own memory, as transaction control statements
 * release both while running.
 */
struct ResourceOwnerData;
typedef struct QueryInstrumentation
{
	Instrumentation instr;

	/* Original instrument_options flags used to create this instrumentation */
	int			instrument_options;

	/* Resource owner used for cleanup for aborts between InstrStart/InstrStop */
	struct ResourceOwnerData *owner;

	/*
	 * Nested statement tracking.  track_nested is set when records should be
	 * collected from nested statements into 'nested'.  The remaining fields
	 * describe this statement for the record it produces in its enclosing
	 * statement's list, and are only set up when that one tracks nested
	 * statements (see InstrQuerySetStatement).
	 */
	bool		track_nested;	/* collect records from nested statements */
	MemoryContext nested_cxt;	/* where the records live: instr_cxt, or for
								 * an entry without one the context of the
								 * enclosing statement that collects them */
	bool		started;		/* InstrQueryStart has been called */
	bool		running;		/* between InstrQueryStart and InstrQueryStop */
	Instrumentation *saved_origin;	/* instr_current_origin to restore at
									 * InstrQueryStop */
	int64		queryId;		/* query identifier, if computed */
	char	   *query_text;		/* copy of the statement's source text */
	int			origin_node_id;	/* plan node of the enclosing statement that
								 * ran this one, or -1 */
	char	   *origin_tgname;	/* trigger that ran this one, or NULL */
	char	   *origin_relname; /* relation of that trigger */
	List	   *nested;			/* NestedStatementInstrumentation records */

	/*
	 * Dedicated memory context for all instrumentation allocations belonging
	 * to this query (node instrumentation, trigger instrumentation, etc.).
	 * Initially a child of the context current at InstrQueryAlloc, moved
	 * under TopMemoryContext by InstrQueryStart while the ResourceOwner is
	 * responsible for it (so it survives transaction abort for the cleanup),
	 * and reassigned to the current memory context on InstrQueryStopFinalize.
	 * NULL for entries set up with InstrQueryInitLocal.
	 */
	MemoryContext instr_cxt;

	/*
	 * Child entries that need to be cleaned up on abort, since they are not
	 * registered as a resource owner themselves. Contains both node and
	 * trigger instrumentation entries linked via instr.unfinalized_entry.
	 */
	dlist_head	unfinalized_entries;
} QueryInstrumentation;

/*
 * Specialized instrumentation for per-node execution statistics
 *
 * Relies on an outer QueryInstrumentation having been set up to handle the
 * stack used for WAL/buffer usage statistics, and relies on it for managing
 * aborts. Solely intended for the executor and anyone reporting about its
 * activities (e.g. EXPLAIN ANALYZE).
 */
typedef struct NodeInstrumentation
{
	Instrumentation instr;
	/* Parameters set at node creation: */
	bool		async_mode;		/* true if node is in async mode */
	int			plan_node_id;	/* plan_node_id of the node, or -1 */
	/* Info about current plan cycle: */
	bool		running;		/* true if we've completed first tuple */
	instr_time	counter;		/* accumulated runtime for this node */
	instr_time	firsttuple;		/* time for first tuple of this cycle */
	double		tuplecount;		/* # of tuples emitted so far this cycle */
	/* Accumulated statistics across all completed cycles: */
	instr_time	startup;		/* total startup time */
	double		ntuples;		/* total tuples produced */
	double		ntuples2;		/* secondary node-specific tuple counter */
	double		nloops;			/* # of run cycles for this node */
	double		nfiltered1;		/* # of tuples removed by scanqual or joinqual */
	double		nfiltered2;		/* # of tuples removed by "other" quals */
} NodeInstrumentation;

/*
 * Care must be taken with any pointers contained within this struct, as this
 * gets copied across processes during parallel query execution.
 */
typedef struct WorkerNodeInstrumentation
{
	int			num_workers;	/* # of structures that follow */
	NodeInstrumentation instrument[FLEXIBLE_ARRAY_MEMBER];
} WorkerNodeInstrumentation;

typedef struct TriggerInstrumentation
{
	Instrumentation instr;
	int64		firings;		/* number of times the instrumented trigger
								 * was fired */
	const char *tgname;			/* trigger name, for nested statement records */
	const char *relname;		/* its relation's name */
	Instrumentation *saved_origin;	/* instr_current_origin to restore at
									 * InstrStopTrigger */
} TriggerInstrumentation;

/*
 * Dynamic array-based stack for tracking current WAL/buffer usage context.
 *
 * When the stack is empty, 'current' points to instr_top which accumulates
 * session-level totals.
 */
typedef struct InstrStackState
{
	int			stack_space;	/* allocated capacity of entries array */
	int			stack_size;		/* current number of entries */

	Instrumentation **entries;	/* dynamic array of pointers */
	Instrumentation *current;	/* top of stack, or &instr_top when empty */
} InstrStackState;

extern PGDLLIMPORT WalUsage pgWalUsage;

/*
 * The top instrumentation represents a running total of the current backend
 * WAL/buffer usage information. This will not be updated immediately, but
 * rather when the current stack entry gets accumulated which typically happens
 * at query end.
 *
 * In a parallel query leader this also includes the usage reported back by
 * parallel workers, since that gets accumulated into the leader's stack.
 */
extern PGDLLIMPORT Instrumentation instr_top;

/*
 * The plan node or trigger entry of the innermost running statement that is
 * executing right now, or NULL.  Maintained by the instrumented ExecProcNode
 * wrappers and InstrStartTrigger/InstrStopTrigger, and scoped to the running
 * statement by InstrQueryStart/InstrQueryStop, so that a nested statement can
 * record where it was run from (see NestedStatementInstrumentation).  Kept
 * separately from the stack, which only tracks entries collecting buffer/WAL
 * usage, since this is needed whichever options are on and is much cheaper to
 * maintain than a stack entry.
 */
extern PGDLLIMPORT Instrumentation *instr_current_origin;

/*
 * The instrumentation stack state. The 'current' field points to the
 * currently active stack entry that is getting updated as activity happens,
 * and will be accumulated to parent stacks when it gets finalized by
 * InstrStop (for non-executor use cases), ExecFinalizeNodeInstrumentation
 * (executor finish) or ResOwnerReleaseInstrumentation on abort.
 */
extern PGDLLIMPORT InstrStackState instr_stack;

extern void InstrStackGrow(void);

/*
 * Pushes the stack so that all WAL/buffer usage updates go to the passed in
 * instrumentation entry.
 *
 * See note on InstrPopStack regarding safe use of these functions.
 */
static inline void
InstrPushStack(Instrumentation *instr)
{
	if (unlikely(instr_stack.stack_size == instr_stack.stack_space))
		InstrStackGrow();

	instr_stack.entries[instr_stack.stack_size++] = instr;
	instr_stack.current = instr;
	instr->on_stack = true;
}

/*
 * Pops the stack entry back to the previous one that was effective at
 * InstrPushStack.
 *
 * Callers must ensure that no intermediate stack entries are skipped, to
 * handle aborts correctly. If you're thinking of calling this in a PG_FINALLY
 * block, consider instead using InstrStart + InstrStopFinalize which can skip
 * intermediate stack entries.
 */
static inline void
InstrPopStack(Instrumentation *instr)
{
	Assert(instr_stack.stack_size > 0);
	Assert(instr_stack.entries[instr_stack.stack_size - 1] == instr);
	instr_stack.stack_size--;
	instr_stack.current = instr_stack.stack_size > 0
		? instr_stack.entries[instr_stack.stack_size - 1]
		: &instr_top;
	instr->on_stack = false;
}

extern void InstrInitOptions(Instrumentation *instr, int instrument_options);
extern void InstrStart(Instrumentation *instr);
extern void InstrStop(Instrumentation *instr);
extern void InstrStopFinalize(Instrumentation *instr);
extern void InstrFinalizeChild(Instrumentation *instr, Instrumentation *parent);
extern void InstrAccumStack(Instrumentation *dst, Instrumentation *add);

extern QueryInstrumentation *InstrQueryAlloc(int instrument_options);
extern void InstrQueryInitLocal(QueryInstrumentation *qinstr,
								int instrument_options);
extern void InstrQueryStart(QueryInstrumentation *instr);
extern void InstrQueryStop(QueryInstrumentation *instr);
extern void InstrQueryStopFinalize(QueryInstrumentation *instr);
extern void InstrQueryRememberChild(QueryInstrumentation *parent, Instrumentation *instr);
extern QueryInstrumentation *InstrQueryCurrent(void);
extern void InstrQueryTrackNested(QueryInstrumentation *qinstr,
								  MemoryContext nested_cxt);
extern void InstrQuerySetStatement(QueryInstrumentation *qinstr,
								   int64 queryId, const char *query_text);

pg_nodiscard extern QueryInstrumentation *InstrStartParallelQuery(void);
extern void InstrEndParallelQuery(QueryInstrumentation *qinstr, BufferUsage *bufusage, WalUsage *walusage);
extern void InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage);
extern void InstrAccumWorkerUsage(const BufferUsage *bufusage,
								  const WalUsage *walusage);
extern void InstrAccumWorkerNodeUsage(QueryInstrumentation *qinstr,
									  Instrumentation *add);

extern NodeInstrumentation *InstrAllocNode(QueryInstrumentation *qinstr,
										   int instrument_options,
										   bool async_mode, int plan_node_id);
extern void InstrInitNode(NodeInstrumentation *instr, int instrument_options,
						  bool async_mode);
extern void InstrStartNode(NodeInstrumentation *instr);
extern void InstrStopNode(NodeInstrumentation *instr, double nTuples);
extern void InstrUpdateTupleCount(NodeInstrumentation *instr, double nTuples);
extern void InstrEndLoop(NodeInstrumentation *instr);
extern void InstrAggNode(NodeInstrumentation *dst, NodeInstrumentation *add);

extern TriggerInstrumentation *InstrAllocTrigger(QueryInstrumentation *qinstr,
												 int instrument_options, int n);
extern void InstrStartTrigger(TriggerInstrumentation *tginstr);
extern void InstrStopTrigger(TriggerInstrumentation *tginstr, int64 firings);

extern void BufferUsageAdd(BufferUsage *dst, const BufferUsage *add);
extern void WalUsageAdd(WalUsage *dst, const WalUsage *add);
extern void WalUsageAccumDiff(WalUsage *dst, const WalUsage *add,
							  const WalUsage *sub);

#define INSTR_BUFUSAGE_INCR(fld) do { \
		instr_stack.current->bufusage.fld++; \
	} while(0)
#define INSTR_BUFUSAGE_ADD(fld,val) do { \
		instr_stack.current->bufusage.fld += (val); \
	} while(0)
#define INSTR_BUFUSAGE_TIME_ADD(fld,val) do { \
	INSTR_TIME_ADD(instr_stack.current->bufusage.fld, val); \
	} while (0)
#define INSTR_BUFUSAGE_TIME_ACCUM_DIFF(fld,endval,startval) do { \
	INSTR_TIME_ACCUM_DIFF(instr_stack.current->bufusage.fld, endval, startval); \
	} while (0)

#define INSTR_WALUSAGE_INCR(fld) do { \
		pgWalUsage.fld++; \
		instr_stack.current->walusage.fld++; \
	} while(0)
#define INSTR_WALUSAGE_ADD(fld,val) do { \
		pgWalUsage.fld += (val); \
		instr_stack.current->walusage.fld += (val); \
	} while(0)

#endif							/* INSTRUMENT_H */

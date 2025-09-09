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

/* Flag bits included in InstrAlloc's instrument_options bitmask */
typedef enum InstrumentOption
{
	INSTRUMENT_TIMER = 1 << 0,	/* needs timer (and row counts) */
	INSTRUMENT_BUFFERS = 1 << 1,	/* needs buffer usage */
	INSTRUMENT_ROWS = 1 << 2,	/* needs row count */
	INSTRUMENT_WAL = 1 << 3,	/* needs WAL usage */
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
typedef struct Instrumentation
{
	/* Parameters set at creation: */
	bool		need_timer;		/* true if we need timer data */
	bool		need_bufusage;	/* true if we need buffer usage data */
	bool		need_walusage;	/* true if we need WAL usage data */
	/* Internal state keeping: */
	instr_time	starttime;		/* start time of last InstrStart */
	/* Accumulated statistics: */
	instr_time	total;			/* total runtime */
	BufferUsage bufusage;		/* total buffer usage */
	WalUsage	walusage;		/* total WAL usage */
} Instrumentation;

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
 * Uses resource owner mechanism for handling aborts, as such, the caller
 * *must* not exit out of the top level transaction after having called
 * InstrQueryStart, without first calling InstrQueryStop. In the case of a
 * transaction abort, logic equivalent to InstrQueryStop will be called
 * automatically.
 */
struct ResourceOwnerData;
typedef struct QueryInstrumentation
{
	Instrumentation instr;

	/* Resource owner used for cleanup for aborts between InstrStart/InstrStop */
	struct ResourceOwnerData *owner;

	/*
	 * NodeInstrumentation child entries that need to be cleaned up on abort,
	 * since they are not registered as a resource owner themselves.
	 */
	dlist_head	unfinalized_children;	/* head of unfinalized children list */
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

	/* Abort handling */
	dlist_node	unfinalized_node;	/* node in parent's unfinalized list */
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
	int			firings;		/* number of times the instrumented trigger
								 * was fired */
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
 * Care must be taken when utilizing this in the parallel worker context:
 * Parallel workers will report back their instrumentation to the caller,
 * and this gets added to the caller's stack. If this were to be used in the
 * shared memory stats infrastructure it would need to be skipped on parallel
 * workers to avoid double counting.
 */
extern PGDLLIMPORT Instrumentation instr_top;

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
 * Any caller using this directly must manage the passed in entry and call
 * InstrPopStack on its own again, typically by using a PG_FINALLY block to
 * ensure the stack gets reset via InstrPopStack on abort. Use InstrStart
 * instead when you want automatic handling of abort cases using the resource
 * owner infrastructure.
 */
static inline void
InstrPushStack(Instrumentation *instr)
{
	if (unlikely(instr_stack.stack_size == instr_stack.stack_space))
		InstrStackGrow();

	instr_stack.entries[instr_stack.stack_size++] = instr;
	instr_stack.current = instr;
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
}

extern void InstrInitOptions(Instrumentation *instr, int instrument_options);
extern void InstrStart(Instrumentation *instr);
extern void InstrStop(Instrumentation *instr);
extern void InstrStopFinalize(Instrumentation *instr);
extern void InstrAccum(Instrumentation *dst, Instrumentation *add);

extern QueryInstrumentation *InstrQueryAlloc(int instrument_options);
extern void InstrQueryStart(QueryInstrumentation *instr);
extern void InstrQueryStop(QueryInstrumentation *instr);
extern QueryInstrumentation *InstrQueryStopFinalize(QueryInstrumentation *instr);
extern void InstrQueryRememberNode(QueryInstrumentation *parent, NodeInstrumentation *instr);

pg_nodiscard extern QueryInstrumentation *InstrStartParallelQuery(void);
extern void InstrEndParallelQuery(QueryInstrumentation *qinstr, BufferUsage *bufusage, WalUsage *walusage);
extern void InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage);

extern NodeInstrumentation *InstrAllocNode(int instrument_options,
										   bool async_mode);
extern void InstrInitNode(NodeInstrumentation *instr, int instrument_options);
extern void InstrStartNode(NodeInstrumentation *instr);
extern void InstrStopNode(NodeInstrumentation *instr, double nTuples);
extern NodeInstrumentation *InstrFinalizeNode(NodeInstrumentation *instr, Instrumentation *parent);
extern void InstrUpdateTupleCount(NodeInstrumentation *instr, double nTuples);
extern void InstrEndLoop(NodeInstrumentation *instr);
extern void InstrAggNode(NodeInstrumentation *dst, NodeInstrumentation *add);

extern TriggerInstrumentation *InstrAllocTrigger(int n, int instrument_options);
extern void InstrStartTrigger(TriggerInstrumentation *tginstr);
extern void InstrStopTrigger(TriggerInstrumentation *tginstr, int firings);

extern void BufferUsageAdd(BufferUsage *dst, const BufferUsage *add);
extern void WalUsageAdd(WalUsage *dst, const WalUsage *add);
extern void WalUsageAccumDiff(WalUsage *dst, const WalUsage *add,
							  const WalUsage *sub);

#define INSTR_BUFUSAGE_INCR(fld) do { \
		instr_stack.current->bufusage.fld++; \
	} while(0)
#define INSTR_BUFUSAGE_ADD(fld,val) do { \
		instr_stack.current->bufusage.fld += val; \
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
		pgWalUsage.fld += val; \
		instr_stack.current->walusage.fld += val; \
	} while(0)

#endif							/* INSTRUMENT_H */

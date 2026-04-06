/*-------------------------------------------------------------------------
 *
 * explain_state.h
 *	  prototypes for explain_state.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain_state.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_STATE_H
#define EXPLAIN_STATE_H

#include "executor/instrument.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_node.h"
#include "port/pg_bitutils.h"

typedef enum ExplainSerializeOption
{
	EXPLAIN_SERIALIZE_NONE,
	EXPLAIN_SERIALIZE_TEXT,
	EXPLAIN_SERIALIZE_BINARY,
} ExplainSerializeOption;

typedef enum ExplainFormat
{
	EXPLAIN_FORMAT_TEXT,
	EXPLAIN_FORMAT_XML,
	EXPLAIN_FORMAT_JSON,
	EXPLAIN_FORMAT_YAML,
} ExplainFormat;

typedef struct ExplainWorkersState
{
	int			num_workers;	/* # of worker processes the plan used */
	bool	   *worker_inited;	/* per-worker state-initialized flags */
	StringInfoData *worker_str; /* per-worker transient output buffers */
	int		   *worker_state_save;	/* per-worker grouping state save areas */
	StringInfo	prev_str;		/* saved output buffer while redirecting */
} ExplainWorkersState;

/*
 * Instrumentation of a plan node as shown by EXPLAIN.
 *
 * The executor keeps the leader's and each parallel worker's per-node stats
 * apart, and its buffer/WAL counters only cover a node's own activity.
 * ExplainPrintPlan computes one of these per node before printing the plan,
 * combining leader and workers and summing buffer/WAL usage over the subtree.
 */
typedef struct ExplainNodeUsage
{
	bool		valid;			/* entry has been computed */
	NodeInstrumentation stats;	/* leader + workers; buffer/WAL usage in here
								 * is this node's own only */
	BufferUsage bufusage;		/* leader + workers, including children */
	WalUsage	walusage;		/* ditto */
	/* per-worker details, present for nodes run by parallel workers */
	int			num_workers;	/* length of the arrays below, 0 if none */
	BufferUsage *worker_bufusage;	/* including children */
	WalUsage   *worker_walusage;	/* ditto */
} ExplainNodeUsage;

typedef struct ExplainState
{
	StringInfo	str;			/* output buffer */
	/* options */
	bool		verbose;		/* be verbose */
	bool		analyze;		/* print actual times */
	bool		costs;			/* print estimated costs */
	bool		buffers;		/* print buffer usage */
	bool		wal;			/* print WAL usage */
	bool		timing;			/* print detailed node timing */
	bool		summary;		/* print total planning and execution timing */
	bool		memory;			/* print planner's memory usage information */
	bool		settings;		/* print modified settings */
	bool		io;				/* print info about IO (prefetch, ...) */
	bool		generic;		/* generate a generic plan */
	ExplainSerializeOption serialize;	/* serialize the query's output? */
	ExplainFormat format;		/* output format */
	/* state for output formatting --- not reset for each new plan tree */
	int			indent;			/* current indentation level */
	List	   *grouping_stack; /* format-specific grouping state */
	/* state related to the current plan tree (filled by ExplainPrintPlan) */
	PlannedStmt *pstmt;			/* top of plan */
	List	   *rtable;			/* range table */
	List	   *rtable_names;	/* alias names for RTEs */
	List	   *deparse_cxt;	/* context list for deparsing expressions */
	Bitmapset  *printed_subplans;	/* ids of SubPlans we've printed */
	bool		hide_workers;	/* set if we find an invisible Gather */
	int			rtable_size;	/* length of rtable excluding the RTE_GROUP
								 * entry */
	ExplainNodeUsage *node_usage;	/* inclusive per-node buffer/WAL usage,
									 * indexed by plan_node_id; NULL if not
									 * shown */
	int			num_node_usage; /* length of node_usage */
	/* state related to the current plan node */
	ExplainWorkersState *workers_state; /* needed if parallel plan */
	/* extensions */
	void	  **extension_state;
	int			extension_state_allocated;
} ExplainState;

typedef void (*ExplainOptionHandler) (ExplainState *, DefElem *, ParseState *);
typedef bool (*ExplainOptionGUCCheckHandler) (const char *option_name,
											  const char *option_value,
											  NodeTag option_type);

/* Hook to perform additional EXPLAIN options validation */
typedef void (*explain_validate_options_hook_type) (ExplainState *es, List *options,
													ParseState *pstate);
extern PGDLLIMPORT explain_validate_options_hook_type explain_validate_options_hook;

extern ExplainState *NewExplainState(void);
extern void ParseExplainOptionList(ExplainState *es, List *options,
								   ParseState *pstate);

extern int	GetExplainExtensionId(const char *extension_name);
extern void *GetExplainExtensionState(ExplainState *es, int extension_id);
extern void SetExplainExtensionState(ExplainState *es, int extension_id,
									 void *opaque);

extern void RegisterExtensionExplainOption(const char *option_name,
										   ExplainOptionHandler handler,
										   ExplainOptionGUCCheckHandler guc_check_handler);
extern bool ApplyExtensionExplainOption(ExplainState *es, DefElem *opt,
										ParseState *pstate);
extern bool GUCCheckExplainExtensionOption(const char *option_name,
										   const char *option_value,
										   NodeTag option_type);

extern bool GUCCheckBooleanExplainOption(const char *option_name,
										 const char *option_value,
										 NodeTag option_type);

#endif							/* EXPLAIN_STATE_H */

/*-------------------------------------------------------------------------
 *
 * dynamic_explain.h
 *	  prototypes for dynamic_explain.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/dynamic_explain.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_EXPLAIN_H
#define DYNAMIC_EXPLAIN_H

#include "executor/executor.h"
#include "commands/explain_state.h"

extern void HandleLogQueryPlanInterrupt(void);
extern void ProcessLogQueryPlanInterrupt(void);
extern TupleTableSlot *ExecProcNodeWithExplain(PlanState *ps);
extern void LogQueryPlan(void);
extern Datum pg_log_query_plan(PG_FUNCTION_ARGS);

#endif							/* DYNAMIC_EXPLAIN_H */

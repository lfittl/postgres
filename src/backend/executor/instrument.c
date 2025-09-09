/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/executor/instrument.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "executor/instrument.h"
#include "utils/memutils.h"

WalUsage	pgWalUsage;
Instrumentation instr_top;
InstrStackState instr_stack = {0, 0, NULL, &instr_top};

/* List of active QueryInstrumentation entries, for abort cleanup */
static dlist_head active_query_instrumentations = DLIST_STATIC_INIT(active_query_instrumentations);

void
InstrStackGrow(void)
{
	int			space = instr_stack.stack_space;

	if (instr_stack.entries == NULL)
	{
		space = 10;				/* Allocate sufficient initial space for
								 * typical activity */
		instr_stack.entries = MemoryContextAlloc(TopMemoryContext,
												 sizeof(Instrumentation *) * space);
	}
	else
	{
		space *= 2;
		instr_stack.entries = repalloc_array(instr_stack.entries, Instrumentation *, space);
	}

	/* Update stack space after allocation succeeded to protect against OOMs */
	instr_stack.stack_space = space;
}

/* General purpose instrumentation handling */
void
InstrInitOptions(Instrumentation *instr, int instrument_options)
{
	instr->need_stack = (instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_WAL)) != 0;
	instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
}

void
InstrStart(Instrumentation *instr)
{
	if (instr->need_timer)
	{
		if (!INSTR_TIME_IS_ZERO(instr->starttime))
			elog(ERROR, "InstrStart called twice in a row");
		else
			INSTR_TIME_SET_CURRENT(instr->starttime);
	}

	if (instr->need_stack)
		InstrPushStack(instr);
}

static void
InstrStopTimer(Instrumentation *instr)
{
	instr_time	endtime;

	/* let's update the time only if the timer was requested */
	if (INSTR_TIME_IS_ZERO(instr->starttime))
		elog(ERROR, "InstrStop called without start");

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(instr->total, endtime, instr->starttime);

	INSTR_TIME_SET_ZERO(instr->starttime);
}

void
InstrStop(Instrumentation *instr)
{
	if (instr->need_timer)
		InstrStopTimer(instr);

	if (instr->need_stack)
		InstrPopStack(instr);
}

/*
 * Stops instrumentation, finalizes the stack entry and accumulates to its parent.
 *
 * Note that this intentionally allows passing a stack that is not the current
 * top, as can happen with PG_FINALLY or abort cleanup, which don't have a
 * guaranteed cleanup order.
 *
 * We are careful here to achieve two goals:
 *
 * 1) Reset the stack to the parent of whichever of the released stack entries
 *    has the lowest index
 * 2) Accumulate all instrumentation to the currently active instrumentation,
 *    so that callers get a complete picture of activity, even after an abort
 */
void
InstrStopFinalize(Instrumentation *instr)
{
	int			idx = -1;

	for (int i = instr_stack.stack_size - 1; i >= 0; i--)
	{
		if (instr_stack.entries[i] == instr)
		{
			idx = i;
			break;
		}
	}

	if (idx >= 0)
	{
		while (instr_stack.stack_size > idx + 1)
			instr_stack.stack_size--;

		InstrPopStack(instr);
	}

	if (instr->need_timer)
		InstrStopTimer(instr);

	InstrFinalize(instr);
}

void
InstrFinalize(Instrumentation *instr)
{
	if (instr->need_stack)
		InstrAccumStack(instr_stack.current, instr);
}


/* Query instrumentation handling */

/*
 * Finalize a single QueryInstrumentation entry during abort.
 *
 * Accumulates buffer/WAL data from unfinalized children and triggers,
 * resets the instrumentation stack, and removes the entry from the
 * active list. We do NOT pfree any memory here since portal/executor
 * context cleanup will handle that shortly after.
 */
static void
InstrAbortQueryInstrumentation(QueryInstrumentation *qinstr)
{
	dlist_mutable_iter iter;

	/* Accumulate data from all unfinalized child entries. */
	dlist_foreach_modify(iter, &qinstr->unfinalized_children)
	{
		Instrumentation *child = dlist_container(Instrumentation, unfinalized_node, iter.cur);

		InstrAccumStack(&qinstr->instr, child);
	}

	/* Ensure the stack is reset as expected, and we accumulate to the parent */
	InstrStopFinalize(&qinstr->instr);
}

/*
 * Transaction abort cleanup for instrumentation.
 *
 * Called from AbortTransaction before AtAbort_Portals, so that we can
 * recover buffer/WAL statistics from active instrumentation entries before
 * the executor memory contexts are destroyed.
 */
void
AtAbort_Instrumentation(void)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &active_query_instrumentations)
	{
		QueryInstrumentation *qinstr = dlist_container(QueryInstrumentation, active_node, iter.cur);

		InstrAbortQueryInstrumentation(qinstr);
	}

	/* Reset the list — memory will be freed by context cleanup */
	dlist_init(&active_query_instrumentations);
}

/*
 * Subtransaction abort cleanup for instrumentation.
 *
 * Only finalizes entries created at or below the given nesting level.
 */
void
AtSubAbort_Instrumentation(int nestLevel)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &active_query_instrumentations)
	{
		QueryInstrumentation *qinstr = dlist_container(QueryInstrumentation, active_node, iter.cur);

		if (qinstr->nestingLevel < nestLevel)
			continue;

		InstrAbortQueryInstrumentation(qinstr);
		dlist_delete(&qinstr->active_node);
	}
}

QueryInstrumentation *
InstrQueryAlloc(int instrument_options)
{
	QueryInstrumentation *instr;

	instr = palloc0(sizeof(QueryInstrumentation));

	InstrInitOptions(&instr->instr, instrument_options);
	dlist_node_init(&instr->active_node);
	dlist_init(&instr->unfinalized_children);

	return instr;
}

void
InstrQueryStart(QueryInstrumentation *qinstr)
{
	InstrStart(&qinstr->instr);

	if (qinstr->instr.need_stack && dlist_node_is_detached(&qinstr->active_node))
	{
		/*
		 * Must be inside a transaction so that AtAbort_Instrumentation() or
		 * AtSubAbort_Instrumentation() will clean up on abort.
		 */
		Assert(GetCurrentTransactionNestLevel() > 0);

		qinstr->nestingLevel = GetCurrentTransactionNestLevel();
		dlist_push_head(&active_query_instrumentations, &qinstr->active_node);
	}
}

void
InstrQueryStop(QueryInstrumentation *qinstr)
{
	InstrStop(&qinstr->instr);
}

QueryInstrumentation *
InstrQueryStopFinalize(QueryInstrumentation *qinstr)
{
	InstrStopFinalize(&qinstr->instr);

	if (qinstr->instr.need_stack && !dlist_node_is_detached(&qinstr->active_node))
		dlist_delete_thoroughly(&qinstr->active_node);

	return qinstr;
}

/*
 * Register a child Instrumentation entry for abort processing.
 *
 * On abort, AtAbort_Instrumentation will walk the parent's list to recover
 * buffer/WAL data from entries that were never finalized, in order for
 * aggregate totals to be accurate despite the query erroring out.
 */
void
InstrQueryRememberChild(QueryInstrumentation *parent, Instrumentation *child)
{
	if (child->need_stack)
		dlist_push_head(&parent->unfinalized_children, &child->unfinalized_node);
}

/* start instrumentation during parallel executor startup */
QueryInstrumentation *
InstrStartParallelQuery(void)
{
	QueryInstrumentation *qinstr = InstrQueryAlloc(INSTRUMENT_BUFFERS | INSTRUMENT_WAL);

	InstrQueryStart(qinstr);
	return qinstr;
}

/* report usage after parallel executor shutdown */
void
InstrEndParallelQuery(QueryInstrumentation *qinstr, BufferUsage *bufusage, WalUsage *walusage)
{
	qinstr = InstrQueryStopFinalize(qinstr);
	memcpy(bufusage, &qinstr->instr.bufusage, sizeof(BufferUsage));
	memcpy(walusage, &qinstr->instr.walusage, sizeof(WalUsage));
}

/*
 * Accumulate work done by parallel workers in the leader's stats.
 *
 * Note that what gets added here effectively depends on whether per-node
 * instrumentation is active. If it's active the parallel worker intentionally
 * skips ExecFinalizeNodeInstrumentation on executor shutdown, because it would
 * cause double counting. Instead, this only accumulates any extra activity
 * outside of nodes.
 *
 * Otherwise this is responsible for making sure that the complete query
 * activity is accumulated.
 */
void
InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage)
{
	BufferUsageAdd(&instr_stack.current->bufusage, bufusage);
	WalUsageAdd(&instr_stack.current->walusage, walusage);

	WalUsageAdd(&pgWalUsage, walusage);
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(int instrument_options, bool async_mode)
{
	NodeInstrumentation *instr;

	instr = palloc(sizeof(NodeInstrumentation));

	InstrInitNode(instr, instrument_options);
	instr->async_mode = async_mode;

	return instr;
}

/* Initialize a pre-allocated instrumentation structure. */
void
InstrInitNode(NodeInstrumentation *instr, int instrument_options)
{
	memset(instr, 0, sizeof(NodeInstrumentation));
	InstrInitOptions(&instr->instr, instrument_options);
}

/* Entry to a plan node */
void
InstrStartNode(NodeInstrumentation *instr)
{
	InstrStart(&instr->instr);
}


/* Exit from a plan node */
void
InstrStopNode(NodeInstrumentation *instr, double nTuples)
{
	double		save_tuplecount = instr->tuplecount;
	instr_time	endtime;

	/* count the returned tuples */
	instr->tuplecount += nTuples;

	/*
	 * Update the time only if the timer was requested.
	 *
	 * Note this is different from InstrStop because total is only updated in
	 * InstrEndLoop. We need the separate counter variable because we need to
	 * calculate start-up time for the first tuple in each cycle, and then
	 * accumulate it together.
	 */
	if (instr->instr.need_timer)
	{
		if (INSTR_TIME_IS_ZERO(instr->instr.starttime))
			elog(ERROR, "InstrStopNode called without start");

		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_ACCUM_DIFF(instr->counter, endtime, instr->instr.starttime);

		INSTR_TIME_SET_ZERO(instr->instr.starttime);
	}

	/*
	 * Only pop the stack, accumulation runs in
	 * ExecFinalizeNodeInstrumentation
	 */
	if (instr->instr.need_stack)
		InstrPopStack(&instr->instr);

	/* Is this the first tuple of this cycle? */
	if (!instr->running)
	{
		instr->running = true;
		instr->firsttuple = instr->counter;
	}
	else
	{
		/*
		 * In async mode, if the plan node hadn't emitted any tuples before,
		 * this might be the first tuple
		 */
		if (instr->async_mode && save_tuplecount < 1.0)
			instr->firsttuple = instr->counter;
	}
}

NodeInstrumentation *
InstrFinalizeNode(NodeInstrumentation *instr, Instrumentation *parent)
{
	/* If we didn't use stack based instrumentation, nothing to be done */
	if (!instr->instr.need_stack)
		return instr;

	/* Accumulate node's buffer/WAL usage to the parent */
	InstrAccumStack(parent, &instr->instr);

	/* Unregister from query's unfinalized list */
	dlist_delete(&instr->instr.unfinalized_node);

	return instr;
}

/* Update tuple count */
void
InstrUpdateTupleCount(NodeInstrumentation *instr, double nTuples)
{
	/* count the returned tuples */
	instr->tuplecount += nTuples;
}

/* Finish a run cycle for a plan node */
void
InstrEndLoop(NodeInstrumentation *instr)
{
	/* Skip if nothing has happened, or already shut down */
	if (!instr->running)
		return;

	/* Ensure InstrNodeStop was called */
	Assert(INSTR_TIME_IS_ZERO(instr->instr.starttime));

	/* Accumulate per-cycle statistics into totals */
	INSTR_TIME_ADD(instr->startup, instr->firsttuple);
	INSTR_TIME_ADD(instr->instr.total, instr->counter);
	instr->ntuples += instr->tuplecount;
	instr->nloops += 1;

	/* Reset for next cycle (if any) */
	instr->running = false;
	INSTR_TIME_SET_ZERO(instr->instr.starttime);
	INSTR_TIME_SET_ZERO(instr->counter);
	INSTR_TIME_SET_ZERO(instr->firsttuple);
	instr->tuplecount = 0;
}

/* aggregate instrumentation information */
void
InstrAggNode(NodeInstrumentation *dst, NodeInstrumentation *add)
{
	if (!dst->running && add->running)
	{
		dst->running = true;
		dst->firsttuple = add->firsttuple;
	}
	else if (dst->running && add->running &&
			 INSTR_TIME_GT(dst->firsttuple, add->firsttuple))
		dst->firsttuple = add->firsttuple;

	INSTR_TIME_ADD(dst->counter, add->counter);

	dst->tuplecount += add->tuplecount;
	INSTR_TIME_ADD(dst->startup, add->startup);
	INSTR_TIME_ADD(dst->instr.total, add->instr.total);
	dst->ntuples += add->ntuples;
	dst->ntuples2 += add->ntuples2;
	dst->nloops += add->nloops;
	dst->nfiltered1 += add->nfiltered1;
	dst->nfiltered2 += add->nfiltered2;

	/* Add delta of buffer usage since entry to node's totals */
	if (dst->instr.need_stack)
		InstrAccumStack(&dst->instr, &add->instr);
}

/* Trigger instrumentation handling */
TriggerInstrumentation *
InstrAllocTrigger(int n, int instrument_options)
{
	TriggerInstrumentation *tginstr;
	int			i;

	tginstr = palloc0(n * sizeof(TriggerInstrumentation));

	for (i = 0; i < n; i++)
		InstrInitOptions(&tginstr[i].instr, instrument_options);

	return tginstr;
}

void
InstrStartTrigger(QueryInstrumentation *qinstr, TriggerInstrumentation *tginstr)
{
	InstrStart(&tginstr->instr);

	/*
	 * On first call, register with the parent QueryInstrumentation for abort
	 * recovery. The trigger stays on the list for the query lifetime -- on
	 * normal completion ExecFinalizeTriggerInstrumentation handles it, on
	 * abort AtAbort_Instrumentation does.
	 *
	 * We detect first call by checking if the dlist_node is still in its
	 * palloc0-zeroed state (prev == NULL).
	 */
	if (qinstr && tginstr->instr.need_stack &&
		tginstr->instr.unfinalized_node.prev == NULL)
		dlist_push_head(&qinstr->unfinalized_children,
						&tginstr->instr.unfinalized_node);
}

void
InstrStopTrigger(TriggerInstrumentation *tginstr, int firings)
{
	/*
	 * This trigger may be called again, so we don't finalize instrumentation
	 * here. Accumulation to the parent happens at ExecutorFinish through
	 * ExecFinalizeTriggerInstrumentation.
	 */
	InstrStop(&tginstr->instr);
	tginstr->firings += firings;
}

void
InstrAccumStack(Instrumentation *dst, Instrumentation *add)
{
	Assert(dst != NULL);
	Assert(add != NULL);

	BufferUsageAdd(&dst->bufusage, &add->bufusage);
	WalUsageAdd(&dst->walusage, &add->walusage);
}

/* dst += add */
void
BufferUsageAdd(BufferUsage *dst, const BufferUsage *add)
{
	dst->shared_blks_hit += add->shared_blks_hit;
	dst->shared_blks_read += add->shared_blks_read;
	dst->shared_blks_dirtied += add->shared_blks_dirtied;
	dst->shared_blks_written += add->shared_blks_written;
	dst->local_blks_hit += add->local_blks_hit;
	dst->local_blks_read += add->local_blks_read;
	dst->local_blks_dirtied += add->local_blks_dirtied;
	dst->local_blks_written += add->local_blks_written;
	dst->temp_blks_read += add->temp_blks_read;
	dst->temp_blks_written += add->temp_blks_written;
	INSTR_TIME_ADD(dst->shared_blk_read_time, add->shared_blk_read_time);
	INSTR_TIME_ADD(dst->shared_blk_write_time, add->shared_blk_write_time);
	INSTR_TIME_ADD(dst->local_blk_read_time, add->local_blk_read_time);
	INSTR_TIME_ADD(dst->local_blk_write_time, add->local_blk_write_time);
	INSTR_TIME_ADD(dst->temp_blk_read_time, add->temp_blk_read_time);
	INSTR_TIME_ADD(dst->temp_blk_write_time, add->temp_blk_write_time);
}

/* dst += add */
void
WalUsageAdd(WalUsage *dst, const WalUsage *add)
{
	dst->wal_bytes += add->wal_bytes;
	dst->wal_records += add->wal_records;
	dst->wal_fpi += add->wal_fpi;
	dst->wal_fpi_bytes += add->wal_fpi_bytes;
	dst->wal_buffers_full += add->wal_buffers_full;
}

void
WalUsageAccumDiff(WalUsage *dst, const WalUsage *add, const WalUsage *sub)
{
	dst->wal_bytes += add->wal_bytes - sub->wal_bytes;
	dst->wal_records += add->wal_records - sub->wal_records;
	dst->wal_fpi += add->wal_fpi - sub->wal_fpi;
	dst->wal_fpi_bytes += add->wal_fpi_bytes - sub->wal_fpi_bytes;
	dst->wal_buffers_full += add->wal_buffers_full - sub->wal_buffers_full;
}

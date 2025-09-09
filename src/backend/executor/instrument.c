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

#include "executor/instrument.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

WalUsage	pgWalUsage;
Instrumentation TopInstrumentation;
InstrStackState instr_stack = {0, 0, NULL, &TopInstrumentation};

static void InstrFinalizeNodesOnAbort(Instrumentation *instr);

/*
 * Use ResourceOwner mechanism to correctly reset instr_stack on abort.
 */
static void ResOwnerReleaseInstrumentation(Datum res);
static const ResourceOwnerDesc instrumentation_resowner_desc =
{
	.name = "instrumentation",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_INSTRUMENTATION,
	.ReleaseResource = ResOwnerReleaseInstrumentation,
	.DebugPrint = NULL,			/* default message is fine */
};

static inline void
ResourceOwnerRememberInstrumentation(ResourceOwner owner, Instrumentation *instr)
{
	ResourceOwnerRemember(owner, PointerGetDatum(instr), &instrumentation_resowner_desc);
}

static inline void
ResourceOwnerForgetInstrumentation(ResourceOwner owner, Instrumentation *instr)
{
	ResourceOwnerForget(owner, PointerGetDatum(instr), &instrumentation_resowner_desc);
}

static void
ResOwnerReleaseInstrumentation(Datum res)
{
	Instrumentation *instr = (Instrumentation *) DatumGetPointer(res);

	/* Accumulate data from all unfinalized child node entries. */
	InstrFinalizeNodesOnAbort(instr);

	/* Ensure the stack is reset as expected, and we accumulate to the parent */
	InstrPopAndFinalizeStack(instr);

	/* Free the Instrumentation struct now, since InstrStop won't be called */
	pfree(instr);
}

void
InstrStackGrow(void)
{
	if (instr_stack.entries == NULL)
	{
		instr_stack.stack_space = 10;	/* Allocate sufficient initial space
										 * for typical activity */
		instr_stack.entries = MemoryContextAlloc(TopMemoryContext,
												 sizeof(Instrumentation *) * instr_stack.stack_space);
	}
	else
	{
		instr_stack.stack_space *= 2;
		instr_stack.entries = repalloc_array(instr_stack.entries, Instrumentation *, instr_stack.stack_space);
	}
}

/*
 * Pops the stack entry and accumulates to its parent.
 *
 * Note that this intentionally allows passing a stack that is not the current
 * top, as can happen with PG_FINALLY, or resource owners, which don't have a
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
InstrPopAndFinalizeStack(Instrumentation *instr)
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

	InstrAccum(instr_stack.current, instr);
}

/* General purpose instrumentation handling */
Instrumentation *
InstrAlloc(int instrument_options)
{
	/*
	 * Allocate in TopMemoryContext so that the Instrumentation survives
	 * transaction abort — ResourceOwner release needs to access it.
	 */
	Instrumentation *instr = MemoryContextAllocZero(TopMemoryContext, sizeof(Instrumentation));

	InstrInit(instr, instrument_options);
	return instr;
}

void
InstrInit(Instrumentation *instr, int instrument_options)
{
	instr->need_bufusage = (instrument_options & INSTRUMENT_BUFFERS) != 0;
	instr->need_walusage = (instrument_options & INSTRUMENT_WAL) != 0;
	instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
}

static void
InstrStartInternal(Instrumentation *instr, bool use_resowner)
{
	if (instr->need_timer)
	{
		if (!INSTR_TIME_IS_ZERO(instr->starttime))
			elog(ERROR, "InstrStart called twice in a row");
		else
			INSTR_TIME_SET_CURRENT(instr->starttime);
	}

	if (instr->need_bufusage || instr->need_walusage)
	{
		if (use_resowner)
		{
			Assert(CurrentResourceOwner != NULL);
			instr->owner = CurrentResourceOwner;

			ResourceOwnerEnlarge(instr->owner);
			ResourceOwnerRememberInstrumentation(instr->owner, instr);
		}

		InstrPushStack(instr);
	}
}

void
InstrStart(Instrumentation *instr)
{
	InstrStartInternal(instr, true);
}

Instrumentation *
InstrStop(Instrumentation *instr, bool finalize)
{
	instr_time	endtime;

	/* let's update the time only if the timer was requested */
	if (instr->need_timer)
	{
		if (INSTR_TIME_IS_ZERO(instr->starttime))
			elog(ERROR, "InstrStop called without start");

		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_ACCUM_DIFF(instr->total, endtime, instr->starttime);

		INSTR_TIME_SET_ZERO(instr->starttime);
	}

	if (instr->need_bufusage || instr->need_walusage)
	{
		InstrPopStack(instr);

		if (finalize)
			InstrAccum(instr_stack.current, instr);

		Assert(instr->owner != NULL);
		ResourceOwnerForgetInstrumentation(instr->owner, instr);
		instr->owner = NULL;
	}

	if (finalize)
	{
		/*
		 * Copy to the current memory context so the caller doesn't need to
		 * explicitly free the TopMemoryContext allocation.
		 */
		Instrumentation *copy = palloc(sizeof(Instrumentation));

		memcpy(copy, instr, sizeof(Instrumentation));
		pfree(instr);
		return copy;
	}

	return instr;
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(int instrument_options, bool async_mode)
{
	/*
	 * We can utilize TopTransactionContext instead of TopMemoryContext here
	 * because nodes don't get used for utility commands that restart
	 * transactions, which would require a context that survives longer
	 * (EXPLAIN ANALYZE is fine).
	 */
	NodeInstrumentation *instr = MemoryContextAlloc(TopTransactionContext, sizeof(NodeInstrumentation));

	InstrInitNode(instr, instrument_options);
	instr->async_mode = async_mode;

	return instr;
}

/* Initialize a pre-allocated instrumentation structure. */
void
InstrInitNode(NodeInstrumentation *instr, int instrument_options)
{
	memset(instr, 0, sizeof(NodeInstrumentation));
	InstrInit(&instr->instr, instrument_options);
}

/*
 * InstrRememberNode - register a child instrumentation entry for abort
 * processing.
 *
 * On abort, InstrFinalizeNodesOnAbort will walk the parent's list to recover
 * buffer/WAL data from entries that were never finalized, in order for
 * aggregate totals to be accurate despite the query erroring out.
 *
 * The child can either be a NodeInstrumentation's embedded Instrumentation or
 * an additional Instrumentation associated with a node. This must not be
 * called with other (non-node) instrumentation as the child that perform their
 * own cleanup. The parent must be a non-node entry that can handle aborts.
 */
void
InstrRememberNode(Instrumentation *parent, Instrumentation *child)
{
	/*
	 * We do not support nesting, to avoid recursion in
	 * InstrFinalizeNodesOnAbort
	 */
	Assert(parent->unfinalized_node.next == NULL);

	slist_push_head(&parent->unfinalized_children, &child->unfinalized_node);
}

/* Entry to a plan node */
void
InstrStartNode(NodeInstrumentation *instr)
{
	InstrStartInternal(&instr->instr, false);
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
	if (instr->instr.need_bufusage || instr->instr.need_walusage)
		InstrPopStack(&instr->instr);

	/* Is this the first tuple of this cycle? */
	if (!instr->running)
	{
		instr->running = true;
		instr->firsttuple = instr->instr.total;
	}
	else
	{
		/*
		 * In async mode, if the plan node hadn't emitted any tuples before,
		 * this might be the first tuple
		 */
		if (instr->async_mode && save_tuplecount < 1.0)
			instr->firsttuple = instr->instr.total;
	}
}

/* Add per-node instrumentation to the parent and move into per-query memory context */
NodeInstrumentation *
InstrFinalizeNode(NodeInstrumentation *instr, Instrumentation *parent)
{
	NodeInstrumentation *dst = palloc(sizeof(NodeInstrumentation));

	memcpy(dst, instr, sizeof(NodeInstrumentation));
	pfree(instr);

	/* Accumulate node's buffer/WAL usage to the parent */
	if (dst->instr.need_bufusage || dst->instr.need_walusage)
		InstrAccum(parent, &dst->instr);

	return dst;
}

/*
 * InstrFinalizeNodesOnAbort
 *
 * Accumulates unfinalized child per-node entries into the resource owner's
 * instrumentation, and resets the list so a theoretical second call is a safe
 * no-op.
 */
static void
InstrFinalizeNodesOnAbort(Instrumentation *instr)
{
	slist_iter	iter;

	slist_foreach(iter, &instr->unfinalized_children)
	{
		Instrumentation *child = slist_container(Instrumentation, unfinalized_node, iter.cur);

		InstrAccum(instr, child);

		/*
		 * Note we don't free the child here since its usually contained
		 * within NodeInstrumentation and we don't have an easy way to access
		 * that, it will be instead be cleaned up by the transaction ending.
		 */
	}

	slist_init(&instr->unfinalized_children);
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

	if (!INSTR_TIME_IS_ZERO(instr->instr.starttime))
		elog(ERROR, "InstrEndLoop called on running node");

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

	dst->tuplecount += add->tuplecount;
	INSTR_TIME_ADD(dst->startup, add->startup);
	INSTR_TIME_ADD(dst->instr.total, add->instr.total);
	dst->ntuples += add->ntuples;
	dst->ntuples2 += add->ntuples2;
	dst->nloops += add->nloops;
	dst->nfiltered1 += add->nfiltered1;
	dst->nfiltered2 += add->nfiltered2;

	/* Add delta of buffer usage since entry to node's totals */
	if (dst->instr.need_bufusage)
		BufferUsageAdd(&dst->instr.bufusage, &add->instr.bufusage);

	if (dst->instr.need_walusage)
		WalUsageAdd(&dst->instr.walusage, &add->instr.walusage);
}

/* Trigger instrumentation handling */
TriggerInstrumentation *
InstrAllocTrigger(int n, int instrument_options)
{
	TriggerInstrumentation *tginstr = palloc0(n * sizeof(TriggerInstrumentation));
	int			i;

	for (i = 0; i < n; i++)
		InstrInit(&tginstr[i].instr, instrument_options);

	return tginstr;
}

void
InstrStartTrigger(TriggerInstrumentation *tginstr)
{
	InstrStart(&tginstr->instr);
}

void
InstrStopTrigger(TriggerInstrumentation *tginstr, int firings)
{
	/*
	 * This trigger may be called again, so we don't finalize instrumentation
	 * here. Accumulation to the parent happens at ExecutorFinish through
	 * ExecFinalizeTriggerInstrumentation.
	 */
	InstrStop(&tginstr->instr, false);
	tginstr->firings += firings;
}

/* start instrumentation during parallel executor startup */
Instrumentation *
InstrStartParallelQuery(void)
{
	Instrumentation *instr = InstrAlloc(INSTRUMENT_BUFFERS | INSTRUMENT_WAL);

	InstrStart(instr);
	return instr;
}

/* report usage after parallel executor shutdown */
void
InstrEndParallelQuery(Instrumentation *instr, BufferUsage *bufusage, WalUsage *walusage)
{
	instr = InstrStop(instr, true);
	memset(bufusage, 0, sizeof(BufferUsage));
	memcpy(bufusage, &instr->bufusage, sizeof(BufferUsage));
	memset(walusage, 0, sizeof(WalUsage));
	memcpy(walusage, &instr->walusage, sizeof(WalUsage));
}

/*
 * Accumulate work done by parallel workers in the leader's stats.
 *
 * Note that what gets added here effectively depends on whether per-node
 * instrumentation is active. If its active the parallel worker intentionally
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

void
InstrAccum(Instrumentation *dst, Instrumentation *add)
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

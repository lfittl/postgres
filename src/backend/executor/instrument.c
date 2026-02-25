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
InstrStack	TopInstrStack;
InstrStack *CurrentInstrStack = &TopInstrStack;

static void WalUsageAdd(WalUsage *dst, WalUsage *add);

static void InstrFinalizeNodesOnAbort(InstrStack *stack);

/*
 * Use ResourceOwner mechanism to correctly reset CurrentInstrStack on abort.
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
ResourceOwnerRememberInstrStack(ResourceOwner owner, InstrStack *stack)
{
	ResourceOwnerRemember(owner, PointerGetDatum(stack), &instrumentation_resowner_desc);
}

static inline void
ResourceOwnerForgetInstrStack(ResourceOwner owner, InstrStack *stack)
{
	ResourceOwnerForget(owner, PointerGetDatum(stack), &instrumentation_resowner_desc);
}

static bool
StackIsParent(InstrStack *stack, InstrStack *entry)
{
	if (entry->previous == NULL)
		return false;

	if (entry->previous == stack)
		return true;

	return StackIsParent(stack, entry->previous);
}

/*
 * OrphanSkippedStacks
 *
 * Orphans all stack entries from the current stack entry to the provided
 * stack which is assumed to be a parent stack of the current stack, and
 * terminates once the current stack entry has reached the provided stack.
 *
 * This sets previous pointers of intermediate stack entries to NULL, so we
 * don't have to worry about calling StackIsParent with a bad pointer in
 * ResOwnerReleaseInstrumentation.
 *
 * This matters because we may process stack entries out of order in aborts
 * because (1) we might have a mix of ResOwner and PG_FINALLY owned stacks
 * (2) ResOwnerReleaseInstrumentation might be called out of order.
 */
static void
OrphanSkippedStacks(InstrStack *stack)
{
	if (CurrentInstrStack == stack || !StackIsParent(stack, CurrentInstrStack))
		return;

	while (CurrentInstrStack != stack)
	{
		InstrStack *previous = CurrentInstrStack->previous;

		Assert(previous != NULL);
		CurrentInstrStack->previous = NULL;
		CurrentInstrStack = previous;
	}
}

static void
ResOwnerReleaseInstrumentation(Datum res)
{
	InstrStack *stack = (InstrStack *) DatumGetPointer(res);

	/*
	 * Because registered resources are *not* cleaned up in a guaranteed
	 * order, we may get a child context after we've processed the parent.
	 * Thus, we only pop the stack if its not already a parent of the stack
	 * being released. Note that OrphanSkippedStacks may have set our previous
	 * stack entry to NULL, in which case we don't modify the stack either.
	 *
	 * Note that StackIsParent will recurse as needed, so it is inadvisible to
	 * use deeply nested stacks.
	 */
	if (stack->previous && !StackIsParent(CurrentInstrStack, stack))
	{
		OrphanSkippedStacks(stack);
		InstrPopStack(stack);
	}

	/* Accumulate data from all unfinalized child node stacks. */
	InstrFinalizeNodesOnAbort(stack);

	/*
	 * Accumulate the stack associated with the ResOwner to the active stack.
	 *
	 * Note that we intentionally directly add to the current stack instead of
	 * the parent of the stack being released, because this can execute out of
	 * order. Explicit PG_FINALLY blocks might have modified the stack as
	 * well.
	 */
	InstrStackAdd(CurrentInstrStack, stack);

	/* Free the stack entry now since InstrStop won't be called */
	pfree(stack);
}

/*
 * Pops the stack entry and accumulates to its parent.
 *
 * Note that this intentionally allows passing a stack that is not
 * CurrentInstrStack, as can happen with PG_FINALLY, and orphans any
 * intermediate stacks that were skipped.
 */
void
InstrPopAndFinalizeStack(InstrStack *stack)
{
	OrphanSkippedStacks(stack);
	InstrPopStack(stack);
	InstrStackAdd(CurrentInstrStack, stack);
}

/* General purpose instrumentation handling */
Instrumentation *
InstrAlloc(int instrument_options)
{
	Instrumentation *instr;

	/* initialize all fields to zeroes, then modify as needed */
	instr = palloc0(sizeof(Instrumentation));
	if (instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_TIMER | INSTRUMENT_WAL))
	{
		instr->need_bufusage = (instrument_options & INSTRUMENT_BUFFERS) != 0;
		instr->need_walusage = (instrument_options & INSTRUMENT_WAL) != 0;
		instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
	}

	return instr;
}

void
InstrStart(Instrumentation *instr)
{
	if (instr->need_timer &&
		!INSTR_TIME_SET_CURRENT_LAZY(instr->starttime))
		elog(ERROR, "InstrStart called twice in a row");

	if (instr->need_bufusage || instr->need_walusage)
	{
		Assert(CurrentResourceOwner != NULL);
		instr->owner = CurrentResourceOwner;

		/*
		 * Allocate the stack resource in a memory context that survives
		 * during an abort. This will be freed by InstrStop (regular
		 * execution) or ResOwnerReleaseInstrumentation (abort).
		 *
		 * We don't do this in InstrAlloc to avoid allocating when InstrStart
		 * + InstrStop isn't called.
		 */
		if (instr->stack == NULL)
			instr->stack = MemoryContextAllocZero(TopMemoryContext, sizeof(InstrStack));

		ResourceOwnerEnlarge(instr->owner);
		ResourceOwnerRememberInstrStack(instr->owner, instr->stack);

		InstrPushStack(instr->stack);
	}
}

void
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
		InstrPopStack(instr->stack);

		if (finalize)
			InstrStackAdd(CurrentInstrStack, instr->stack);

		Assert(instr->owner != NULL);
		ResourceOwnerForgetInstrStack(instr->owner, instr->stack);
		instr->owner = NULL;

		if (finalize)
		{
			/*
			 * To avoid keeping memory allocated beyond when its needed, copy
			 * the result to the current memory context, and free it in the
			 * transaction context.
			 */
			InstrStack *stack = palloc(sizeof(InstrStack));

			memcpy(stack, instr->stack, sizeof(InstrStack));
			pfree(instr->stack);
			instr->stack = stack;
		}
	}
}

/* Trigger instrumentation handling */
TriggerInstrumentation *
InstrAllocTrigger(int n, int instrument_options)
{
	TriggerInstrumentation *tginstr = palloc0(n * sizeof(TriggerInstrumentation));
	bool		need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
	bool		need_buffers = (instrument_options & INSTRUMENT_BUFFERS) != 0;
	bool		need_wal = (instrument_options & INSTRUMENT_WAL) != 0;
	int			i;

	for (i = 0; i < n; i++)
	{
		tginstr[i].instr.need_timer = need_timer;
		tginstr[i].instr.need_bufusage = need_buffers;
		tginstr[i].instr.need_walusage = need_wal;
	}

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

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(int instrument_options, bool async_mode)
{
	/*
	 * We can utilize TopTransactionContext instead of TopMemoryContext here
	 * (despite the inlined InstrStack in NodeInstrumentation) because nodes
	 * don't get used for utility commands that restart transactions, which
	 * would require a context that survives longer (EXPLAIN ANALYZE is fine).
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
	instr->need_bufusage = (instrument_options & INSTRUMENT_BUFFERS) != 0;
	instr->need_walusage = (instrument_options & INSTRUMENT_WAL) != 0;
	instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
}

/*
 * InstrRememberNodeStack - register a child node stack for abort processing.
 *
 * On abort, InstrFinalizeNodesOnAbort will walk the parent's list to recover
 * buffer/WAL data from stacks that were never finalized, in order for
 * aggregate totals to be accurate despite the query erroring out.
 *
 * The passed in node stack can either be the NodeInstrumentation stack or an
 * additional stack that is associated with a node. This must not be called
 * with other (non-node) instrumentation stacks as the child that perform their
 * own cleanup. The parent must be a non-node stack that can handle aborts.
 */
void
InstrRememberNodeStack(InstrStack *parent, InstrStack *node_stack)
{
	/*
	 * We do not support nesting, to avoid recursion in
	 * InstrFinalizeNodesOnAbort
	 */
	Assert(parent->unfinalized_node.next == NULL);

	slist_push_head(&parent->unfinalized_children, &node_stack->unfinalized_node);
}

/* Entry to a plan node */
void
InstrStartNode(NodeInstrumentation *instr)
{
	if (instr->need_timer &&
		!INSTR_TIME_SET_CURRENT_LAZY(instr->starttime))
		elog(ERROR, "InstrStartNode called twice in a row");

	if (instr->need_bufusage || instr->need_walusage)
	{
		/* Ensure that we always have a parent, even at the top most node */
		Assert(CurrentInstrStack != &TopInstrStack);

		InstrPushStack(&instr->stack);
	}
}

/* Exit from a plan node */
void
InstrStopNode(NodeInstrumentation *instr, double nTuples)
{
	double		save_tuplecount = instr->tuplecount;
	instr_time	endtime;

	/* count the returned tuples */
	instr->tuplecount += nTuples;

	/* let's update the time only if the timer was requested */
	if (instr->need_timer)
	{
		if (INSTR_TIME_IS_ZERO(instr->starttime))
			elog(ERROR, "InstrStopNode called without start");

		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_ACCUM_DIFF(instr->counter, endtime, instr->starttime);

		INSTR_TIME_SET_ZERO(instr->starttime);
	}

	if (instr->need_bufusage || instr->need_walusage)
	{
		/* Ensure that we always have a parent, even at the top most node */
		Assert(instr->stack.previous != NULL);

		/* Adding to parent is handled by ExecFinalizeNodeInstrumentation */
		InstrPopStack(&instr->stack);
	}

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

/* Add per-node instrumentation to the parent and move into per-query memory context */
NodeInstrumentation *
InstrFinalizeNode(NodeInstrumentation *instr, InstrStack *parent)
{
	NodeInstrumentation *dst = palloc(sizeof(NodeInstrumentation));

	memcpy(dst, instr, sizeof(NodeInstrumentation));
	pfree(instr);

	/* Avoid stale pointer references */
	dst->stack.previous = NULL;

	InstrStackAdd(parent, &dst->stack);

	return dst;
}

/*
 * InstrFinalizeNodesOnAbort
 *
 * Accumulates unfinalized child per-node stacks into the resource owner stack,
 * and resets the list so a theoretical second call is a safe no-op.
 */
static void
InstrFinalizeNodesOnAbort(InstrStack *stack)
{
	slist_iter	iter;

	slist_foreach(iter, &stack->unfinalized_children)
	{
		InstrStack *child = slist_container(InstrStack, unfinalized_node, iter.cur);

		InstrStackAdd(stack, child);

		/*
		 * Note we don't free the child here since its usually contained
		 * within NodeInstrumentation and we don't have an easy way to access
		 * that, it will be instead be cleaned up by the transaction ending.
		 */
	}

	slist_init(&stack->unfinalized_children);
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

	if (!INSTR_TIME_IS_ZERO(instr->starttime))
		elog(ERROR, "InstrEndLoop called on running node");

	/* Accumulate per-cycle statistics into totals */
	INSTR_TIME_ADD(instr->startup, instr->firsttuple);
	INSTR_TIME_ADD(instr->total, instr->counter);
	instr->ntuples += instr->tuplecount;
	instr->nloops += 1;

	/* Reset for next cycle (if any) */
	instr->running = false;
	INSTR_TIME_SET_ZERO(instr->starttime);
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
	INSTR_TIME_ADD(dst->total, add->total);
	dst->ntuples += add->ntuples;
	dst->ntuples2 += add->ntuples2;
	dst->nloops += add->nloops;
	dst->nfiltered1 += add->nfiltered1;
	dst->nfiltered2 += add->nfiltered2;

	/* Add delta of buffer usage since entry to node's totals */
	if (dst->need_bufusage)
		BufferUsageAdd(&dst->stack.bufusage, &add->stack.bufusage);

	if (dst->need_walusage)
		WalUsageAdd(&dst->stack.walusage, &add->stack.walusage);
}

/*
 * Allocate an additional InstrStack for a node, e.g. for tracking table
 * buffer usage separately from index buffer usage. Allocated in
 * TopTransactionContext so it survives long enough for abort recovery.
 */
InstrStack *
InstrAllocAdditionalNodeStack(NodeInstrumentation *instr)
{
	if (instr->need_bufusage || instr->need_walusage)
		return MemoryContextAllocZero(TopTransactionContext, sizeof(InstrStack));

	return NULL;
}

void
InstrStartNodeStack(NodeInstrumentation *instr, InstrStack *stack)
{
	if (instr->need_bufusage || instr->need_walusage)
	{
		/* Ensure the executor set up a parent node below the top level stack */
		Assert(CurrentInstrStack != &TopInstrStack);

		InstrPushStack(stack);
	}
}

void
InstrStopNodeStack(NodeInstrumentation *instr, InstrStack *stack)
{
	if (instr->need_bufusage || instr->need_walusage)
	{
		/* Adding to parent is handled by InstrFinalizeAdditionalNodeStack */
		InstrPopStack(stack);
	}
}

/* Add additional node stacks to the parent and move into per-query memory context */
InstrStack *
InstrFinalizeAdditionalNodeStack(InstrStack *stack, NodeInstrumentation *instr)
{
	InstrStack *dst = palloc(sizeof(InstrStack));

	memcpy(dst, stack, sizeof(InstrStack));
	pfree(stack);

	/* Avoid stale pointer references */
	dst->previous = NULL;

	InstrStackAdd(&instr->stack, dst);

	return dst;
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
	InstrStop(instr, true);
	memset(bufusage, 0, sizeof(BufferUsage));
	memcpy(bufusage, &instr->stack->bufusage, sizeof(BufferUsage));
	memset(walusage, 0, sizeof(WalUsage));
	memcpy(walusage, &instr->stack->walusage, sizeof(WalUsage));
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
	BufferUsageAdd(&CurrentInstrStack->bufusage, bufusage);
	WalUsageAdd(&CurrentInstrStack->walusage, walusage);

	WalUsageAdd(&pgWalUsage, walusage);
}

void
InstrStackAdd(InstrStack *dst, InstrStack *add)
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

/* helper functions for WAL usage accumulation */
static void
WalUsageAdd(WalUsage *dst, WalUsage *add)
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

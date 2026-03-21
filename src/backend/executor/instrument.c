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
Instrumentation instr_top;
InstrStackState instr_stack = {0, 0, NULL, &instr_top};

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

static inline void
InstrStartTimer(Instrumentation *instr)
{
	Assert(INSTR_TIME_IS_ZERO(instr->starttime));

	INSTR_TIME_SET_CURRENT(instr->starttime);
}

static inline void
InstrStopTimer(Instrumentation *instr)
{
	instr_time	endtime;

	Assert(!INSTR_TIME_IS_ZERO(instr->starttime));

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(instr->total, endtime, instr->starttime);

	INSTR_TIME_SET_ZERO(instr->starttime);
}

void
InstrStart(Instrumentation *instr)
{
	if (instr->need_timer)
		InstrStartTimer(instr);

	if (instr->need_stack)
		InstrPushStack(instr);
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

	InstrAccumStack(instr_stack.current, instr);
}


/* Query instrumentation handling */

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
ResourceOwnerRememberInstrumentation(ResourceOwner owner, QueryInstrumentation *qinstr)
{
	ResourceOwnerRemember(owner, PointerGetDatum(qinstr), &instrumentation_resowner_desc);
}

static inline void
ResourceOwnerForgetInstrumentation(ResourceOwner owner, QueryInstrumentation *qinstr)
{
	ResourceOwnerForget(owner, PointerGetDatum(qinstr), &instrumentation_resowner_desc);
}

static void
ResOwnerReleaseInstrumentation(Datum res)
{
	QueryInstrumentation *qinstr = (QueryInstrumentation *) DatumGetPointer(res);
	dlist_mutable_iter iter;

	/* Accumulate data from all unfinalized child node entries. */
	dlist_foreach_modify(iter, &qinstr->unfinalized_children)
	{
		NodeInstrumentation *child = dlist_container(NodeInstrumentation, unfinalized_node, iter.cur);

		InstrAccumStack(&qinstr->instr, &child->instr);
	}

	/* Accumulate data from any active trigger instrumentation entries. */
	dlist_foreach_modify(iter, &qinstr->unfinalized_triggers)
	{
		TriggerInstrumentation *tginstr = dlist_container(TriggerInstrumentation, unfinalized_trigger, iter.cur);

		InstrAccumStack(&qinstr->instr, &tginstr->instr);

		/*
		 * We can't pfree tginstr here, since its part of a bigger allocation - we'll instead
		 * let transaction end deal with clean up, and instead just remove the list entry.
		 */
		dlist_delete(&tginstr->unfinalized_trigger);
	}

	/* Ensure the stack is reset as expected, and we accumulate to the parent */
	InstrStopFinalize(&qinstr->instr);

	/* Free QueryInstrumentation now, since InstrStop won't be called */
	pfree(qinstr);
}

QueryInstrumentation *
InstrQueryAlloc(int instrument_options)
{
	QueryInstrumentation *instr;

	/*
	 * If needed, allocate in TopMemoryContext so that the Instrumentation
	 * survives transaction abort — ResourceOwner release needs to access
	 * it.
	 */
	if ((instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_WAL)) != 0 && MemoryContextIsValid(PortalContext))
		instr = MemoryContextAllocZero(PortalContext, sizeof(QueryInstrumentation));
	else
		instr = palloc0(sizeof(QueryInstrumentation));

	InstrInitOptions(&instr->instr, instrument_options);
	dlist_init(&instr->unfinalized_children);
	dlist_init(&instr->unfinalized_triggers);

	return instr;
}

void
InstrQueryStart(QueryInstrumentation *qinstr)
{
	InstrStart(&qinstr->instr);

	if (qinstr->instr.need_stack)
	{
		Assert(CurrentResourceOwner != NULL);
		qinstr->owner = CurrentResourceOwner;

		ResourceOwnerEnlarge(qinstr->owner);
		ResourceOwnerRememberInstrumentation(qinstr->owner, qinstr);
	}
}

void
InstrQueryStop(QueryInstrumentation *qinstr)
{
	InstrStop(&qinstr->instr);

	if (qinstr->instr.need_stack)
	{
		Assert(qinstr->owner != NULL);
		ResourceOwnerForgetInstrumentation(qinstr->owner, qinstr);
		qinstr->owner = NULL;
	}
}

QueryInstrumentation *
InstrQueryStopFinalize(QueryInstrumentation *qinstr)
{
	QueryInstrumentation *copy;

	InstrStopFinalize(&qinstr->instr);

	if (!qinstr->instr.need_stack)
		return qinstr;

	Assert(qinstr->owner != NULL);
	ResourceOwnerForgetInstrumentation(qinstr->owner, qinstr);
	qinstr->owner = NULL;

	/*
	 * Copy to the current memory context so the caller doesn't need to
	 * explicitly free the TopMemoryContext allocation.
	 */
	copy = qinstr;
	//copy = palloc(sizeof(QueryInstrumentation));
	//memcpy(copy, qinstr, sizeof(QueryInstrumentation));
	//pfree(qinstr);
	return copy;
}

/*
 * Register a child NodeInstrumentation entry for abort processing.
 *
 * On abort, ResOwnerReleaseInstrumentation will walk the parent's list to
 * recover buffer/WAL data from entries that were never finalized, in order for
 * aggregate totals to be accurate despite the query erroring out.
 */
void
InstrQueryRememberNode(QueryInstrumentation *parent, NodeInstrumentation *child)
{
	if (child->instr.need_stack)
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
InstrEndParallelQuery(QueryInstrumentation *qinstr, Instrumentation *dst)
{
	qinstr = InstrQueryStopFinalize(qinstr);
	memcpy(&dst->bufusage, &qinstr->instr.bufusage, sizeof(BufferUsage));
	memcpy(&dst->walusage, &qinstr->instr.walusage, sizeof(WalUsage));
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
InstrAccumParallelQuery(Instrumentation *instr)
{
	InstrAccumStack(instr_stack.current, instr);

	WalUsageAdd(&pgWalUsage, &instr->walusage);
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(int instrument_options, bool async_mode)
{
	NodeInstrumentation *instr;

	/*
	 * If needed, allocate in a context that supports stack-based
	 * instrumentation abort handling. We use PortalContext because it
	 * survives transaction abort (needed for abort recovery) and gets
	 * cleaned up when the portal is destroyed.
	 */
	if ((instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_WAL)) != 0 && MemoryContextIsValid(PortalContext))
		instr = MemoryContextAlloc(PortalContext, sizeof(NodeInstrumentation));
	else
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

/* Entry to a plan node. If you modify this, check InstrNodeSetupExecProcNode. */
void
InstrStartNode(NodeInstrumentation *instr)
{
	InstrStart(&instr->instr);
}

/*
 * Updates the node instrumentation time counter.
 *
 * Note this is different from InstrStop because total is only updated in
 * InstrEndLoop. We need the separate counter variable because we need to
 * calculate start-up time for the first tuple in each cycle, and then
 * accumulate it together.
 */
static inline void
InstrStopNodeTimer(NodeInstrumentation *instr)
{
	instr_time	endtime;

	Assert(!INSTR_TIME_IS_ZERO(instr->instr.starttime));

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(instr->counter, endtime, instr->instr.starttime);
	INSTR_TIME_SET_ZERO(instr->instr.starttime);

	/*
	 * Is this the first tuple of this cycle?
	 *
	 * In async mode, if the plan node hadn't emitted any tuples before, this
	 * might be the first tuple
	 */
	if (!instr->running || (instr->async_mode && instr->tuplecount < 1.0))
		instr->firsttuple = instr->counter;
}

/* Exit from a plan node. If you modify this, check InstrNodeSetupExecProcNode. */
void
InstrStopNode(NodeInstrumentation *instr, double nTuples)
{
	if (instr->instr.need_timer)
		InstrStopNodeTimer(instr);

	/* Only pop the stack, accumulation runs in InstrFinalizeNode */
	if (instr->instr.need_stack)
		InstrPopStack(&instr->instr);

	instr->running = true;

	/* count the returned tuples */
	instr->tuplecount += nTuples;
}

NodeInstrumentation *
InstrFinalizeNode(NodeInstrumentation *instr, Instrumentation *parent)
{
	NodeInstrumentation *dst;

	/* If we didn't use stack based instrumentation, nothing to be done */
	if (!instr->instr.need_stack)
		return instr;

	/* Copy into per-query memory context */
	dst = instr;
	//dst = palloc(sizeof(NodeInstrumentation));
	//memcpy(dst, instr, sizeof(NodeInstrumentation));

	/* Accumulate node's buffer/WAL usage to the parent */
	InstrAccumStack(parent, &dst->instr);

	/* Unregister from query's unfinalized list before freeing */
	if (instr->instr.need_stack)
		dlist_delete(&instr->unfinalized_node);

	//pfree(instr);

	return dst;
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

/*
 * Specialized handling of instrumented ExecProcNode
 *
 * These functions are equivalent to running ExecProcNodeReal wrapped in
 * InstrStartNode and InstrStopNode, but avoid the conditionals in the hot path
 * by checking the instrumentation options when the ExecProcNode pointer gets
 * first set, and then using a special-purpose function for each. This results
 * in a more optimized set of compiled instructions.
 */

#include "executor/tuptable.h"
#include "nodes/execnodes.h"

/* Simplified pop: restore saved state instead of re-deriving from array */
static inline void
InstrPopStackTo(Instrumentation *prev)
{
	Assert(instr_stack.stack_size > 0);
	Assert(instr_stack.stack_size > 1 ? instr_stack.entries[instr_stack.stack_size - 2] == prev : &instr_top == prev);
	instr_stack.stack_size--;
	instr_stack.current = prev;
}

static TupleTableSlot *
ExecProcNodeInstrFull(PlanState *node)
{
	NodeInstrumentation *instr = node->instrument;
	Instrumentation *prev = instr_stack.current;
	TupleTableSlot *result;

	InstrPushStack(&instr->instr);
	InstrStartTimer(&instr->instr);

	result = node->ExecProcNodeReal(node);

	InstrStopNodeTimer(instr);
	InstrPopStackTo(prev);

	instr->running = true;
	if (!TupIsNull(result))
		instr->tuplecount += 1.0;

	return result;
}

static TupleTableSlot *
ExecProcNodeInstrRowsStackOnly(PlanState *node)
{
	NodeInstrumentation *instr = node->instrument;
	Instrumentation *prev = instr_stack.current;
	TupleTableSlot *result;

	InstrPushStack(&instr->instr);

	result = node->ExecProcNodeReal(node);

	InstrPopStackTo(prev);

	instr->running = true;
	if (!TupIsNull(result))
		instr->tuplecount += 1.0;

	return result;
}

static TupleTableSlot *
ExecProcNodeInstrRowsTimerOnly(PlanState *node)
{
	NodeInstrumentation *instr = node->instrument;
	TupleTableSlot *result;

	InstrStartTimer(&instr->instr);

	result = node->ExecProcNodeReal(node);

	InstrStopNodeTimer(instr);

	instr->running = true;
	if (!TupIsNull(result))
		instr->tuplecount += 1.0;

	return result;
}

static TupleTableSlot *
ExecProcNodeInstrRowsOnly(PlanState *node)
{
	NodeInstrumentation *instr = node->instrument;
	TupleTableSlot *result;

	result = node->ExecProcNodeReal(node);

	instr->running = true;
	if (!TupIsNull(result))
		instr->tuplecount += 1.0;

	return result;
}

/*
 * Returns an ExecProcNode wrapper that performs instrumentation calls,
 * tailored to the instrumentation options enabled for the node.
 */
ExecProcNodeMtd
InstrNodeSetupExecProcNode(NodeInstrumentation *instr)
{
	bool		need_timer = instr->instr.need_timer;
	bool		need_stack = instr->instr.need_stack;

	if (need_timer && need_stack)
		return ExecProcNodeInstrFull;
	else if (need_stack)
		return ExecProcNodeInstrRowsStackOnly;
	else if (need_timer)
		return ExecProcNodeInstrRowsTimerOnly;
	else
		return ExecProcNodeInstrRowsOnly;
}

/* Trigger instrumentation handling */
TriggerInstrumentation *
InstrAllocTrigger(int n, int instrument_options)
{
	TriggerInstrumentation *tginstr;
	int			i;

	/*
	 * If needed, allocate in PortalContext so the memory survives
	 * transaction abort — ResOwnerReleaseInstrumentation needs to access it.
	 */
	if ((instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_WAL)) != 0 && MemoryContextIsValid(PortalContext))
		tginstr = MemoryContextAllocZero(PortalContext,
										 n * sizeof(TriggerInstrumentation));
	else
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
	 * abort ResOwnerReleaseInstrumentation does.
	 *
	 * We detect first call by checking if the dlist_node is still in its
	 * palloc0-zeroed state (prev == NULL).
	 */
	if (qinstr && tginstr->instr.need_stack &&
		tginstr->unfinalized_trigger.prev == NULL)
		dlist_push_head(&qinstr->unfinalized_triggers,
						&tginstr->unfinalized_trigger);
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

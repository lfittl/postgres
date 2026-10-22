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

#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "portability/instr_time.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

WalUsage	pgWalUsage;
Instrumentation instr_top;
InstrStackState instr_stack = {
	.stack_space = 0,
	.stack_size = 0,
	.entries = NULL,
	.current = &instr_top,
};

void
InstrStackGrow(void)
{
	int			space = instr_stack.stack_space;

	Assert(instr_stack.stack_size >= instr_stack.stack_space);

	/*
	 * Growing the stack is the only part of pushing an entry that can fail,
	 * so this allows testing that an entry is cleaned up correctly when it
	 * could not be pushed.
	 */
	INJECTION_POINT("instrumentation-stack-grow", NULL);

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
static inline bool
InstrNeedStack(int instrument_options)
{
	return (instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_WAL)) != 0;
}

void
InstrInitOptions(Instrumentation *instr, int instrument_options)
{
	instr->need_stack = InstrNeedStack(instrument_options);
	instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
}

inline void
InstrStart(Instrumentation *instr)
{
	if (instr->need_timer)
	{
		if (!INSTR_TIME_IS_ZERO(instr->starttime))
			elog(ERROR, "InstrStart called twice in a row");
		else
			INSTR_TIME_SET_CURRENT_FAST(instr->starttime);
	}

	if (instr->need_stack)
		InstrPushStack(instr);
}

/*
 * Helper for InstrStop() and InstrStopNode(), to avoid code duplication
 * despite slightly different needs about how time is accumulated.
 */
static inline void
InstrStopCommon(Instrumentation *instr, instr_time *accum_time)
{
	instr_time	endtime;

	/* update the time only if the timer was requested */
	if (instr->need_timer)
	{
		if (INSTR_TIME_IS_ZERO(instr->starttime))
			elog(ERROR, "InstrStop called without start");

		INSTR_TIME_SET_CURRENT_FAST(endtime);
		INSTR_TIME_ACCUM_DIFF(*accum_time, endtime, instr->starttime);

		INSTR_TIME_SET_ZERO(instr->starttime);
	}

	/* pop the stack, unless InstrStopFinalize previously cleaned up */
	if (instr->on_stack)
		InstrPopStack(instr);
}

void
InstrStop(Instrumentation *instr)
{
	InstrStopCommon(instr, &instr->total);
}

static void InstrQueryFinish(QueryInstrumentation *qinstr, bool allow_stopped);

/*
 * Pop the entries above stack position 'idx', innermost first.
 *
 * These belong to code that has been (or is being) abandoned due to an error,
 * so they will not be stopped normally.  They are children of some query
 * entry, whose total includes them, or query entries, which pass on what they
 * have not passed on yet when their own abort cleanup runs.
 */
static void
InstrUnwindStack(int idx)
{
	while (instr_stack.stack_size > idx + 1)
	{
		int			top = instr_stack.stack_size - 1;

		instr_stack.entries[top]->on_stack = false;
		instr_stack.stack_size--;
		instr_stack.current = instr_stack.entries[top - 1];
	}
}

/*
 * Workhorse for finalizing an entry, normally and in the abort cleanup: pops
 * the entry off the stack, along with whatever is still above it, and stops
 * it.  Accumulating it to its parent is left to the caller.
 *
 * If 'allow_stopped' is set, the entry may already have been stopped (as
 * happens for a QueryInstrumentation between two InstrQueryStart/InstrQueryStop
 * pairs, e.g. between fetches from a cursor), in which case there is no timer
 * to stop and only the accumulation to the parent remains.
 */
static void
InstrStopFinalizeInternal(Instrumentation *instr, bool allow_stopped)
{
	if (instr->on_stack)
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

		if (idx < 0)
			elog(ERROR, "instrumentation entry not found on stack");

		InstrUnwindStack(idx);
	}

	if (!(allow_stopped && instr->need_timer &&
		  INSTR_TIME_IS_ZERO(instr->starttime)))
		InstrStop(instr);
}

/*
 * Stops instrumentation, finalizes the stack entry and accumulates to its parent.
 *
 * Note that this intentionally allows passing a stack that is not the current
 * top, as can happen with PG_FINALLY, or resource owners, which don't have a
 * guaranteed cleanup order.
 */
void
InstrStopFinalize(Instrumentation *instr)
{
	InstrStopFinalizeInternal(instr, false);

	/*
	 * Accumulate all instrumentation to the enclosing instrumentation, so
	 * that callers get a complete picture of activity, even after an abort.
	 */
	InstrAccumStack(instr_stack.current, instr);
}

/* Query instrumentation handling */

/*
 * Pass on to the entry that is now current what a query entry has
 * accumulated since this was last done for it.
 *
 * This runs whenever the entry is stopped, when it is finalized, and in the
 * abort cleanup, as a statement can be run in several steps on behalf of
 * different callers: each fetch from a cursor runs the cursor's query a bit
 * further, and the entry that is current at that time must see the activity
 * that caused, rather than leaving all of it to whichever statement happens
 * to close the cursor.
 *
 * The child entries registered with InstrQueryRememberChild (plan nodes) are
 * never added to the entry's own counters: per-node stats stay exclusive of
 * child nodes, i.e. they only reflect activity that happened while the node
 * itself was the top of the instrumentation stack (which includes statements
 * it ran, e.g. through a function), and EXPLAIN computes the inclusive
 * numbers it displays by summing subtrees at display time.  The total of the
 * statement is therefore computed here, as the entry's own counters plus
 * those of all its children, and remembered in passed_bufusage and
 * passed_walusage, which is what InstrTotalUsage reports.  What is passed on
 * is the difference to the total arrived at last time.  Calling this again
 * without any activity in between passes on nothing, so the normal and the
 * abort path can never count anything twice.
 *
 * This relies on a child's counters only changing while its query entry is
 * running, i.e. on the stack, so that the difference describes what happened
 * since the entry was last started.
 */
static void
InstrQueryPassOnUsage(QueryInstrumentation *qinstr)
{
	BufferUsage bufusage;
	WalUsage	walusage;
	dlist_iter	iter;

	if (!qinstr->instr.need_stack)
		return;

	Assert(!qinstr->instr.on_stack);

	bufusage = qinstr->instr.bufusage;
	walusage = qinstr->instr.walusage;

	dlist_foreach(iter, &qinstr->children)
	{
		Instrumentation *child = dlist_container(Instrumentation, child_node, iter.cur);

		BufferUsageAdd(&bufusage, &child->bufusage);
		WalUsageAdd(&walusage, &child->walusage);
	}

	BufferUsageAccumDiff(&instr_stack.current->bufusage,
						 &bufusage, &qinstr->passed_bufusage);
	WalUsageAccumDiff(&instr_stack.current->walusage,
					  &walusage, &qinstr->passed_walusage);
	qinstr->passed_bufusage = bufusage;
	qinstr->passed_walusage = walusage;
}

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
	MemoryContext instr_cxt = qinstr->instr_cxt;

	/*
	 * Ensure the stack is reset as expected, and pass on what has not been
	 * passed on yet, including what the child entries (plan nodes) did.  The
	 * entry stays registered between InstrQueryStop and the next
	 * InstrQueryStart, so it may well not be running at this point.
	 */
	InstrQueryFinish(qinstr, true);

	/*
	 * Destroy the dedicated instrumentation context, which frees the
	 * QueryInstrumentation and all child allocations.
	 */
	MemoryContextDelete(instr_cxt);
}

QueryInstrumentation *
InstrQueryAlloc(int instrument_options)
{
	QueryInstrumentation *instr;
	MemoryContext instr_cxt;

	/*
	 * When the instrumentation stack is used, create a dedicated memory
	 * context for this query's instrumentation allocations.  It starts out as
	 * a child of the current memory context, so that it is freed along with
	 * the caller's state if the caller is abandoned before InstrQueryStart,
	 * and is moved under TopMemoryContext while a ResourceOwner is
	 * responsible for it (see InstrQueryStart), since the abort cleanup needs
	 * to access it after the caller's memory may already be gone.
	 *
	 * For simpler cases (timer/rows only), use the current memory context.
	 *
	 * All child instrumentation allocations (i.e. plan nodes) must be
	 * allocated within this context to ensure correct clean up on abort.
	 */
	if (InstrNeedStack(instrument_options))
		instr_cxt = AllocSetContextCreate(CurrentMemoryContext,
										  "Instrumentation",
										  ALLOCSET_SMALL_SIZES);
	else
		instr_cxt = CurrentMemoryContext;

	instr = MemoryContextAlloc(instr_cxt, sizeof(QueryInstrumentation));
	InstrQueryInit(instr, instrument_options);
	instr->instr_cxt = instr_cxt;

	return instr;
}

/*
 * Initialize a query entry in caller-provided memory.
 *
 * Unlike an entry from InstrQueryAlloc, which builds on this, such an entry
 * owns no memory context and is not handed to a resource owner by
 * InstrQueryStart, so it is finalized by the caller.  This is meant for
 * measuring a utility statement from the caller's stack frame, since the
 * statement may end the transaction while running and with it any resource
 * owner or transaction-lifetime memory.  The caller must ensure
 * InstrQueryStopFinalize runs even on error, e.g. from a PG_FINALLY block.
 */
void
InstrQueryInit(QueryInstrumentation *qinstr, int instrument_options)
{
	memset(qinstr, 0, sizeof(QueryInstrumentation));
	qinstr->instrument_options = instrument_options;

	InstrInitOptions(&qinstr->instr, instrument_options);
	dlist_init(&qinstr->children);
}

void
InstrQueryStart(QueryInstrumentation *qinstr)
{
	/*
	 * On the first start, hand the entry over to the current ResourceOwner,
	 * which stays responsible for it until InstrQueryStopFinalize (or abort).
	 * It is intentionally not released at InstrQueryStop, so that an abort
	 * between two start/stop pairs (e.g. between fetches from a cursor) still
	 * accumulates what was measured so far.  Entries in caller-provided
	 * memory (see InstrQueryInit) are the caller's responsibility.
	 */
	if (qinstr->instr.need_stack && qinstr->instr_cxt != NULL &&
		qinstr->owner == NULL)
	{
		Assert(CurrentResourceOwner != NULL);

		/*
		 * Enlarge first, as that is the only step that can fail; once the
		 * context is under TopMemoryContext we must be sure to register it.
		 */
		ResourceOwnerEnlarge(CurrentResourceOwner);
		MemoryContextSetParent(qinstr->instr_cxt, TopMemoryContext);

		qinstr->owner = CurrentResourceOwner;
		ResourceOwnerRememberInstrumentation(qinstr->owner, qinstr);
	}

	InstrStart(&qinstr->instr);
}

void
InstrQueryStop(QueryInstrumentation *qinstr)
{
	InstrStop(&qinstr->instr);

	/*
	 * Let the entry that is now current see what was done since the last
	 * start, as the next start might have a different one, see
	 * InstrQueryPassOnUsage.
	 */
	InstrQueryPassOnUsage(qinstr);
}

void
InstrQueryStopFinalize(QueryInstrumentation *qinstr)
{
	InstrQueryFinish(qinstr, false);

	if (!qinstr->instr.need_stack)
	{
		Assert(qinstr->owner == NULL);
		return;
	}

	/* Entries in caller-provided memory have nothing further to release */
	if (qinstr->instr_cxt == NULL)
		return;

	Assert(qinstr->owner != NULL);
	ResourceOwnerForgetInstrumentation(qinstr->owner, qinstr);
	qinstr->owner = NULL;

	/*
	 * Reparent the dedicated instrumentation context under the current memory
	 * context, so that its lifetime is now tied to the caller's context
	 * rather than TopMemoryContext.
	 */
	MemoryContextSetParent(qinstr->instr_cxt, CurrentMemoryContext);
}

/*
 * Finalize a query entry: pop it off the stack along with whatever is still
 * above it, if it is running, and pass on what it has not passed on yet.
 * See InstrStopFinalizeInternal for 'allow_stopped'.
 */
static void
InstrQueryFinish(QueryInstrumentation *qinstr, bool allow_stopped)
{
	InstrStopFinalizeInternal(&qinstr->instr, allow_stopped);

	/*
	 * A query entry has already passed on what it had accumulated each time
	 * it was stopped, so only the remainder is left to pass on.
	 */
	InstrQueryPassOnUsage(qinstr);
}

/*
 * Total buffer/WAL usage of a statement, as of the last time its entry was
 * stopped: its own counters plus those of its child entries, see
 * InstrQueryPassOnUsage.
 */
void
InstrTotalUsage(const QueryInstrumentation *qinstr, BufferUsage *bufusage,
				WalUsage *walusage)
{
	if (bufusage)
		*bufusage = qinstr->passed_bufusage;
	if (walusage)
		*walusage = qinstr->passed_walusage;
}

/*
 * Register a child Instrumentation entry (i.e. a plan node) with its query
 * entry, whose total is computed from the entry's own counters plus those of
 * all its children, see InstrQueryPassOnUsage.  The child must be allocated
 * in the query entry's instr_cxt, as the abort cleanup reads it after the
 * per-query memory may be gone.
 */
void
InstrQueryRememberChild(QueryInstrumentation *parent, Instrumentation *child)
{
	if (child->need_stack)
		dlist_push_head(&parent->children, &child->child_node);
}

/* start instrumentation during parallel worker startup */
QueryInstrumentation *
InstrStartParallelQuery(void)
{
	QueryInstrumentation *qinstr = InstrQueryAlloc(INSTRUMENT_BUFFERS | INSTRUMENT_WAL);

	InstrQueryStart(qinstr);
	return qinstr;
}

/*
 * Report usage after parallel worker shutdown, to be handed to the leader
 * (which adds it to the totals of the statement that launched the workers,
 * see ExecParallelFinish).  The totals include what the worker's plan nodes
 * did.
 */
void
InstrEndParallelQuery(QueryInstrumentation *qinstr, BufferUsage *bufusage, WalUsage *walusage)
{
	InstrQueryStopFinalize(qinstr);
	InstrTotalUsage(qinstr, bufusage, walusage);
}

/*
 * Accumulate work done by parallel workers in the leader's stats, for a
 * statement that has no query entry of its own (see InstrQueryAddWorkerUsage
 * otherwise).
 *
 * The usage goes to whatever entry is current on the stack, e.g. the plan
 * node of an enclosing statement that ran this one through a function, like
 * the statement's own activity, and the WAL usage also to pgWalUsage.
 */
void
InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage)
{
	WalUsageAdd(&pgWalUsage, walusage);

	BufferUsageAdd(&instr_stack.current->bufusage, bufusage);
	WalUsageAdd(&instr_stack.current->walusage, walusage);
}

/*
 * Accumulate work done by the parallel workers of a statement into its query
 * entry.
 *
 * The totals each worker reports cover everything it did, including its plan
 * nodes, whose per-node statistics the leader retrieves for EXPLAIN only (see
 * ExecParallelRetrieveInstrumentation).  The usage goes to the query entry's
 * own counters, and the WAL usage also to pgWalUsage.  It does not go to the
 * current stack entry: while the workers are shut down, that is typically
 * the Gather node, whose numbers EXPLAIN would then show the workers' plan
 * node activity in twice, once through these totals and once through the
 * per-node statistics of the nodes below it.
 *
 * An entry that does not track buffer/WAL usage is not on the stack, and
 * nothing passes on its counters, so the usage goes to the current entry
 * instead, as for a statement without an entry of its own.
 */
void
InstrQueryAddWorkerUsage(QueryInstrumentation *qinstr,
						 const BufferUsage *bufusage, const WalUsage *walusage)
{
	Instrumentation *target = qinstr->instr.need_stack
		? &qinstr->instr
		: instr_stack.current;

	WalUsageAdd(&pgWalUsage, walusage);

	BufferUsageAdd(&target->bufusage, bufusage);
	WalUsageAdd(&target->walusage, walusage);
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(QueryInstrumentation *qinstr, int instrument_options,
			   bool async_mode)
{
	NodeInstrumentation *instr = MemoryContextAlloc(qinstr->instr_cxt, sizeof(NodeInstrumentation));

	InstrInitNode(instr, instrument_options, async_mode);

	InstrQueryRememberChild(qinstr, &instr->instr);

	return instr;
}

/* Initialize a pre-allocated instrumentation structure. */
void
InstrInitNode(NodeInstrumentation *instr, int instrument_options, bool async_mode)
{
	memset(instr, 0, sizeof(NodeInstrumentation));
	InstrInitOptions(&instr->instr, instrument_options);
	instr->async_mode = async_mode;
}

/* Entry to a plan node */
inline void
InstrStartNode(NodeInstrumentation *instr)
{
	InstrStart(&instr->instr);
}


/* Exit from a plan node */
inline void
InstrStopNode(NodeInstrumentation *instr, double nTuples)
{
	double		save_tuplecount = instr->tuplecount;

	/* count the returned tuples */
	instr->tuplecount += nTuples;

	/*
	 * Note that in contrast to InstrStop() the time is accumulated into
	 * NodeInstrumentation->counter, with total only getting updated in
	 * InstrEndLoop.  We need the separate counter variable because we need to
	 * calculate start-up time for the first tuple in each cycle, and then
	 * accumulate it together.
	 */
	InstrStopCommon(&instr->instr, &instr->counter);

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

/*
 * ExecProcNode wrapper that performs instrumentation calls.  By keeping
 * this a separate function, we avoid overhead in the normal case where
 * no instrumentation is wanted.
 *
 * This is implemented in instrument.c as all the functions it calls directly
 * are here, allowing them to be inlined even when not using LTO.
 */
TupleTableSlot *
ExecProcNodeInstr(PlanState *node)
{
	TupleTableSlot *result;

	InstrStartNode(node->instrument);

	result = node->ExecProcNodeReal(node);

	InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

	return result;
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

/*
 * Aggregate node instrumentation, e.g. a parallel worker's stats into its
 * shared memory slot, or for display purposes.  Must be called after
 * InstrEndLoop.
 */
void
InstrAggNode(NodeInstrumentation *dst, NodeInstrumentation *add)
{
	Assert(!add->running);

	INSTR_TIME_ADD(dst->startup, add->startup);
	INSTR_TIME_ADD(dst->instr.total, add->instr.total);
	dst->ntuples += add->ntuples;
	dst->ntuples2 += add->ntuples2;
	dst->nloops += add->nloops;
	dst->nfiltered1 += add->nfiltered1;
	dst->nfiltered2 += add->nfiltered2;

	InstrAccumStack(&dst->instr, &add->instr);
}

/*
 * Trigger instrumentation handling
 *
 * Only the time spent in a trigger and the number of times it fired are
 * tracked per trigger.  A trigger has no stack entry of its own, so its
 * buffer/WAL usage is recorded in whichever entry is current while it runs:
 * the plan node that fired it for BEFORE and INSTEAD OF triggers, and the
 * query entry for AFTER triggers, which run once the plan has completed.
 */
TriggerInstrumentation *
InstrAllocTrigger(int n, int instrument_options)
{
	TriggerInstrumentation *tginstr = palloc0_array(TriggerInstrumentation, n);
	int			i;

	/*
	 * Only request timing: the buffer/WAL usage of a trigger is recorded in
	 * whichever entry is current while it runs.
	 */
	for (i = 0; i < n; i++)
		InstrInitOptions(&tginstr[i].instr,
						 instrument_options & INSTRUMENT_TIMER);

	return tginstr;
}

void
InstrStartTrigger(TriggerInstrumentation *tginstr)
{
	Assert(!tginstr->instr.need_stack);

	InstrStart(&tginstr->instr);
}

void
InstrStopTrigger(TriggerInstrumentation *tginstr, int64 firings)
{
	InstrStop(&tginstr->instr);
	tginstr->firings += firings;
}

void
InstrAccumStack(Instrumentation *dst, Instrumentation *add)
{
	Assert(dst != NULL);
	Assert(add != NULL);

	if (!add->need_stack)
		return;

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

/* dst += add - sub */
inline void
BufferUsageAccumDiff(BufferUsage *dst,
					 const BufferUsage *add,
					 const BufferUsage *sub)
{
	dst->shared_blks_hit += add->shared_blks_hit - sub->shared_blks_hit;
	dst->shared_blks_read += add->shared_blks_read - sub->shared_blks_read;
	dst->shared_blks_dirtied += add->shared_blks_dirtied - sub->shared_blks_dirtied;
	dst->shared_blks_written += add->shared_blks_written - sub->shared_blks_written;
	dst->local_blks_hit += add->local_blks_hit - sub->local_blks_hit;
	dst->local_blks_read += add->local_blks_read - sub->local_blks_read;
	dst->local_blks_dirtied += add->local_blks_dirtied - sub->local_blks_dirtied;
	dst->local_blks_written += add->local_blks_written - sub->local_blks_written;
	dst->temp_blks_read += add->temp_blks_read - sub->temp_blks_read;
	dst->temp_blks_written += add->temp_blks_written - sub->temp_blks_written;
	INSTR_TIME_ACCUM_DIFF(dst->shared_blk_read_time,
						  add->shared_blk_read_time, sub->shared_blk_read_time);
	INSTR_TIME_ACCUM_DIFF(dst->shared_blk_write_time,
						  add->shared_blk_write_time, sub->shared_blk_write_time);
	INSTR_TIME_ACCUM_DIFF(dst->local_blk_read_time,
						  add->local_blk_read_time, sub->local_blk_read_time);
	INSTR_TIME_ACCUM_DIFF(dst->local_blk_write_time,
						  add->local_blk_write_time, sub->local_blk_write_time);
	INSTR_TIME_ACCUM_DIFF(dst->temp_blk_read_time,
						  add->temp_blk_read_time, sub->temp_blk_read_time);
	INSTR_TIME_ACCUM_DIFF(dst->temp_blk_write_time,
						  add->temp_blk_write_time, sub->temp_blk_write_time);
}

/* helper functions for WAL usage accumulation */
void
WalUsageAdd(WalUsage *dst, const WalUsage *add)
{
	dst->wal_bytes += add->wal_bytes;
	dst->wal_records += add->wal_records;
	dst->wal_fpi += add->wal_fpi;
	dst->wal_fpi_bytes += add->wal_fpi_bytes;
	dst->wal_buffers_full += add->wal_buffers_full;
}

inline void
WalUsageAccumDiff(WalUsage *dst, const WalUsage *add, const WalUsage *sub)
{
	dst->wal_bytes += add->wal_bytes - sub->wal_bytes;
	dst->wal_records += add->wal_records - sub->wal_records;
	dst->wal_fpi += add->wal_fpi - sub->wal_fpi;
	dst->wal_fpi_bytes += add->wal_fpi_bytes - sub->wal_fpi_bytes;
	dst->wal_buffers_full += add->wal_buffers_full - sub->wal_buffers_full;
}

/* GUC hooks for timing_clock_source */

bool
check_timing_clock_source(int *newval, void **extra, GucSource source)
{
	/*
	 * Do nothing if timing is not initialized. This is only expected on child
	 * processes in EXEC_BACKEND builds, as GUC hooks can be called during
	 * InitializeGUCOptions() before InitProcessGlobals() has had a chance to
	 * run pg_initialize_timing(). Instead, TSC will be initialized via
	 * restore_backend_variables.
	 */
#ifdef EXEC_BACKEND
	if (!timing_initialized)
		return true;
#else
	Assert(timing_initialized);
#endif

#if PG_INSTR_TSC_CLOCK
	pg_initialize_timing_tsc();

	if (*newval == TIMING_CLOCK_SOURCE_TSC && timing_tsc_frequency_khz <= 0)
	{
		GUC_check_errdetail("TSC is not supported as timing clock source");
		return false;
	}
#endif

	return true;
}

void
assign_timing_clock_source(int newval, void *extra)
{
#ifdef EXEC_BACKEND
	if (!timing_initialized)
		return;
#else
	Assert(timing_initialized);
#endif

	/*
	 * Ignore the return code since the check hook already verified TSC is
	 * usable if it's explicitly requested.
	 */
	pg_set_timing_clock_source(newval);
}

const char *
show_timing_clock_source(void)
{
	switch (timing_clock_source)
	{
		case TIMING_CLOCK_SOURCE_AUTO:
#if PG_INSTR_TSC_CLOCK
			if (pg_current_timing_clock_source() == TIMING_CLOCK_SOURCE_TSC)
				return "auto (tsc)";
#endif
			return "auto (system)";
		case TIMING_CLOCK_SOURCE_SYSTEM:
			return "system";
#if PG_INSTR_TSC_CLOCK
		case TIMING_CLOCK_SOURCE_TSC:
			return "tsc";
#endif
	}

	/* unreachable */
	return "?";
}

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

static void InstrQueryPassOnUsage(QueryInstrumentation *qinstr);

Instrumentation instr_top;
InstrStackState instr_stack = {
	.stack_space = 0,
	.stack_size = 0,
	.entries = NULL,
	.current = &instr_top,
	.current_query = NULL,
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

static inline void
InstrStartTimer(Instrumentation *instr)
{
	if (!INSTR_TIME_IS_ZERO(instr->starttime))
		elog(ERROR, "InstrStart called twice in a row");

	INSTR_TIME_SET_CURRENT_FAST(instr->starttime);
}

static inline void
InstrStopTimer(Instrumentation *instr, instr_time *accum_time)
{
	instr_time	endtime;

	if (INSTR_TIME_IS_ZERO(instr->starttime))
		elog(ERROR, "InstrStop called without start");

	INSTR_TIME_SET_CURRENT_FAST(endtime);
	INSTR_TIME_ACCUM_DIFF(*accum_time, endtime, instr->starttime);

	INSTR_TIME_SET_ZERO(instr->starttime);
}

/*
 * Helper for InstrStop() and InstrStopNode(), to avoid code duplication
 * despite slightly different needs about how time is accumulated.
 */
static inline void
InstrStopCommon(Instrumentation *instr, instr_time *accum_time)
{
	/* update the time only if the timer was requested */
	if (instr->need_timer)
		InstrStopTimer(instr, accum_time);

	/* pop the stack, unless InstrStopFinalize previously cleaned up */
	if (instr->on_stack)
		InstrPopStack(instr);
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
	InstrStopCommon(instr, &instr->total);
}

static void InstrQueryFinish(QueryInstrumentation *qinstr, bool allow_stopped);

/*
 * Pop the entries above stack position 'idx', innermost first.
 *
 * These belong to code that has been (or is being) abandoned due to an error,
 * so they will not be stopped normally.  Running query entries among them,
 * recognizable as the innermost one of the chain of running query entries,
 * pass on what they have not passed on yet right here, while the entry each
 * of them accumulates to is still right below it on the stack, so that what a
 * failed nested statement did stays attributed to the statement that ran it.
 * Leaving that to the abort cleanup would accumulate it to whatever entry
 * survives, since the cleanup releases entries in no particular order and
 * only after we are gone.  When the cleanup does get to them, there is
 * nothing left to pass on.  Other entries are children of some query entry,
 * whose total includes them, so they are simply popped.
 *
 * The entries are still valid at this point: registered query entries live
 * under TopMemoryContext until their cleanup runs, and an entry in
 * caller-provided memory is only ever above us while its own PG_FINALLY is
 * still pending (or at FATAL, when nothing unwinds).
 */
static void
InstrUnwindStack(int idx)
{
	while (instr_stack.stack_size > idx + 1)
	{
		int			top = instr_stack.stack_size - 1;
		QueryInstrumentation *qinstr = instr_stack.current_query;

		if (qinstr != NULL && qinstr->stack_pos == top)
			InstrQueryFinish(qinstr, false);
		else
		{
			instr_stack.entries[top]->on_stack = false;
			instr_stack.stack_size--;
			instr_stack.current = instr_stack.entries[top - 1];
		}
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

/*
 * Finalize whatever is still on the stack into instr_top, at process exit.
 *
 * A FATAL error exits without unwinding the C stack, so the PG_FINALLY blocks
 * that would normally finalize plain Instrumentation entries never run.  Such
 * entries still live in their (abandoned) stack frames, so their activity can
 * be accumulated before the session totals are reported for the last time.
 * QueryInstrumentation entries have been released by the transaction abort
 * that precedes this, except for those in caller-provided memory, and after a
 * clean exit the stack is empty anyway.
 */
void
InstrStackFinalizeAll(void)
{
	while (instr_stack.stack_size > 0)
	{
		int			top = instr_stack.stack_size - 1;
		QueryInstrumentation *qinstr = instr_stack.current_query;

		if (qinstr != NULL && qinstr->stack_pos == top)
			InstrQueryFinish(qinstr, true);
		else
		{
			Instrumentation *instr = instr_stack.entries[top];

			InstrStopFinalizeInternal(instr, true);
			InstrAccumStack(instr_stack.current, instr);
		}
	}
}

/* Query instrumentation handling */

/*
 * Pass on to the enclosing statement what a query entry has accumulated since
 * this was last done for it.
 *
 * This runs whenever the entry is stopped, when it is finalized, and in the
 * abort cleanup, as a statement can be run in several steps on behalf of
 * different callers: each fetch from a cursor runs the cursor's query a bit
 * further, and must see the activity that caused, rather than leaving all of
 * it to whichever statement happens to close the cursor.
 *
 * The child entries registered with InstrQueryRememberChild (plan nodes,
 * triggers) are never added to the entry's own counters: per-node stats stay
 * exclusive of child nodes, i.e. they only reflect activity that happened
 * while the node itself was the top of the instrumentation stack (which
 * includes statements it ran, e.g. through a function), and EXPLAIN computes
 * the inclusive numbers it displays by summing subtrees at display time.  The
 * total of the statement is therefore computed here, as the entry's own
 * counters plus those of all its children, and remembered in passed_usage,
 * which is what InstrTotalUsage reports.  What is passed on is the difference
 * to the total arrived at last time, and goes to the current entry, like the
 * counters of any other entry.  Calling this again without any activity in
 * between passes on nothing, so the normal and the abort path can never count
 * anything twice.
 *
 * This relies on a child's counters only changing while its query entry is
 * running, i.e. on the stack, so that the difference describes what happened
 * since the entry was last started.
 *
 * What parallel workers did on behalf of the statement is not handled here:
 * it reaches the enclosing statements as soon as it is imported, see
 * InstrQueryAddWorkerUsage.
 *
 * The entry must have been popped off the stack, so that the current entry is
 * the one enclosing it.
 */
static void
InstrQueryPassOnUsage(QueryInstrumentation *qinstr)
{
	InstrUsage	usage;
	dlist_iter	iter;

	Assert(!qinstr->instr.on_stack);

	usage.bufusage = qinstr->instr.bufusage;
	usage.walusage = qinstr->instr.walusage;

	dlist_foreach(iter, &qinstr->children)
	{
		Instrumentation *child = dlist_container(Instrumentation, child_node, iter.cur);

		BufferUsageAdd(&usage.bufusage, &child->bufusage);
		WalUsageAdd(&usage.walusage, &child->walusage);
	}

	BufferUsageAccumDiff(&instr_stack.current->bufusage,
						 &usage.bufusage, &qinstr->passed_usage.bufusage);
	WalUsageAccumDiff(&instr_stack.current->walusage,
					  &usage.walusage, &qinstr->passed_usage.walusage);
	qinstr->passed_usage = usage;
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
	 * passed on yet.  The entry stays registered between InstrQueryStop and
	 * the next InstrQueryStart, so it may well not be running at this point,
	 * and if an outer entry already popped it off the stack (see
	 * InstrUnwindStack), there is nothing left to pass on.
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
	 * Create a dedicated memory context for this query's instrumentation
	 * allocations.  It starts out as a child of the current memory context,
	 * so that it is freed along with the caller's state if the caller is
	 * abandoned before InstrQueryStart, and is moved under TopMemoryContext
	 * while a ResourceOwner is responsible for it (see InstrQueryStart),
	 * since the abort cleanup needs to access it after the caller's memory
	 * may already be gone.
	 *
	 * All child instrumentation allocations (plan nodes, triggers) must be
	 * allocated within this context to ensure correct clean up on abort.
	 */
	instr_cxt = AllocSetContextCreate(CurrentMemoryContext,
									  "Instrumentation",
									  ALLOCSET_SMALL_SIZES);

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
 * Such an entry can be the enclosing statement of nested statements and
 * receive worker usage.
 */
void
InstrQueryInit(QueryInstrumentation *qinstr, int instrument_options)
{
	memset(qinstr, 0, sizeof(QueryInstrumentation));
	qinstr->instrument_options = instrument_options;

	InstrInitOptions(&qinstr->instr, instrument_options);

	/*
	 * Query entries always go on the stack, regardless of the options, so
	 * that the statement that is running can always be found (see
	 * InstrQueryCurrent).
	 */
	qinstr->instr.need_stack = true;

	dlist_init(&qinstr->children);
}

/*
 * Total buffer/WAL usage of a statement: what this process did, plus what
 * was imported from parallel workers.
 */
void
InstrTotalUsage(const QueryInstrumentation *qinstr, BufferUsage *bufusage,
				WalUsage *walusage)
{
	/* The process' own total, as of the last stop, see InstrQueryPassOnUsage */
	if (bufusage)
	{
		*bufusage = qinstr->passed_usage.bufusage;
		BufferUsageAdd(bufusage, &qinstr->worker_usage.bufusage);
	}
	if (walusage)
	{
		*walusage = qinstr->passed_usage.walusage;
		WalUsageAdd(walusage, &qinstr->worker_usage.walusage);
	}
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
	if (!qinstr->started)
	{
		if (qinstr->instr_cxt != NULL)
		{
			Assert(CurrentResourceOwner != NULL);
			Assert(qinstr->owner == NULL);

			/*
			 * Enlarge first, as that is the only step that can fail; once the
			 * context is under TopMemoryContext we must be sure to register
			 * it.
			 */
			ResourceOwnerEnlarge(CurrentResourceOwner);
			MemoryContextSetParent(qinstr->instr_cxt, TopMemoryContext);

			qinstr->owner = CurrentResourceOwner;
			ResourceOwnerRememberInstrumentation(qinstr->owner, qinstr);
		}
		qinstr->started = true;
	}

	InstrStart(&qinstr->instr);

	/*
	 * Become the innermost running query entry, remembering the previous one
	 * and where we are on the stack, so that InstrUnwindStack can tell us
	 * apart from other entries without any per-entry marker.
	 */
	Assert(qinstr->instr.on_stack);
	qinstr->parent_query = instr_stack.current_query;
	qinstr->stack_pos = instr_stack.stack_size - 1;
	instr_stack.current_query = qinstr;
}

void
InstrQueryStop(QueryInstrumentation *qinstr)
{
	Assert(instr_stack.current_query == qinstr);

	InstrStop(&qinstr->instr);
	instr_stack.current_query = qinstr->parent_query;

	/*
	 * Let the enclosing entry see what was done since the last start, as the
	 * next start might have a different one, see InstrQueryPassOnUsage.
	 */
	InstrQueryPassOnUsage(qinstr);
}

void
InstrQueryStopFinalize(QueryInstrumentation *qinstr)
{
	InstrQueryFinish(qinstr, false);

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
	bool		running = qinstr->instr.on_stack;

	InstrStopFinalizeInternal(&qinstr->instr, allow_stopped);

	/* Whatever ran within this entry has been popped, see InstrUnwindStack */
	if (running)
	{
		Assert(instr_stack.current_query == qinstr);
		instr_stack.current_query = qinstr->parent_query;
	}

	/*
	 * A query entry has already passed on what it had accumulated each time
	 * it was stopped, so only the remainder is left to pass on, and what
	 * parallel workers did on its behalf takes a separate route to the
	 * enclosing statement, see InstrQueryAddWorkerUsage.
	 */
	InstrQueryPassOnUsage(qinstr);
}

/*
 * The innermost query entry that is currently running, i.e. the entry of the
 * innermost enclosing statement for which instrumentation was requested.
 * Statements that nobody asked to instrument have no entry, so this is NULL
 * outside of any instrumented statement, e.g. in autovacuum, or for a nested
 * statement whose top-level statement is not tracked by anything.
 */
QueryInstrumentation *
InstrQueryCurrent(void)
{
	return instr_stack.current_query;
}

/*
 * Register a child Instrumentation entry (e.g. a plan node) with its
 * query entry, whose total is computed from the entry's own counters plus
 * those of all its children, see InstrQueryPassOnUsage.  The child must be
 * allocated in the query entry's instr_cxt, as the abort cleanup reads it
 * after the per-query memory may be gone.
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
 * Report usage after parallel worker shutdown.
 *
 * The usage is both handed to the leader (which imports it, see
 * InstrAccumParallelQuery) and, through the finalization, added to the
 * worker's own session totals in instr_top, so that each process' instr_top
 * fully accounts for what the process itself did.
 */
void
InstrEndParallelQuery(QueryInstrumentation *qinstr, BufferUsage *bufusage, WalUsage *walusage)
{
	InstrQueryStopFinalize(qinstr);
	memcpy(bufusage, &qinstr->passed_usage.bufusage, sizeof(BufferUsage));
	memcpy(walusage, &qinstr->passed_usage.walusage, sizeof(WalUsage));
}

/*
 * Add what parallel workers did to the worker part of a query entry, and of
 * every query entry enclosing it.
 *
 * The usage only ever goes to the worker parts of query entries, never to the
 * counters of an entry on the stack: those accumulate to the session totals in
 * instr_top eventually, which only ever describe the process' own activity,
 * since each worker reports what it did itself.
 *
 * If the entry is running, the enclosing statements are the chain of running
 * query entries below it, and the usage is added to them right away, rather
 * than being passed on when each entry is stopped like its own activity: each
 * of them sees it as part of its total from then on.  For EXPLAIN, the usage
 * is also attributed to the entry right below each running query entry on the
 * stack, i.e. what ran that statement, if it keeps a worker part for that,
 * which plan nodes and triggers do (see Instrumentation.nested_worker_usage).
 *
 * If the entry is not running, e.g. because the query's executor shutdown and
 * with it the import was deferred until after it was finalized, only the
 * entry itself receives the usage.
 */
static void
InstrAddWorkerUsageToChain(QueryInstrumentation *qinstr,
						   const BufferUsage *bufusage,
						   const WalUsage *walusage)
{
	for (; qinstr != NULL; qinstr = qinstr->parent_query)
	{
		Instrumentation *below;

		BufferUsageAdd(&qinstr->worker_usage.bufusage, bufusage);
		WalUsageAdd(&qinstr->worker_usage.walusage, walusage);

		if (!qinstr->instr.on_stack)
			break;

		below = qinstr->stack_pos > 0
			? instr_stack.entries[qinstr->stack_pos - 1]
			: &instr_top;
		if (below->nested_worker_usage != NULL)
		{
			BufferUsageAdd(&below->nested_worker_usage->bufusage, bufusage);
			WalUsageAdd(&below->nested_worker_usage->walusage, walusage);
		}
	}
}

/*
 * Import work done by parallel workers into the leader's statement-level
 * stats.
 *
 * The usage is the complete activity of a worker, as reported through
 * InstrEndParallelQuery, and goes to the worker part of the query entry of
 * the statement that launched the workers (the executor's es_query_instr),
 * and of the statements enclosing it, see InstrAddWorkerUsageToChain.  Nothing
 * is attributed to a plan node of the statement itself: with per-node
 * instrumentation, the leader shows what the workers did on each node from the
 * per-worker copies it retrieves (see ExecParallelRetrieveInstrumentation),
 * which are also part of these totals, and there is no other node of the
 * statement this activity belongs to.
 */
void
InstrQueryAddWorkerUsage(QueryInstrumentation *qinstr,
						 const BufferUsage *bufusage, const WalUsage *walusage)
{
	if (qinstr == NULL)
		return;

	InstrAddWorkerUsageToChain(qinstr, bufusage, walusage);
}

/*
 * Import work done by parallel workers on behalf of a statement that has no
 * query entry of its own: a query nobody asked to instrument, or a parallel
 * index build or vacuum run by a utility command.
 *
 * The usage goes to the given query entry, which is the innermost one on the
 * stack (InstrQueryCurrent), i.e. the statement the current one is nested in,
 * and to the statements enclosing that, see InstrAddWorkerUsageToChain.  If
 * there is none (e.g. in autovacuum, or when nothing tracks the top-level
 * statement), nobody is interested in the usage and it is dropped: each
 * worker reports its activity to the cumulative statistics system itself.
 *
 * For EXPLAIN, the usage is additionally attributed to the entry that is
 * current on the stack, if it keeps a worker part for that, which plan nodes
 * and triggers do: that is the node or trigger that ran the nested statement
 * whose workers these are, and the entry the statement's own counters go to
 * as well.  If a query or plain entry (e.g. planning) is current, no node or
 * trigger is responsible, matching where those counters go.
 */
void
InstrAccumParallelQuery(QueryInstrumentation *qinstr,
						const BufferUsage *bufusage, const WalUsage *walusage)
{
	if (qinstr == NULL)
		return;

	if (instr_stack.current->nested_worker_usage != NULL)
	{
		InstrUsage *nested = instr_stack.current->nested_worker_usage;

		BufferUsageAdd(&nested->bufusage, bufusage);
		WalUsageAdd(&nested->walusage, walusage);
	}

	InstrAddWorkerUsageToChain(qinstr, bufusage, walusage);
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(QueryInstrumentation *qinstr, int instrument_options,
			   bool async_mode)
{
	NodeInstrumentation *instr = MemoryContextAlloc(qinstr->instr_cxt, sizeof(NodeInstrumentation));

	InstrInitNode(instr, instrument_options, async_mode);

	/*
	 * With buffer/WAL instrumentation, also allocate the node's worker part
	 * (see InstrAccumParallelQuery), up front so that importing worker usage
	 * never needs to allocate.
	 */
	if (instr->instr.need_stack)
		instr->instr.nested_worker_usage =
			MemoryContextAllocZero(qinstr->instr_cxt, sizeof(InstrUsage));

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

/* Entry to a plan node. If you modify this, check InstrNodeSetupExecProcNode. */
void
InstrStartNode(NodeInstrumentation *instr)
{
	InstrStart(&instr->instr);
}

/*
 * Count the tuples returned by a plan node call, and remember the time it
 * took to get to the first tuple of this cycle (which InstrEndLoop turns into
 * the node's startup time).  Must be called after the node's timer has been
 * stopped for this call.
 *
 * Shared by InstrStopNode and the specialized ExecProcNode wrappers below,
 * so that both account in exactly the same way.
 */
static inline void
InstrCountNodeTuples(NodeInstrumentation *instr, double nTuples)
{
	double		save_tuplecount = instr->tuplecount;

	/* count the returned tuples */
	instr->tuplecount += nTuples;

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

/* Exit from a plan node */
void
InstrStopNode(NodeInstrumentation *instr, double nTuples)
{
	/*
	 * Note that in contrast to InstrStop() the time is accumulated into
	 * NodeInstrumentation->counter, with total only getting updated in
	 * InstrEndLoop.  We need the separate counter variable because we need to
	 * calculate start-up time for the first tuple in each cycle, and then
	 * accumulate it together.
	 */
	InstrStopCommon(&instr->instr, &instr->counter);

	InstrCountNodeTuples(instr, nTuples);
}

/*
 * ExecProcNode wrappers that perform instrumentation calls.  By keeping
 * them in separate functions, we avoid overhead in the normal case where
 * no instrumentation is wanted.
 *
 * These functions are equivalent to running ExecProcNodeReal wrapped in
 * InstrStartNode and InstrStopNode, but avoid the conditionals in the hot path
 * by checking the instrumentation options when the ExecProcNode pointer gets
 * first set, and then using a special-purpose function for each. This results
 * in a more optimized set of compiled instructions.
 *
 * This is implemented in instrument.c as all the functions it calls directly
 * are here, allowing them to be inlined even when not using LTO.
 */

/* Simplified pop: restore saved state instead of re-deriving from array */
static inline void
InstrPopStackTo(Instrumentation *prev)
{
	Assert(instr_stack.stack_size > 0);
	Assert(instr_stack.stack_size > 1 ? instr_stack.entries[instr_stack.stack_size - 2] == prev : &instr_top == prev);
	instr_stack.entries[instr_stack.stack_size - 1]->on_stack = false;
	instr_stack.stack_size--;
	instr_stack.current = prev;
}

static pg_always_inline TupleTableSlot *
ExecProcNodeInstr(PlanState *node, bool need_timer, bool need_stack)
{
	NodeInstrumentation *instr = node->instrument;
	Instrumentation *prev = instr_stack.current;
	TupleTableSlot *result;

	if (need_stack)
		InstrPushStack(&instr->instr);
	if (need_timer)
		InstrStartTimer(&instr->instr);

	result = node->ExecProcNodeReal(node);

	if (need_timer)
		InstrStopTimer(&instr->instr, &instr->counter);
	if (need_stack)
		InstrPopStackTo(prev);

	InstrCountNodeTuples(instr, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}

static TupleTableSlot *
ExecProcNodeInstrFull(PlanState *node)
{
	return ExecProcNodeInstr(node, true, true);
}

static TupleTableSlot *
ExecProcNodeInstrRowsStackOnly(PlanState *node)
{
	return ExecProcNodeInstr(node, false, true);
}

static TupleTableSlot *
ExecProcNodeInstrRowsTimerOnly(PlanState *node)
{
	return ExecProcNodeInstr(node, true, false);
}

static TupleTableSlot *
ExecProcNodeInstrRowsOnly(PlanState *node)
{
	return ExecProcNodeInstr(node, false, false);
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
 * Besides the time spent in a trigger and the number of times it fired, the
 * buffer/WAL usage of a trigger is tracked when requested.  For that, the
 * trigger's entry goes on the instrumentation stack while the trigger runs,
 * and is registered as a child of the query entry like a plan node, so that
 * it counts towards the query's total, also if a firing is cut short by an
 * error.  Its counters accumulate across all firings and are what EXPLAIN
 * shows for the trigger; they are not included in the plan node that fired
 * the trigger, which only measures the time spent in it.
 */
TriggerInstrumentation *
InstrAllocTrigger(QueryInstrumentation *qinstr, int n, int instrument_options)
{
	TriggerInstrumentation *tginstr;
	int			i;

	/*
	 * Allocate in the query's dedicated instrumentation context, since the
	 * abort handling accesses registered entries after the per-query memory
	 * may be gone.
	 */
	Assert(qinstr != NULL && qinstr->instr_cxt != NULL);
	tginstr = MemoryContextAllocZero(qinstr->instr_cxt,
									 n * sizeof(TriggerInstrumentation));

	for (i = 0; i < n; i++)
	{
		InstrInitOptions(&tginstr[i].instr, instrument_options);

		/*
		 * Like a plan node, a trigger can run statements that launch parallel
		 * workers, whose usage is shown with the trigger, see
		 * InstrAccumParallelQuery.  Allocate the worker part up front, so
		 * that importing worker usage never needs to allocate.
		 */
		if (tginstr[i].instr.need_stack)
			tginstr[i].instr.nested_worker_usage =
				MemoryContextAllocZero(qinstr->instr_cxt, sizeof(InstrUsage));

		InstrQueryRememberChild(qinstr, &tginstr[i].instr);
	}

	return tginstr;
}

void
InstrStartTrigger(TriggerInstrumentation *tginstr)
{
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
void
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

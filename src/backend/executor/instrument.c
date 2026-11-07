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
#include "utils/memutils.h"
#include "utils/resowner.h"

static void InstrQueryFinalizeTruncated(QueryInstrumentation *qinstr);
static void InstrQueryPassOnWorkerUsage(QueryInstrumentation *qinstr);

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

/*
 * Workhorse for InstrStopFinalize and the abort cleanup.
 *
 * If 'allow_stopped' is set, the entry may already have been stopped (as
 * happens for a QueryInstrumentation between two InstrQueryStart/InstrQueryStop
 * pairs, e.g. between fetches from a cursor), in which case there is no timer
 * to stop and only the accumulation to the parent remains.
 */
static void
InstrStopFinalizeInternal(Instrumentation *instr, bool allow_stopped)
{
	/*
	 * If our current node is on the stack, make sure we reset the stack to
	 * the parent of whichever of the released stack entries has the lowest
	 * index
	 */
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

		/*
		 * Pop the entries above us, innermost first.  These belong to code
		 * that has been (or is being) abandoned due to an error, so they will
		 * not be stopped normally.  Query entries among them are finalized
		 * right here, while the entry each of them accumulates to is still
		 * right below it on the stack, so that what a failed nested statement
		 * did stays attributed to the statement that ran it.  Leaving them to
		 * the abort cleanup would accumulate them to whatever entry survives,
		 * since the cleanup releases entries in no particular order and only
		 * after we are gone.
		 *
		 * The entries are still valid at this point: registered query entries
		 * live under TopMemoryContext until their cleanup runs, and an entry
		 * in caller-provided memory is only ever above us while its own
		 * PG_FINALLY is still pending (or at FATAL, when nothing unwinds).
		 */
		while (instr_stack.stack_size > idx + 1)
		{
			Instrumentation *entry = instr_stack.entries[--instr_stack.stack_size];

			instr_stack.current = instr_stack.entries[instr_stack.stack_size - 1];
			entry->on_stack = false;

			if (entry->kind == INSTR_KIND_QUERY)
				InstrQueryFinalizeTruncated((QueryInstrumentation *) entry);
		}
	}

	if (!(allow_stopped && instr->need_timer &&
		  INSTR_TIME_IS_ZERO(instr->starttime)))
		InstrStop(instr);

	/*
	 * Accumulate all instrumentation to the enclosing instrumentation, so
	 * that callers get a complete picture of activity, even after an abort.
	 *
	 * What parallel workers did on behalf of a statement takes a separate
	 * route to the enclosing statement, see InstrQueryPassOnWorkerUsage.
	 */
	InstrAccumStack(instr_stack.current, instr);

	if (instr->kind == INSTR_KIND_QUERY)
		InstrQueryPassOnWorkerUsage((QueryInstrumentation *) instr);
}

/*
 * Finalize a running query entry that an outer entry just popped off the
 * stack, see InstrStopFinalizeInternal.  This does everything the abort
 * cleanup would, except releasing the memory, which stays with the resource
 * owner (for an entry in caller-provided memory there is nothing to release).
 */
static void
InstrQueryFinalizeTruncated(QueryInstrumentation *qinstr)
{
	Assert(!qinstr->instr.on_stack);
	Assert(!qinstr->instr.finalized);

	InstrQueryFinalizeChildren(qinstr, &qinstr->instr);
	InstrStopFinalizeInternal(&qinstr->instr, false);
	qinstr->instr.finalized = true;
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
}

/*
 * Finalize a child entry registered with InstrQueryRememberChild, by
 * accumulating its buffer/WAL usage to the provided instrumentation, which may
 * be the current entry, or one the caller treats as a parent and will add to
 * the totals later.
 *
 * The entry is removed from its parent's unfinalized list, so that neither
 * InstrQueryFinalizeChildren nor the abort handling count it again, and
 * marked as finalized: later calls are no-ops.  Most children are finalized
 * through InstrQueryFinalizeChildren; a caller that is done with an entry
 * earlier finalizes it directly.
 *
 * For entries that are not registered, e.g. copies of other processes'
 * instrumentation, use InstrAccumStack instead.
 */
void
InstrFinalizeChild(Instrumentation *instr, Instrumentation *parent)
{
	if (!instr->need_stack || instr->finalized)
		return;

	Assert(!dlist_node_is_detached(&instr->unfinalized_entry));
	dlist_delete_thoroughly(&instr->unfinalized_entry);

	InstrAccumStack(parent, instr);
	instr->finalized = true;
}


/* Query instrumentation handling */

/*
 * Finalize all entries registered with InstrQueryRememberChild that have not
 * been finalized yet (plan nodes, triggers, etc.), accumulating each one's
 * buffer/WAL usage directly to the target.
 *
 * The executor calls this at ExecutorFinish with the query entry as the
 * target, and the abort handling does the same, so that nothing registered
 * with a query entry can ever be lost, whichever way the query ends.
 *
 * Children are intentionally not rolled up into their parents: per-node
 * stats stay exclusive of child nodes, i.e. they only reflect activity that
 * happened while the node itself was the top of the instrumentation stack
 * (which includes statements it ran, e.g. through a function).  This keeps
 * the per-node counters identical in the leader and in parallel workers
 * (whose nodes get copied to the leader through shared memory), and makes the
 * normal and the abort path do the same thing.  EXPLAIN computes the
 * inclusive numbers it displays by summing subtrees at display time.
 *
 * In the executor, this must run after ExecShutdownNode has done its work,
 * e.g. parallel worker instrumentation has been retrieved by the leader.
 */
void
InstrQueryFinalizeChildren(QueryInstrumentation *qinstr, Instrumentation *target)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &qinstr->unfinalized_entries)
	{
		Instrumentation *child = dlist_container(Instrumentation, unfinalized_entry, iter.cur);

		InstrFinalizeChild(child, target);
	}
}

/*
 * Pass what parallel workers did on behalf of a finalized query entry on to
 * the statement it is nested in.
 *
 * Unlike the entry's own counters, which go to the current entry like those
 * of any other entry, the worker part must never reach a plan node, trigger
 * or plain entry: those accumulate to the session totals in instr_top
 * eventually, which only ever describe the process' own activity, since each
 * worker reports what it did itself.  So it is imported into the innermost
 * query entry on the stack instead, see InstrAccumParallelQuery, which also
 * takes care of attributing it to the plan node that ran this statement, for
 * EXPLAIN.
 */
static void
InstrQueryPassOnWorkerUsage(QueryInstrumentation *qinstr)
{
	/* Common case: no parallel workers were involved */
	if (!qinstr->has_worker_usage)
		return;

	InstrAccumParallelQuery(InstrQueryCurrent(),
							&qinstr->worker_bufusage, &qinstr->worker_walusage);
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
	 * An entry that an outer entry popped off the stack when it was finalized
	 * has been finalized along with it (see InstrStopFinalizeInternal), and
	 * only its memory remains to be released.
	 */
	if (qinstr->instr.finalized)
		Assert(!qinstr->instr.on_stack);
	else
	{
		/* Accumulate the unfinalized child entries (nodes, triggers) */
		InstrQueryFinalizeChildren(qinstr, &qinstr->instr);

		/*
		 * Ensure the stack is reset as expected, and we accumulate to the
		 * parent.  The entry stays registered between InstrQueryStop and the
		 * next InstrQueryStart, so it may well not be running at this point.
		 */
		InstrStopFinalizeInternal(&qinstr->instr, true);
	}

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
	 * All child instrumentation allocations (nodes, triggers, etc) must be
	 * allocated within this context to ensure correct clean up on abort.
	 */
	instr_cxt = AllocSetContextCreate(CurrentMemoryContext,
									  "Instrumentation",
									  ALLOCSET_SMALL_SIZES);

	instr = MemoryContextAllocZero(instr_cxt, sizeof(QueryInstrumentation));
	instr->instrument_options = instrument_options;
	instr->instr_cxt = instr_cxt;

	InstrInitOptions(&instr->instr, instrument_options);
	instr->instr.kind = INSTR_KIND_QUERY;

	/*
	 * Query entries always go on the stack, regardless of the options, so
	 * that the statement that is running can always be found (see
	 * InstrQueryCurrent).
	 */
	instr->instr.need_stack = true;

	dlist_init(&instr->unfinalized_entries);

	return instr;
}

/*
 * Set up a query entry that lives in caller-provided memory, such as the one
 * embedded in a UtilityStmtDesc, and is finalized by the caller rather than
 * through a resource owner.  The caller must ensure InstrQueryStopFinalize
 * runs even on error, e.g. from a PG_FINALLY block.  Such an entry can be the
 * enclosing statement of nested statements and receive worker usage.
 */
void
InstrQueryInitLocal(QueryInstrumentation *qinstr, int instrument_options)
{
	memset(qinstr, 0, sizeof(QueryInstrumentation));
	qinstr->instrument_options = instrument_options;

	InstrInitOptions(&qinstr->instr, instrument_options);
	qinstr->instr.kind = INSTR_KIND_QUERY;
	qinstr->instr.need_stack = true;

	dlist_init(&qinstr->unfinalized_entries);
}

/*
 * Total buffer/WAL usage of a statement: what this process did, plus what
 * was imported from parallel workers.
 */
void
InstrTotalUsage(const QueryInstrumentation *qinstr, BufferUsage *bufusage,
				WalUsage *walusage)
{
	if (bufusage)
	{
		*bufusage = qinstr->instr.bufusage;
		BufferUsageAdd(bufusage, &qinstr->worker_bufusage);
	}
	if (walusage)
	{
		*walusage = qinstr->instr.walusage;
		WalUsageAdd(walusage, &qinstr->worker_walusage);
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
	 * memory (see InstrQueryInitLocal) are the caller's responsibility.
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
}

void
InstrQueryStop(QueryInstrumentation *qinstr)
{
	InstrStop(&qinstr->instr);
}

void
InstrQueryStopFinalize(QueryInstrumentation *qinstr)
{
	InstrStopFinalize(&qinstr->instr);

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
 * The innermost query entry that is currently running, i.e. the entry of the
 * innermost enclosing statement for which instrumentation was requested.
 * Statements that nobody asked to instrument have no entry, so this is NULL
 * outside of any instrumented statement, e.g. in autovacuum, or for a nested
 * statement whose top-level statement is not tracked by anything.
 */
QueryInstrumentation *
InstrQueryCurrent(void)
{
	for (int i = instr_stack.stack_size - 1; i >= 0; i--)
	{
		Instrumentation *entry = instr_stack.entries[i];

		if (entry->kind == INSTR_KIND_QUERY)
			return (QueryInstrumentation *) entry;
	}

	return NULL;
}

/*
 * Register a child Instrumentation entry for abort processing.
 *
 * On abort, ResOwnerReleaseInstrumentation will walk the parent's list to
 * recover buffer/WAL data from entries that were never finalized, in order for
 * aggregate totals to be accurate despite the query erroring out.
 */
void
InstrQueryRememberChild(QueryInstrumentation *parent, Instrumentation *child)
{
	if (child->need_stack)
		dlist_push_head(&parent->unfinalized_entries, &child->unfinalized_entry);
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
	memcpy(bufusage, &qinstr->instr.bufusage, sizeof(BufferUsage));
	memcpy(walusage, &qinstr->instr.walusage, sizeof(WalUsage));
}

/*
 * Import work done by parallel workers into the leader's statement-level
 * stats.
 *
 * The usage is the complete activity of a worker, as reported through
 * InstrEndParallelQuery, and goes to the worker part of the query entry of
 * the statement that launched the workers (the executor's es_query_instr).
 * From there on it is carried along separately from the leader's own
 * activity, so the session totals in instr_top stay free of it.  Nothing is
 * attributed to a plan node here: with per-node instrumentation, the leader
 * shows what the workers did on each node from the per-worker copies it
 * retrieves (see ExecParallelRetrieveInstrumentation), which are also part of
 * these totals, and there is no other node this activity belongs to.
 */
void
InstrQueryAddWorkerUsage(QueryInstrumentation *qinstr,
						 const BufferUsage *bufusage, const WalUsage *walusage)
{
	if (qinstr == NULL)
		return;

	qinstr->has_worker_usage = true;
	BufferUsageAdd(&qinstr->worker_bufusage, bufusage);
	WalUsageAdd(&qinstr->worker_walusage, walusage);
}

/*
 * Import work done by parallel workers on behalf of a statement that has no
 * query entry of its own: a query nobody asked to instrument, a parallel
 * index build or vacuum run by a utility command, or a query entry passing
 * on its worker part when finalized.
 *
 * The usage goes to the given query entry, which is the innermost one on the
 * stack (InstrQueryCurrent), i.e. the statement the current one is nested in.
 * If there is none (e.g. in autovacuum, or when nothing tracks the top-level
 * statement), nobody is interested in the usage and it is dropped: each
 * worker reports its activity to the cumulative statistics system itself.
 *
 * For EXPLAIN, the usage is additionally attributed to the plan node that is
 * current on the stack, if any, i.e. the node that ran the nested statement
 * whose workers these are.  That is the node the statement's own counters go
 * to as well.  A trigger or plain entry (e.g. planning) being current means
 * no node is responsible, matching where those counters go.  This runs during
 * abort cleanup too, where no memory can be allocated, which is why the
 * node's worker part is allocated up front.
 */
void
InstrAccumParallelQuery(QueryInstrumentation *qinstr,
						const BufferUsage *bufusage, const WalUsage *walusage)
{
	if (qinstr == NULL)
		return;

	InstrQueryAddWorkerUsage(qinstr, bufusage, walusage);

	if (instr_stack.current->kind == INSTR_KIND_NODE)
	{
		NodeInstrumentation *node = (NodeInstrumentation *) instr_stack.current;

		if (node->worker_usage != NULL)
		{
			BufferUsageAdd(&node->worker_usage->bufusage, bufusage);
			WalUsageAdd(&node->worker_usage->walusage, walusage);
		}
	}
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
	 * (see InstrAccumParallelQuery), up front since it may be filled during
	 * abort cleanup, where no memory can be allocated.
	 */
	if (instr->instr.need_stack)
		instr->worker_usage = MemoryContextAllocZero(qinstr->instr_cxt,
													 sizeof(InstrWorkerUsage));

	InstrQueryRememberChild(qinstr, &instr->instr);

	return instr;
}

/* Initialize a pre-allocated instrumentation structure. */
void
InstrInitNode(NodeInstrumentation *instr, int instrument_options, bool async_mode)
{
	memset(instr, 0, sizeof(NodeInstrumentation));
	InstrInitOptions(&instr->instr, instrument_options);
	instr->instr.kind = INSTR_KIND_NODE;
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

/* Trigger instrumentation handling */
TriggerInstrumentation *
InstrAllocTrigger(QueryInstrumentation *qinstr, int instrument_options, int n)
{
	TriggerInstrumentation *tginstr;
	int			i;

	/*
	 * Allocate in the query's dedicated instrumentation context so all
	 * instrumentation data is grouped together and cleaned up as a unit.
	 */
	Assert(qinstr != NULL && qinstr->instr_cxt != NULL);
	tginstr = MemoryContextAllocZero(qinstr->instr_cxt,
									 n * sizeof(TriggerInstrumentation));

	for (i = 0; i < n; i++)
	{
		InstrInitOptions(&tginstr[i].instr, instrument_options);
		tginstr[i].instr.kind = INSTR_KIND_TRIGGER;
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
	/*
	 * This trigger may be called again, so we don't finalize instrumentation
	 * here. Accumulation to the parent happens at ExecutorFinish through
	 * InstrQueryFinalizeChildren.
	 */
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

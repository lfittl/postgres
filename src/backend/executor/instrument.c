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

static void InstrQueryAccumToEnclosing(QueryInstrumentation *qinstr);
static inline void InstrQueryRestoreOrigin(QueryInstrumentation *qinstr);

WalUsage	pgWalUsage;
Instrumentation instr_top;
Instrumentation *instr_current_origin = NULL;
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

		/* Clear on_stack for any intermediate entries we're skipping over */
		for (int i = instr_stack.stack_size - 1; i > idx; i--)
			instr_stack.entries[i]->on_stack = false;

		while (instr_stack.stack_size > idx + 1)
			instr_stack.stack_size--;
	}

	if (!(allow_stopped && instr->need_timer &&
		  INSTR_TIME_IS_ZERO(instr->starttime)))
		InstrStop(instr);

	/*
	 * Accumulate all instrumentation to the enclosing instrumentation, so that
	 * callers get a complete picture of activity, even after an abort.
	 *
	 * A query entry skips over the plan node or trigger entries that ran it,
	 * and may produce a nested statement record, see
	 * InstrQueryAccumToEnclosing.  If it is still running (an abort got here
	 * before InstrQueryStop), the origin it displaced needs to be restored.
	 */
	if (instr->kind == INSTR_KIND_QUERY)
	{
		QueryInstrumentation *qinstr = (QueryInstrumentation *) instr;

		if (qinstr->running)
			InstrQueryRestoreOrigin(qinstr);
		InstrQueryAccumToEnclosing(qinstr);
	}
	else
		InstrAccumStack(instr_stack.current, instr);
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
 * The entry is removed from its parent's unfinalized list, so that the abort
 * handling does not count it again, and marked as finalized: later calls are
 * no-ops.  The latter matters for the plan tree walkers in execProcnode.c,
 * since planstate_tree_walker can reach the same PlanState more than once (a
 * SubPlan appearing in several expressions of a plan node gets one
 * SubPlanState per expression, all pointing at the same subplan PlanState).
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
 * While a statement runs, instr_current_origin points to the plan node or
 * trigger entry of that statement that is executing, so that a statement
 * started from there (e.g. through a function) can be attributed to it.
 * InstrQueryStart displaces it for the statement's own nodes and InstrQueryStop
 * restores it, so an outer statement's nodes never show through an inner one.
 */
static inline void
InstrQueryRestoreOrigin(QueryInstrumentation *qinstr)
{
	instr_current_origin = qinstr->saved_origin;
	qinstr->running = false;
}

/*
 * Find the nearest entry on the stack, searching down from the top, that a
 * query entry accumulates to: another query entry, or a plain one.  Plan node
 * and trigger entries are skipped, so that their stats only ever describe
 * their own activity rather than the statements they ran.  Returns instr_top
 * if there is no such entry.
 */
static Instrumentation *
InstrNearestEnclosingEntry(void)
{
	for (int i = instr_stack.stack_size - 1; i >= 0; i--)
	{
		Instrumentation *entry = instr_stack.entries[i];

		if (entry->kind == INSTR_KIND_QUERY || entry->kind == INSTR_KIND_PLAIN)
			return entry;
	}

	return &instr_top;
}

/*
 * Merge a nested statement record into a list of records, aggregating with an
 * existing record for the same statement and origin if there is one.  The
 * list and any new records live in 'cxt', which belongs to the statement that
 * collects the records; the source may be in memory that is about to go away.
 */
static void
InstrMergeNestedRecord(List **records, MemoryContext cxt,
					   const NestedStatementInstrumentation *src)
{
	NestedStatementInstrumentation *dst = NULL;
	MemoryContext oldcxt;

	foreach_ptr(NestedStatementInstrumentation, rec, *records)
	{
		if (rec->origin_node_id != src->origin_node_id ||
			strcmp(rec->query_text, src->query_text) != 0)
			continue;
		if ((rec->origin_tgname == NULL) != (src->origin_tgname == NULL))
			continue;
		if (rec->origin_tgname != NULL &&
			(strcmp(rec->origin_tgname, src->origin_tgname) != 0 ||
			 strcmp(rec->origin_relname, src->origin_relname) != 0))
			continue;

		dst = rec;
		break;
	}

	oldcxt = MemoryContextSwitchTo(cxt);

	if (dst == NULL)
	{
		dst = palloc0_object(NestedStatementInstrumentation);
		dst->queryId = src->queryId;
		dst->query_text = pstrdup(src->query_text);
		dst->origin_node_id = src->origin_node_id;
		if (src->origin_tgname)
		{
			dst->origin_tgname = pstrdup(src->origin_tgname);
			dst->origin_relname = pstrdup(src->origin_relname);
		}
		*records = lappend(*records, dst);
	}

	dst->calls += src->calls;
	INSTR_TIME_ADD(dst->total, src->total);
	BufferUsageAdd(&dst->bufusage, &src->bufusage);
	WalUsageAdd(&dst->walusage, &src->walusage);

	foreach_ptr(NestedStatementInstrumentation, child, src->nested)
		InstrMergeNestedRecord(&dst->nested, cxt, child);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Accumulate a finalized query entry to its enclosing entry, and record it as
 * a nested statement there if that entry collects such records.
 */
static void
InstrQueryAccumToEnclosing(QueryInstrumentation *qinstr)
{
	Instrumentation *target = InstrNearestEnclosingEntry();
	QueryInstrumentation *enclosing;

	InstrAccumStack(target, &qinstr->instr);

	/*
	 * What parallel workers did on behalf of this statement is only passed
	 * on to an enclosing statement, never to a plain entry or the session
	 * totals.
	 */
	if (target->kind != INSTR_KIND_QUERY)
		return;

	enclosing = (QueryInstrumentation *) target;
	BufferUsageAdd(&enclosing->worker_bufusage, &qinstr->worker_bufusage);
	WalUsageAdd(&enclosing->worker_walusage, &qinstr->worker_walusage);

	if (!enclosing->track_nested || qinstr->query_text == NULL)
		return;

	{
		NestedStatementInstrumentation rec = {0};

		rec.queryId = qinstr->queryId;
		rec.query_text = qinstr->query_text;
		rec.origin_node_id = qinstr->origin_node_id;
		rec.origin_tgname = qinstr->origin_tgname;
		rec.origin_relname = qinstr->origin_relname;
		rec.calls = 1;
		rec.total = qinstr->instr.total;
		InstrTotalUsage(qinstr, &rec.bufusage, &rec.walusage);
		rec.nested = qinstr->nested;

		InstrMergeNestedRecord(&enclosing->nested, enclosing->nested_cxt, &rec);
	}
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
	dlist_mutable_iter iter;

	/* Accumulate data from all unfinalized child entries (nodes, triggers) */
	dlist_foreach_modify(iter, &qinstr->unfinalized_entries)
	{
		Instrumentation *child = dlist_container(Instrumentation, unfinalized_entry, iter.cur);

		InstrAccumStack(&qinstr->instr, child);
	}

	/*
	 * Ensure the stack is reset as expected, and we accumulate to the parent.
	 * The entry stays registered between InstrQueryStop and the next
	 * InstrQueryStart, so it may well not be running at this point.
	 */
	InstrStopFinalizeInternal(&qinstr->instr, true);

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
	 * while a ResourceOwner is responsible for it (see InstrQueryStart), since
	 * the abort cleanup needs to access it after the caller's memory may
	 * already be gone.
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
	instr->origin_node_id = -1;

	InstrInitOptions(&instr->instr, instrument_options);
	instr->instr.kind = INSTR_KIND_QUERY;

	/*
	 * Query entries always go on the stack, regardless of the options, so
	 * that nested statements can find the statement that ran them (see
	 * InstrQueryCurrent and InstrQueryAccumToEnclosing).
	 */
	instr->instr.need_stack = true;

	dlist_init(&instr->unfinalized_entries);

	return instr;
}

/*
 * Set up a query entry that lives in caller-provided memory, typically on the
 * C stack around a utility statement, and is finalized by the caller rather
 * than through a resource owner.  The caller must ensure InstrQueryStopFinalize
 * runs even on error, e.g. from a PG_FINALLY block.  Such an entry can be the
 * enclosing statement of nested statements and receive worker usage; to
 * collect nested statement records it needs to be given memory that outlives
 * it, see InstrQueryTrackNested.
 */
void
InstrQueryInitLocal(QueryInstrumentation *qinstr, int instrument_options)
{
	memset(qinstr, 0, sizeof(QueryInstrumentation));
	qinstr->instrument_options = instrument_options;
	qinstr->origin_node_id = -1;

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
		Instrumentation *origin = instr_current_origin;

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

		/*
		 * Remember where in the enclosing statement this one is run from, for
		 * the nested statement record.  The plan node or trigger that is
		 * current now may be gone by the time the record is made (e.g. a
		 * cursor closed after the statement that opened it finished), so
		 * copy what we need.
		 */
		if (qinstr->query_text != NULL && origin != NULL)
		{
			if (origin->kind == INSTR_KIND_NODE)
				qinstr->origin_node_id = ((NodeInstrumentation *) origin)->plan_node_id;
			else if (origin->kind == INSTR_KIND_TRIGGER)
			{
				TriggerInstrumentation *tginstr = (TriggerInstrumentation *) origin;

				/*
				 * An entry in caller-provided memory finishes within the
				 * enclosing statement, so it can refer to the names directly.
				 */
				if (tginstr->tgname != NULL && qinstr->instr_cxt != NULL)
				{
					qinstr->origin_tgname =
						MemoryContextStrdup(qinstr->instr_cxt, tginstr->tgname);
					qinstr->origin_relname =
						MemoryContextStrdup(qinstr->instr_cxt, tginstr->relname);
				}
				else if (tginstr->tgname != NULL)
				{
					qinstr->origin_tgname = unconstify(char *, tginstr->tgname);
					qinstr->origin_relname = unconstify(char *, tginstr->relname);
				}
			}
		}
	}

	/* This statement's own nodes and triggers are the origin from here on */
	Assert(!qinstr->running);
	qinstr->saved_origin = instr_current_origin;
	instr_current_origin = NULL;
	qinstr->running = true;

	InstrStart(&qinstr->instr);
}

void
InstrQueryStop(QueryInstrumentation *qinstr)
{
	InstrStop(&qinstr->instr);

	Assert(qinstr->running);
	InstrQueryRestoreOrigin(qinstr);
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
 * The innermost query entry that is currently running, or NULL.  While a
 * statement executes, this is the entry of the statement that any statement
 * started now would be nested in.
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
 * Have the entry collect records about the statements run while it executes
 * (see NestedStatementInstrumentation), for EXPLAIN to list.  The records are
 * kept in nested_cxt: the entry's own context, or for an entry in
 * caller-provided memory that of the enclosing statement collecting them,
 * which outlives it.
 */
void
InstrQueryTrackNested(QueryInstrumentation *qinstr, MemoryContext nested_cxt)
{
	Assert(nested_cxt != NULL);

	qinstr->track_nested = true;
	qinstr->nested_cxt = nested_cxt;
}

/*
 * Describe the statement an entry belongs to, so that it can be recorded as a
 * nested statement by the enclosing entry.  Only needed when that one tracks
 * nested statements.  The text is copied, since the caller's copy may be gone
 * by the time the record is made, except for an entry in caller-provided
 * memory, which finishes while the caller's text is still around.
 */
void
InstrQuerySetStatement(QueryInstrumentation *qinstr, int64 queryId,
					   const char *query_text)
{
	qinstr->queryId = queryId;
	if (query_text == NULL)
		return;

	if (qinstr->instr_cxt != NULL)
		qinstr->query_text = MemoryContextStrdup(qinstr->instr_cxt, query_text);
	else
		qinstr->query_text = unconstify(char *, query_text);
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
 * The usage goes to the worker part of the given query entry, typically the
 * innermost one (see InstrQueryCurrent), and is carried along separately from
 * the leader's own activity from there on, so the session totals in instr_top
 * stay free of it.  With no query entry to account it to, nobody is
 * interested in it, and it is dropped: each worker reports its activity to
 * the cumulative statistics system itself.
 *
 * Note that what gets added here effectively depends on whether per-node
 * instrumentation is active. If it's active the parallel worker intentionally
 * does not roll up its nodes into the query-level stats, because the leader
 * imports the per-node stats separately (see InstrAccumWorkerNodeUsage).
 * Instead, this only accumulates any extra activity outside of nodes.
 *
 * Otherwise this is responsible for making sure that the complete query
 * activity is accumulated.
 */
void
InstrAccumParallelQuery(QueryInstrumentation *qinstr,
						const BufferUsage *bufusage, const WalUsage *walusage)
{
	if (qinstr == NULL)
		return;

	BufferUsageAdd(&qinstr->worker_bufusage, bufusage);
	WalUsageAdd(&qinstr->worker_walusage, walusage);
}

/*
 * Import a parallel worker's per-node usage into the leader's query entry.
 *
 * The usage is deliberately kept out of the leader's own per-node entries,
 * which only ever describe the leader's activity; EXPLAIN combines the two
 * from the per-worker copies at display time.
 */
void
InstrAccumWorkerNodeUsage(QueryInstrumentation *qinstr, Instrumentation *add)
{
	Assert(qinstr != NULL);

	InstrAccumParallelQuery(qinstr, &add->bufusage, &add->walusage);
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(QueryInstrumentation *qinstr, int instrument_options,
			   bool async_mode, int plan_node_id)
{
	NodeInstrumentation *instr = MemoryContextAlloc(qinstr->instr_cxt, sizeof(NodeInstrumentation));

	InstrInitNode(instr, instrument_options, async_mode);
	instr->plan_node_id = plan_node_id;

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
	instr->plan_node_id = -1;
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
	Instrumentation *prev_origin = instr_current_origin;
	TupleTableSlot *result;

	/* a statement the node runs is attributed to it, see instr_current_origin */
	instr_current_origin = &instr->instr;
	if (need_stack)
		InstrPushStack(&instr->instr);
	if (need_timer)
		InstrStartTimer(&instr->instr);

	result = node->ExecProcNodeReal(node);

	if (need_timer)
		InstrStopTimer(&instr->instr, &instr->counter);
	if (need_stack)
		InstrPopStackTo(prev);
	instr_current_origin = prev_origin;

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
	/* a statement the trigger runs is attributed to it, see instr_current_origin */
	tginstr->saved_origin = instr_current_origin;
	instr_current_origin = &tginstr->instr;

	InstrStart(&tginstr->instr);
}

void
InstrStopTrigger(TriggerInstrumentation *tginstr, int64 firings)
{
	/*
	 * This trigger may be called again, so we don't finalize instrumentation
	 * here. Accumulation to the parent happens at ExecutorFinish through
	 * ExecFinalizeTriggerInstrumentation.
	 */
	InstrStop(&tginstr->instr);
	tginstr->firings += firings;

	instr_current_origin = tginstr->saved_origin;
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

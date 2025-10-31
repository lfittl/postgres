/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
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
InstrStack *pgInstrStack = NULL;

static void WalUsageAdd(WalUsage *dst, WalUsage *add);

/*
 * Use ResourceOwner mechanism to correctly reset pgInstrStack on abort.
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
ResourceOwnerRememberInstrStack(ResourceOwner owner, InstrStack * stack)
{
	ResourceOwnerRemember(owner, PointerGetDatum(stack), &instrumentation_resowner_desc);
}

static inline void
ResourceOwnerForgetInstrStack(ResourceOwner owner, InstrStack * stack)
{
	ResourceOwnerForget(owner, PointerGetDatum(stack), &instrumentation_resowner_desc);
}

static bool
StackIsParent(InstrStack * stack, InstrStack * entry)
{
	if (entry->previous == NULL)
		return false;

	if (entry->previous == stack)
		return true;

	return StackIsParent(stack, entry->previous);
}

static void
ResOwnerReleaseInstrumentation(Datum res)
{
	InstrStack *stack = (InstrStack *) DatumGetPointer(res);

	if (pgInstrStack)
	{
		/*
		 * Because registered resources are *not* cleaned up in a guaranteed
		 * order, we may get a child context after we've processed the parent.
		 * Thus, we only change the stack if its not already a parent of the
		 * stack being released.
		 *
		 * If we already walked up the stack with an earlier resource, simply
		 * accumulate all collected stats before the abort to the current
		 * stack.
		 *
		 * Note that StackIsParent will recurse as needed, so it is
		 * inadvisible to use deeply nested stacks.
		 */
		if (!StackIsParent(pgInstrStack, stack))
			InstrPopStack(stack, true);
		else
			InstrStackAdd(pgInstrStack, stack);
	}

	/*
	 * Ensure long-lived memory is freed now, as we don't expect InstrStop to
	 * be called
	 */
	pfree(stack);
}

/* General purpose instrumentation handling */
Instrumentation *
InstrAlloc(int n, int instrument_options)
{
	Instrumentation *instr = palloc0(n * sizeof(Instrumentation));
	bool		need_buffers = (instrument_options & INSTRUMENT_BUFFERS) != 0;
	bool		need_wal = (instrument_options & INSTRUMENT_WAL) != 0;
	bool		need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
	int			i;

	for (i = 0; i < n; i++)
	{
		instr[i].need_bufusage = need_buffers;
		instr[i].need_walusage = need_wal;
		instr[i].need_timer = need_timer;
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
		 * We don't do this in InstrAlloc to avoid leaking when InstrStart +
		 * InstrStop isn't called.
		 */
		if (instr->stack == NULL)
			instr->stack = MemoryContextAllocZero(CurTransactionContext, sizeof(InstrStack));

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
		InstrPopStack(instr->stack, finalize);

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
	int			i;

	/*
	 * To avoid having to determine when the last trigger fired, we never
	 * track WAL/buffer usage for now
	 */
	Assert((instrument_options & INSTRUMENT_BUFFERS) == 0);
	Assert((instrument_options & INSTRUMENT_WAL) == 0);

	for (i = 0; i < n; i++)
	{
		tginstr[i].instr.need_bufusage = false;
		tginstr[i].instr.need_walusage = false;
		tginstr[i].instr.need_timer = need_timer;
	}

	return tginstr;
}

void
InstrStartTrigger(TriggerInstrumentation * tginstr)
{
	InstrStart(&tginstr->instr);
}

void
InstrStopTrigger(TriggerInstrumentation * tginstr, int firings)
{
	/*
	 * trigger instrumentation does not track WAL/buffer usage, so its okay to
	 * never finalize
	 */
	InstrStop(&tginstr->instr, false);
	tginstr->firings += firings;
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure(s) */
NodeInstrumentation *
InstrAllocNode(int n, int instrument_options, bool async_mode)
{
	NodeInstrumentation *instr;

	/* initialize all fields to zeroes, then modify as needed */
	instr = palloc0(n * sizeof(NodeInstrumentation));
	if (instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_TIMER | INSTRUMENT_WAL))
	{
		bool		need_buffers = (instrument_options & INSTRUMENT_BUFFERS) != 0;
		bool		need_wal = (instrument_options & INSTRUMENT_WAL) != 0;
		bool		need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
		int			i;

		for (i = 0; i < n; i++)
		{
			instr[i].need_bufusage = need_buffers;
			instr[i].need_walusage = need_wal;
			instr[i].need_timer = need_timer;
			instr[i].async_mode = async_mode;
		}
	}

	return instr;
}

/* Initialize a pre-allocated instrumentation structure. */
void
InstrInitNode(NodeInstrumentation * instr, int instrument_options)
{
	memset(instr, 0, sizeof(NodeInstrumentation));
	instr->need_bufusage = (instrument_options & INSTRUMENT_BUFFERS) != 0;
	instr->need_walusage = (instrument_options & INSTRUMENT_WAL) != 0;
	instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
}

/* Entry to a plan node */
void
InstrStartNode(NodeInstrumentation * instr)
{
	if (instr->need_timer &&
		!INSTR_TIME_SET_CURRENT_LAZY(instr->starttime))
		elog(ERROR, "InstrStartNode called twice in a row");

	if (instr->need_bufusage || instr->need_walusage)
	{
		/* Ensure that we always have a parent, even at the top most node */
		Assert(pgInstrStack != NULL);

		InstrPushStack(&instr->stack);
	}
}

/* Exit from a plan node */
void
InstrStopNode(NodeInstrumentation * instr, double nTuples)
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

		/* Adding to parent is handled by ExecAccumNodeInstrumentation */
		InstrPopStack(&instr->stack, false);
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

/* Update tuple count */
void
InstrUpdateTupleCount(NodeInstrumentation * instr, double nTuples)
{
	/* count the returned tuples */
	instr->tuplecount += nTuples;
}

/* Finish a run cycle for a plan node */
void
InstrEndLoop(NodeInstrumentation * instr)
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
InstrAggNode(NodeInstrumentation * dst, NodeInstrumentation * add)
{
	if (!dst->running && add->running)
	{
		dst->running = true;
		dst->firsttuple = add->firsttuple;
	}
	else if (dst->running && add->running && INSTR_TIME_LT(dst->firsttuple, add->firsttuple))
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

void
InstrStartNodeStack(NodeInstrumentation * instr, InstrStack * stack)
{
	if (instr->need_bufusage || instr->need_walusage)
	{
		/* Ensure that we always have a parent, even at the top most node */
		Assert(pgInstrStack != NULL);

		InstrPushStack(stack);
	}
}

void
InstrStopNodeStack(NodeInstrumentation * instr, InstrStack * stack)
{
	if (instr->need_bufusage || instr->need_walusage)
	{
		/* Ensure that we always have a parent, even at the top most node */
		Assert(stack->previous != NULL);

		/* Adding to parent is handled by ExecAccumNodeInstrumentation */
		InstrPopStack(stack, false);
	}
}

/* start instrumentation during parallel executor startup */
Instrumentation *
InstrStartParallelQuery(void)
{
	Instrumentation *instr = InstrAlloc(1, INSTRUMENT_BUFFERS | INSTRUMENT_WAL);

	InstrStart(instr);
	return instr;
}

/* report usage after parallel executor shutdown */
void
InstrEndParallelQuery(Instrumentation *instr, BufferUsage *bufusage, WalUsage *walusage)
{
	InstrStop(instr, true);
	memcpy(bufusage, &instr->stack->bufusage, sizeof(BufferUsage));
	memcpy(walusage, &instr->stack->walusage, sizeof(WalUsage));
}

/* accumulate work done by workers in leader's stats */
void
InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage)
{
	if (pgInstrStack != NULL)
	{
		InstrStack *dst = pgInstrStack;

		BufferUsageAdd(&dst->bufusage, bufusage);
		WalUsageAdd(&dst->walusage, walusage);
	}

	WalUsageAdd(&pgWalUsage, walusage);
}

void
InstrStackAdd(InstrStack * dst, InstrStack * add)
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

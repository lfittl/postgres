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

BufferUsage pgBufferUsage;
static BufferUsage save_pgBufferUsage;
WalUsage	pgWalUsage;
InstrStack *pgInstrStack = NULL;
static WalUsage save_pgWalUsage;

static void BufferUsageAdd(BufferUsage *dst, const BufferUsage *add);
static void WalUsageAdd(WalUsage *dst, WalUsage *add);

/*
 * Use ResourceOwner mechanism to correctly reset pgInstrStack on abort.
 */
static void ResOwnerReleaseInstrumentation(Datum res);
static const ResourceOwnerDesc instrumentation_resowner_desc =
{
	.name = "instrumentation",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_FIRST,
	.ReleaseResource = ResOwnerReleaseInstrumentation,
	.DebugPrint = NULL,			/* default message is fine */
};

static inline void
ResourceOwnerRememberInstrStack(ResourceOwner owner, Instrumentation *instr)
{
	ResourceOwnerRemember(owner, PointerGetDatum(instr), &instrumentation_resowner_desc);
}

static inline void
ResourceOwnerForgetInstrStack(ResourceOwner owner, Instrumentation *instr)
{
	ResourceOwnerForget(owner, PointerGetDatum(instr), &instrumentation_resowner_desc);
}

static void
InstrPushStackResource(Instrumentation *res)
{
	ResourceOwner owner = CurrentResourceOwner;

	Assert(owner != NULL);

	res->owner = owner;

	ResourceOwnerEnlarge(owner);
	ResourceOwnerRememberInstrStack(owner, res);

	res->stack.previous = pgInstrStack;
	pgInstrStack = &res->stack;
}

static void
InstrPopStackResource(Instrumentation *res)
{
	Assert(res != NULL);
	Assert(res->owner != NULL);

	pgInstrStack = res->stack.previous;

	ResourceOwnerForgetInstrStack(res->owner, res);
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
	Instrumentation *instr = (Instrumentation *) DatumGetPointer(res);

	/*
	 * Because registered resources are *not* called in reverse order, we'll
	 * get what was first registered first at shutdown. Thus, on any later
	 * resources we need to not change the stack, which was already set to the
	 * correct previous entry.
	 */
	if (pgInstrStack && !StackIsParent(pgInstrStack, &instr->stack))
		pgInstrStack = instr->stack.previous;

	/*
	 * Always accumulate all collected stats before the abort, even if we
	 * already walked up the stack with an earlier resource.
	 */
	if (pgInstrStack)
		InstrStackAdd(pgInstrStack, &instr->stack);

	instr->finalized = true;
}

/* General purpose instrumentation handling */
Instrumentation *
InstrAlloc(int n, int instrument_options)
{
	Instrumentation *instr = NULL;
	bool		need_buffers = (instrument_options & INSTRUMENT_BUFFERS) != 0;
	bool		need_wal = (instrument_options & INSTRUMENT_WAL) != 0;
	bool		need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
	int			i;

	/*
	 * If resource owner will be used, we must allocate in the transaction
	 * context (not the calling context, usually a lower context), because the
	 * memory might otherwise be freed too early in an abort situation.
	 */
	if (need_buffers || need_wal)
		instr = MemoryContextAllocZero(CurTransactionContext, n * sizeof(Instrumentation));
	else
		instr = palloc0(n * sizeof(Instrumentation));

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
	Assert(!instr->finalized);

	if (instr->need_timer &&
		!INSTR_TIME_SET_CURRENT_LAZY(instr->starttime))
		elog(ERROR, "InstrStart called twice in a row");

	if (instr->need_bufusage || instr->need_walusage)
		InstrPushStackResource(instr);
}

void
InstrStop(Instrumentation *instr, double nTuples, bool finalize)
{
	instr_time	endtime;

	/* count the specified tuples */
	instr->ntuples += nTuples;

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
		InstrPopStackResource(instr);

	if (finalize)
	{
		instr->finalized = true;
		if (pgInstrStack)
			InstrStackAdd(pgInstrStack, &instr->stack);
	}
}

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

		instr->stack.previous = pgInstrStack;
		pgInstrStack = &instr->stack;
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
		pgInstrStack = instr->stack.previous;
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
	else if (dst->running && add->running && INSTR_TIME_CMP_LT(dst->firsttuple, add->firsttuple))
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
InstrStackAdd(InstrStack * dst, InstrStack * add)
{
	Assert(dst != NULL);
	Assert(add != NULL);

	BufferUsageAdd(&dst->bufusage, &add->bufusage);
	WalUsageAdd(&dst->walusage, &add->walusage);
}

/* note current values during parallel executor startup */
void
InstrStartParallelQuery(void)
{
	save_pgBufferUsage = pgBufferUsage;
	save_pgWalUsage = pgWalUsage;
}

/* report usage after parallel executor shutdown */
void
InstrEndParallelQuery(BufferUsage *bufusage, WalUsage *walusage)
{
	memset(bufusage, 0, sizeof(BufferUsage));
	BufferUsageAccumDiff(bufusage, &pgBufferUsage, &save_pgBufferUsage);
	memset(walusage, 0, sizeof(WalUsage));
	WalUsageAccumDiff(walusage, &pgWalUsage, &save_pgWalUsage);
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

	BufferUsageAdd(&pgBufferUsage, bufusage);
	WalUsageAdd(&pgWalUsage, walusage);
}

/* dst += add */
static void
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
static void
WalUsageAdd(WalUsage *dst, WalUsage *add)
{
	dst->wal_bytes += add->wal_bytes;
	dst->wal_records += add->wal_records;
	dst->wal_fpi += add->wal_fpi;
	dst->wal_buffers_full += add->wal_buffers_full;
}

void
WalUsageAccumDiff(WalUsage *dst, const WalUsage *add, const WalUsage *sub)
{
	dst->wal_bytes += add->wal_bytes - sub->wal_bytes;
	dst->wal_records += add->wal_records - sub->wal_records;
	dst->wal_fpi += add->wal_fpi - sub->wal_fpi;
	dst->wal_buffers_full += add->wal_buffers_full - sub->wal_buffers_full;
}

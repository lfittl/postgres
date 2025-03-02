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

WalUsage	pgWalUsage;
InstrumentUsage *pgInstrumentUsageStack = NULL;

static void WalUsageAdd(WalUsage *dst, const WalUsage *add);


/* Allocate new instrumentation structure(s) */
Instrumentation *
InstrAlloc(int n, int instrument_options, bool async_mode)
{
	Instrumentation *instr;

	/* initialize all fields to zeroes, then modify as needed */
	instr = palloc0(n * sizeof(Instrumentation));
	if (instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_TIMER | INSTRUMENT_WAL | INSTRUMENT_SHARED_HIT_DISTINCT))
	{
		bool		need_buffers = (instrument_options & INSTRUMENT_BUFFERS) != 0;
		bool		need_wal = (instrument_options & INSTRUMENT_WAL) != 0;
		bool		need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
		bool		need_shared_hit_distinct = (instrument_options & INSTRUMENT_SHARED_HIT_DISTINCT) != 0;
		int			i;

		for (i = 0; i < n; i++)
		{
			instr[i].need_bufusage = need_buffers;
			instr[i].need_walusage = need_wal;
			instr[i].need_timer = need_timer;
			instr[i].need_shared_hit_distinct = need_shared_hit_distinct;
			instr[i].async_mode = async_mode;
		}
	}

	return instr;
}

/* Initialize a pre-allocated instrumentation structure. */
void
InstrInit(Instrumentation *instr, int instrument_options)
{
	memset(instr, 0, sizeof(Instrumentation));
	instr->need_bufusage = (instrument_options & INSTRUMENT_BUFFERS) != 0;
	instr->need_walusage = (instrument_options & INSTRUMENT_WAL) != 0;
	instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
	instr->need_shared_hit_distinct = (instrument_options & INSTRUMENT_SHARED_HIT_DISTINCT) != 0;
}

/* Entry to a plan node */
void
InstrStartNode(Instrumentation *instr)
{
	if (instr->need_timer &&
		!INSTR_TIME_SET_CURRENT_LAZY(instr->starttime))
		elog(ERROR, "InstrStartNode called twice in a row");

	if (instr->need_bufusage || instr->need_walusage || instr->need_shared_hit_distinct)
	{
		instr->instrusage.previous = pgInstrumentUsageStack;
		pgInstrumentUsageStack = &instr->instrusage;
	}

	if (instr->need_shared_hit_distinct && !instr->instrusage.shared_blks_hit_distinct)
	{
		instr->instrusage.shared_blks_hit_distinct = palloc0(sizeof(hyperLogLogState));
		initHyperLogLog(instr->instrusage.shared_blks_hit_distinct, 16);
	}
}

/* Exit from a plan node */
void
InstrStopNode(Instrumentation *instr, double nTuples)
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
		pgInstrumentUsageStack = pgInstrumentUsageStack->previous;

	/* Is this the first tuple of this cycle? */
	if (!instr->running)
	{
		instr->running = true;
		instr->firsttuple = INSTR_TIME_GET_DOUBLE(instr->counter);
	}
	else
	{
		/*
		 * In async mode, if the plan node hadn't emitted any tuples before,
		 * this might be the first tuple
		 */
		if (instr->async_mode && save_tuplecount < 1.0)
			instr->firsttuple = INSTR_TIME_GET_DOUBLE(instr->counter);
	}
}

/* Update tuple count */
void
InstrUpdateTupleCount(Instrumentation *instr, double nTuples)
{
	/* count the returned tuples */
	instr->tuplecount += nTuples;
}

/* Finish a run cycle for a plan node */
void
InstrEndLoop(Instrumentation *instr)
{
	double		totaltime;

	/* Skip if nothing has happened, or already shut down */
	if (!instr->running)
		return;

	if (!INSTR_TIME_IS_ZERO(instr->starttime))
		elog(ERROR, "InstrEndLoop called on running node");

	/* Accumulate per-cycle statistics into totals */
	totaltime = INSTR_TIME_GET_DOUBLE(instr->counter);

	instr->startup += instr->firsttuple;
	instr->total += totaltime;
	instr->ntuples += instr->tuplecount;
	instr->nloops += 1;

	/*
	 * Accumulate usage stats into active one (if any)
	 *
	 * This ensures that if we tracked buffer/WAL usage for EXPLAIN ANALYZE, a
	 * potential extension interested in summary data can also get it.
	 */
	InstrUsageAddToCurrent(&instr->instrusage);

	/* Reset for next cycle (if any) */
	instr->running = false;
	INSTR_TIME_SET_ZERO(instr->starttime);
	INSTR_TIME_SET_ZERO(instr->counter);
	instr->firsttuple = 0;
	instr->tuplecount = 0;
}

/* aggregate instrumentation information */
void
InstrAggNode(Instrumentation *dst, Instrumentation *add)
{
	if (!dst->running && add->running)
	{
		dst->running = true;
		dst->firsttuple = add->firsttuple;
	}
	else if (dst->running && add->running && dst->firsttuple > add->firsttuple)
		dst->firsttuple = add->firsttuple;

	INSTR_TIME_ADD(dst->counter, add->counter);

	dst->tuplecount += add->tuplecount;
	dst->startup += add->startup;
	dst->total += add->total;
	dst->ntuples += add->ntuples;
	dst->ntuples2 += add->ntuples2;
	dst->nloops += add->nloops;
	dst->nfiltered1 += add->nfiltered1;
	dst->nfiltered2 += add->nfiltered2;

	/* Add delta of buffer/WAL usage since entry to node's totals */
	if (dst->need_bufusage || dst->need_walusage)
		InstrUsageAdd(&dst->instrusage, &add->instrusage);
}

/* Start buffer/WAL usage measurement */
void
InstrUsageStart()
{
	InstrumentUsage *usage = palloc0(sizeof(InstrumentUsage));

	usage->previous = pgInstrumentUsageStack;
	pgInstrumentUsageStack = usage;
}

/*
 * Call this before calling stop to add the usage metrics to the previous item
 * on the stack (if it exists)
 */
void
InstrUsageAccumToPrevious()
{
	if (!pgInstrumentUsageStack || !pgInstrumentUsageStack->previous)
		return;

	InstrUsageAdd(pgInstrumentUsageStack->previous, pgInstrumentUsageStack);
}

/* Stop usage measurement and return results */
InstrumentUsage *
InstrUsageStop()
{
	InstrumentUsage *result = pgInstrumentUsageStack;

	Assert(result != NULL);

	pgInstrumentUsageStack = result->previous;
	result->previous = NULL;

	return result;
}

void
InstrUsageReset_AfterError()
{
	pgInstrumentUsageStack = NULL;
}

void
InstrUsageAdd(InstrumentUsage * dst, const InstrumentUsage * add)
{
	BufferUsageAdd(&dst->bufusage, &add->bufusage);
	WalUsageAdd(&dst->walusage, &add->walusage);
}

void
InstrUsageAddToCurrent(InstrumentUsage * instrusage)
{
	if (pgInstrumentUsageStack != NULL)
		InstrUsageAdd(pgInstrumentUsageStack, instrusage);
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
WalUsageAdd(WalUsage *dst, const WalUsage *add)
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

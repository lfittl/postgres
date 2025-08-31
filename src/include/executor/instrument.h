/*-------------------------------------------------------------------------
 *
 * instrument.h
 *	  definitions for run-time statistics collection
 *
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * src/include/executor/instrument.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include "common/hashfn.h"
#include "lib/hyperloglog.h"
#include "portability/instr_time.h"
#include "utils/resowner.h"


typedef struct BufferUsage
{
	int64		shared_blks_hit;	/* # of shared buffer hits */
	int64		shared_blks_read;	/* # of shared disk blocks read */
	int64		shared_blks_dirtied;	/* # of shared blocks dirtied */
	int64		shared_blks_written;	/* # of shared disk blocks written */
	int64		local_blks_hit; /* # of local buffer hits */
	int64		local_blks_read;	/* # of local disk blocks read */
	int64		local_blks_dirtied; /* # of local blocks dirtied */
	int64		local_blks_written; /* # of local disk blocks written */
	int64		temp_blks_read; /* # of temp blocks read */
	int64		temp_blks_written;	/* # of temp blocks written */
	instr_time	shared_blk_read_time;	/* time spent reading shared blocks */
	instr_time	shared_blk_write_time;	/* time spent writing shared blocks */
	instr_time	local_blk_read_time;	/* time spent reading local blocks */
	instr_time	local_blk_write_time;	/* time spent writing local blocks */
	instr_time	temp_blk_read_time; /* time spent reading temp blocks */
	instr_time	temp_blk_write_time;	/* time spent writing temp blocks */
} BufferUsage;

/*
 * WalUsage tracks only WAL activity like WAL records generation that
 * can be measured per query and is displayed by EXPLAIN command,
 * pg_stat_statements extension, etc. It does not track other WAL activity
 * like WAL writes that it's not worth measuring per query. That's tracked
 * by WAL global statistics counters in WalStats, instead.
 */
typedef struct WalUsage
{
	int64		wal_records;	/* # of WAL records produced */
	int64		wal_fpi;		/* # of WAL full page images produced */
	uint64		wal_bytes;		/* size of WAL records produced */
	int64		wal_buffers_full;	/* # of times the WAL buffers became full */
} WalUsage;

/* Flag bits included in InstrAlloc's instrument_options bitmask */
typedef enum InstrumentOption
{
	INSTRUMENT_TIMER = 1 << 0,	/* needs timer (and row counts) */
	INSTRUMENT_BUFFERS = 1 << 1,	/* needs buffer usage */
	INSTRUMENT_ROWS = 1 << 2,	/* needs row count */
	INSTRUMENT_WAL = 1 << 3,	/* needs WAL usage */
	INSTRUMENT_SHARED_HIT_DISTINCT = 1 << 4,	/* needs estimated distinct
												 * shared hit buffer count */
	INSTRUMENT_ALL = PG_INT32_MAX
} InstrumentOption;

typedef struct InstrumentUsageResource
{
	struct InstrumentUsage *previous;

	ResourceOwner owner;
}			InstrumentUsageResource;

typedef struct InstrumentUsage
{
	struct InstrumentUsage *previous;
	BufferUsage bufusage;
	WalUsage	walusage;
	hyperLogLogState *shared_blks_hit_distinct;

	InstrumentUsageResource *res;
}			InstrumentUsage;

typedef struct Instrumentation
{
	/* Parameters set at node creation: */
	bool		need_timer;		/* true if we need timer data */
	bool		need_bufusage;	/* true if we need buffer usage data */
	bool		need_walusage;	/* true if we need WAL usage data */
	bool		need_shared_hit_distinct;	/* true if we need estimated
											 * distinct shared hit buffer
											 * count */
	bool		async_mode;		/* true if node is in async mode */
	/* Info about current plan cycle: */
	bool		running;		/* true if we've completed first tuple */
	instr_time	starttime;		/* start time of current iteration of node */
	instr_time	counter;		/* accumulated runtime for this node */
	double		firsttuple;		/* time for first tuple of this cycle */
	double		tuplecount;		/* # of tuples emitted so far this cycle */
	/* Accumulated statistics across all completed cycles: */
	double		startup;		/* total startup time (in seconds) */
	double		total;			/* total time (in seconds) */
	double		ntuples;		/* total tuples produced */
	double		ntuples2;		/* secondary node-specific tuple counter */
	double		nloops;			/* # of run cycles for this node */
	double		nfiltered1;		/* # of tuples removed by scanqual or joinqual */
	double		nfiltered2;		/* # of tuples removed by "other" quals */
	InstrumentUsage instrusage; /* total buffer/WAL usage */
} Instrumentation;

typedef struct WorkerInstrumentation
{
	int			num_workers;	/* # of structures that follow */
	Instrumentation instrument[FLEXIBLE_ARRAY_MEMBER];
} WorkerInstrumentation;

extern PGDLLIMPORT WalUsage pgWalUsage;
extern PGDLLIMPORT InstrumentUsage * pgInstrumentUsageStack;

extern Instrumentation *InstrAlloc(int n, int instrument_options,
								   bool async_mode);
extern void InstrInit(Instrumentation *instr, int instrument_options);
extern void InstrStart(Instrumentation *instr, bool use_resowner);
extern void InstrStartNode(Instrumentation *instr);
extern void InstrStop(Instrumentation *instr, double nTuples, bool use_resowner);
extern void InstrStopNode(Instrumentation *instr, double nTuples);
extern void InstrUpdateTupleCount(Instrumentation *instr, double nTuples);
extern void InstrEndLoop(Instrumentation *instr);
extern void InstrAggNode(Instrumentation *dst, Instrumentation *add);
extern void WalUsageAccumDiff(WalUsage *dst, const WalUsage *add,
							  const WalUsage *sub);

static inline bool
InstrumentUsageActive(void)
{
	return pgInstrumentUsageStack != NULL;
}

#define INSTR_BUFUSAGE_INCR(fld) do { \
		if (pgInstrumentUsageStack) \
			pgInstrumentUsageStack->bufusage.fld++; \
	} while(0)
#define INSTR_BUFUSAGE_ADD(fld,val) do { \
		if (pgInstrumentUsageStack) \
			pgInstrumentUsageStack->bufusage.fld += val; \
	} while(0)
#define INSTR_BUFUSAGE_TIME_ADD(fld,val) do { \
	if (pgInstrumentUsageStack) \
		INSTR_TIME_ADD(pgInstrumentUsageStack->bufusage.fld, val); \
	} while (0)
#define INSTR_BUFUSAGE_TIME_ACCUM_DIFF(fld,endval,startval) do { \
	if (pgInstrumentUsageStack) \
		INSTR_TIME_ACCUM_DIFF(pgInstrumentUsageStack->bufusage.fld, endval, startval); \
	} while (0)

#define INSTR_BUFUSAGE_COUNT_SHARED_HIT(bufId) do { \
		if (pgInstrumentUsageStack) { \
			pgInstrumentUsageStack->bufusage.shared_blks_hit++; \
			if (pgInstrumentUsageStack->shared_blks_hit_distinct) \
				addHyperLogLog(pgInstrumentUsageStack->shared_blks_hit_distinct, DatumGetUInt32(hash_any((unsigned char *) &bufId, sizeof(int)))); \
		} \
	} while(0)

#define INSTR_WALUSAGE_INCR(fld) do { \
		if (pgInstrumentUsageStack) \
			pgInstrumentUsageStack->walusage.fld++; \
	} while(0)
#define INSTR_WALUSAGE_ADD(fld,val) do { \
		if (pgInstrumentUsageStack) \
			pgInstrumentUsageStack->walusage.fld += val; \
	} while(0)

extern void InstrUsageStart(void);
extern InstrumentUsage * InstrUsageStop(void);
extern void InstrUsageAccumToPrevious(void);
extern void InstrUsageAdd(InstrumentUsage * dst, const InstrumentUsage * add);
extern void InstrUsageAddToCurrent(InstrumentUsage * instrusage);
extern void BufferUsageAdd(BufferUsage *dst, const BufferUsage *add);

#endif							/* INSTRUMENT_H */

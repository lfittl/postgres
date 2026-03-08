/*-------------------------------------------------------------------------
 *
 * pg_session_buffer_usage.c
 *	  show buffer usage statistics for the current session
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_session_buffer_usage/pg_session_buffer_usage.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_session_buffer_usage",
					.version = PG_VERSION
);

#define NUM_BUFFER_USAGE_COLUMNS 16

PG_FUNCTION_INFO_V1(pg_session_buffer_usage);
PG_FUNCTION_INFO_V1(pg_session_buffer_usage_reset);

#define HAVE_INSTR_STACK 1		/* Change to 0 when testing before stack
								 * change */

/*
 * SQL function: pg_session_buffer_usage()
 *
 * Returns a single row with all BufferUsage counters accumulated since the
 * start of the session. Excludes any usage not yet added to the top of the
 * stack (e.g. if this gets called inside a statement that also had buffer
 * activity).
 */
Datum
pg_session_buffer_usage(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[NUM_BUFFER_USAGE_COLUMNS];
	bool		nulls[NUM_BUFFER_USAGE_COLUMNS];
	BufferUsage *usage;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(nulls, 0, sizeof(nulls));

#if HAVE_INSTR_STACK
	usage = &TopInstrStack.bufusage;
#else
	usage = &pgBufferUsage;
#endif

	values[0] = Int64GetDatum(usage->shared_blks_hit);
	values[1] = Int64GetDatum(usage->shared_blks_read);
	values[2] = Int64GetDatum(usage->shared_blks_dirtied);
	values[3] = Int64GetDatum(usage->shared_blks_written);
	values[4] = Int64GetDatum(usage->local_blks_hit);
	values[5] = Int64GetDatum(usage->local_blks_read);
	values[6] = Int64GetDatum(usage->local_blks_dirtied);
	values[7] = Int64GetDatum(usage->local_blks_written);
	values[8] = Int64GetDatum(usage->temp_blks_read);
	values[9] = Int64GetDatum(usage->temp_blks_written);
	values[10] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(usage->shared_blk_read_time));
	values[11] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(usage->shared_blk_write_time));
	values[12] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(usage->local_blk_read_time));
	values[13] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(usage->local_blk_write_time));
	values[14] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(usage->temp_blk_read_time));
	values[15] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(usage->temp_blk_write_time));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * SQL function: pg_session_buffer_usage_reset()
 *
 * Resets all BufferUsage counters on the top instrumentation stack to zero.
 * Useful in tests to avoid the baseline/delta pattern.
 */
Datum
pg_session_buffer_usage_reset(PG_FUNCTION_ARGS)
{
#if HAVE_INSTR_STACK
	memset(&TopInstrStack.bufusage, 0, sizeof(BufferUsage));
#else
	memset(&pgBufferUsage, 0, sizeof(BufferUsage));
#endif

	PG_RETURN_VOID();
}

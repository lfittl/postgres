/*-------------------------------------------------------------------------
 *
 * instr_time.c
 *	   Non-inline parts of the portable high-precision interval timing
 *	 implementation
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/port/instr_time.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "portability/instr_time.h"

/*
 * Stores what the number of ticks needs to be multiplied with to end up
 * with nanoseconds using integer math.
 *
 * On certain platforms (currently Windows) the ticks to nanoseconds conversion
 * requires floating point math because:
 *
 * sec = ticks / frequency_hz
 * ns  = ticks / frequency_hz * 1,000,000,000
 * ns  = ticks * (1,000,000,000 / frequency_hz)
 * ns  = ticks * (1,000,000 / frequency_khz) <-- now in kilohertz
 *
 * Here, 'ns' is usually a floating number. For example for a 2.5 GHz CPU
 * the scaling factor becomes 1,000,000 / 2,500,000 = 1.2.
 *
 * To be able to use integer math we work around the lack of precision. We
 * first scale the integer up and after the multiplication by the number
 * of ticks in INSTR_TIME_GET_NANOSEC() we divide again by the same value.
 * We picked the scaler such that it provides enough precision and is a
 * power-of-two which allows for shifting instead of doing an integer
 * division. We utilize unsigned integers even though ticks are stored as a
 * signed value because that encourages compilers to generate better assembly.
 *
 * On all other platforms we are using clock_gettime(), which uses nanoseconds
 * as ticks. Hence, we set the multiplier to zero, which causes pg_ticks_to_ns
 * to return the original value.
 */
uint64		ticks_per_ns_scaled = 0;
uint64		max_ticks_no_overflow = 0;

static void set_ticks_per_ns(void);

static bool timing_initialized = false;

void
pg_initialize_timing(void)
{
	if (timing_initialized)
		return;

	set_ticks_per_ns();
	timing_initialized = true;
}

#ifndef WIN32

static void
set_ticks_per_ns()
{
	ticks_per_ns_scaled = 0;
	max_ticks_no_overflow = 0;
}

#else							/* WIN32 */

/* GetTimerFrequency returns counts per second */
static inline double
GetTimerFrequency(void)
{
	LARGE_INTEGER f;

	QueryPerformanceFrequency(&f);
	return (double) f.QuadPart;
}

static void
set_ticks_per_ns()
{
	ticks_per_ns_scaled = NS_PER_S * TICKS_TO_NS_PRECISION / GetTimerFrequency();
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#endif							/* WIN32 */

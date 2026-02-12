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

#ifndef WIN32
#include <unistd.h>
#endif

#include "port/pg_cpu.h"
#include "portability/instr_time.h"

/*
 * Stores what the number of ticks needs to be multiplied with to end up
 * with nanoseconds using integer math.
 *
 * In certain cases (TSC on x86-64, and QueryPerformanceCounter on Windows)
 * the ticks to nanoseconds conversion requires floating point math because:
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
 * first scale the integer up (left shift by TICKS_TO_NS_SHIFT) and after the
 * multiplication by the number of ticks in pg_ticks_to_ns() we shift right by
 * the same amount. We utilize unsigned integers even though ticks are stored
 * as a signed value to encourage compilers to generate better assembly.
 *
 * We remember the maximum number of ticks that can be multiplied by the scale
 * factor without overflowing so we can check via a * b > max <=> a > max / b.
 *
 * In all other cases we are using clock_gettime(), which uses nanoseconds
 * as ticks. Hence, we set the multiplier to zero, which causes pg_ticks_to_ns
 * to return the original value.
 */
uint64		ticks_per_ns_scaled = 0;
uint64		max_ticks_no_overflow = 0;

static void set_ticks_per_ns(void);

int			timing_clock_source = TIMING_CLOCK_SOURCE_AUTO;
static bool timing_initialized = false;

#if PG_INSTR_TSC_CLOCK
/* Indicates if TSC instructions (RDTSC and RDTSCP) are usable. */
static bool has_usable_tsc = false;

static void tsc_initialize(void);
static bool tsc_use_by_default(void);
static void set_ticks_per_ns_for_tsc(void);
#endif

void
pg_initialize_timing(void)
{
	if (timing_initialized)
		return;

#if PG_INSTR_TSC_CLOCK
	tsc_initialize();
#endif

	set_ticks_per_ns();
	timing_initialized = true;
}

bool
pg_set_timing_clock_source(TimingClockSourceType source)
{
	Assert(timing_initialized);

#if PG_INSTR_TSC_CLOCK
	switch (source)
	{
		case TIMING_CLOCK_SOURCE_AUTO:
			use_tsc = has_usable_tsc && tsc_use_by_default();
			break;
		case TIMING_CLOCK_SOURCE_SYSTEM:
			use_tsc = false;
			break;
		case TIMING_CLOCK_SOURCE_TSC:
			if (!has_usable_tsc)	/* Tell caller TSC is not usable */
				return false;
			use_tsc = true;
			break;
	}
#endif

	set_ticks_per_ns();
	timing_clock_source = source;
	return true;
}

#ifndef WIN32

static void
set_ticks_per_ns_system()
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
set_ticks_per_ns_system()
{
	ticks_per_ns_scaled = (NS_PER_S << TICKS_TO_NS_SHIFT) / GetTimerFrequency();
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#endif							/* WIN32 */

static void
set_ticks_per_ns()
{
#if PG_INSTR_TSC_CLOCK
	if (use_tsc)
		set_ticks_per_ns_for_tsc();
	else
		set_ticks_per_ns_system();
#else
	set_ticks_per_ns_system();
#endif
}

/* GUC handling */

#ifndef FRONTEND

#include "utils/guc_hooks.h"

bool
check_timing_clock_source(int *newval, void **extra, GucSource source)
{
	pg_initialize_timing();

#if PG_INSTR_TSC_CLOCK
	if (*newval == TIMING_CLOCK_SOURCE_TSC && !has_usable_tsc)
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
	/*
	 * Ignore the return code since the check hook already verified TSC is
	 * usable if its explicitly requested
	 */
	pg_set_timing_clock_source(newval);
}

const char *
show_timing_clock_source()
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

#endif							/* !FRONTEND */

/* TSC specific logic */

#if PG_INSTR_TSC_CLOCK

bool		use_tsc = false;

static uint32 tsc_frequency_khz = 0;

/*
 * Decide whether we use the RDTSC/RDTSCP instructions at runtime, for Linux/x86-64,
 * instead of incurring the overhead of a full clock_gettime() call.
 *
 * This can't be reliably determined at compile time, since the
 * availability of an "invariant" TSC (that is not affected by CPU
 * frequency changes) is dependent on the CPU architecture. Additionally,
 * there are cases where TSC availability is impacted by virtualization,
 * where a simple cpuid feature check would not be enough.
 */
static void
tsc_initialize(void)
{
	/* Determine speed at which the TSC advances */
	tsc_frequency_khz = x86_tsc_frequency_khz();
	if (!tsc_frequency_khz)
		return;

	has_usable_tsc = x86_feature_available(PG_RDTSCP);
}

/*
 * Decides whether to use the TSC clock source if the user did not specify it
 * one way or the other, and it is available (checked separately).
 *
 * Mirrors the Linux kernel's clocksource watchdog disable logic as updated in
 * 2021 to reflect the reliability of the TSC on Intel platforms, see
 * check_system_tsc_reliable() in arch/x86/kernel/tsc.c, as well as discussion
 * in https://lore.kernel.org/lkml/87eekfk8bd.fsf@nanos.tec.linutronix.de/
 * and https://lore.kernel.org/lkml/87a6pimt1f.ffs@nanos.tec.linutronix.de/
 * for reference.
 *
 * When the CPU has an invariant TSC (which we require in x86_tsc_frequency_khz),
 * TSC_ADJUST bit set (Intel-only), and the system has at most 4 physical
 * packages (sockets), we consider the TSC trustworthy by default, matching the
 * Linux kernel.
 *
 * On other CPU platforms (e.g. AMD), in a virtual machine, or on 8+ socket
 * systems we don't have an easy way to determine the TSC's reliability. If on
 * Linux, we can check if TSC is the active clocksource, based on it having run
 * the watchdog logic to monitor TSC correctness. For other platforms the user
 * must explicitly enable it via GUC instead.
 */
static bool
tsc_use_by_default(void)
{
	if (x86_feature_available(PG_TSC_ADJUST))
	{
		int			cpus_per_package = x86_logical_processors_per_package();
		long		total_cpus;

#ifdef _SC_NPROCESSORS_CONF
		total_cpus = sysconf(_SC_NPROCESSORS_CONF);
#elif defined(WIN32)
		{
			SYSTEM_INFO si;

			GetSystemInfo(&si);
			total_cpus = si.dwNumberOfProcessors;
		}
#else
		total_cpus = -1;
#endif

		if (total_cpus > 0 && cpus_per_package > 0 && (total_cpus / cpus_per_package) <= 4)
			return true;
	}

#if defined(__linux__)
	{
		FILE	   *fp;
		char		buf[128];

		fp = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
		if (fp)
		{
			bool		is_tsc = (fgets(buf, sizeof(buf), fp) != NULL &&
								  strcmp(buf, "tsc\n") == 0);

			fclose(fp);
			if (is_tsc)
				return true;
		}
	}
#endif

	return false;
}

static void
set_ticks_per_ns_for_tsc(void)
{
	ticks_per_ns_scaled = ((NS_PER_S / 1000) << TICKS_TO_NS_SHIFT) / tsc_frequency_khz;
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#endif							/* PG_INSTR_TSC_CLOCK */

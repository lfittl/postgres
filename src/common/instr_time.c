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

#include <math.h>

#ifndef WIN32
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
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
bool		has_usable_tsc = false;

static void tsc_initialize(bool allow_tsc_calibration);
static bool tsc_use_by_default(void);
static void set_ticks_per_ns_system();
static void set_ticks_per_ns_for_tsc(void);
#endif

/*
 * Initializes timing infrastructure. Must be called before making any use
 * of INSTR* macros.
 *
 * The allow_tsc_calibration argument sets whether the TSC logic (if available)
 * is permitted to do calibration if it couldn't get the frequency from CPUID.
 *
 * Calibration may take up to TSC_CALIBRATION_MAX_NS and delays program start.
 */
void
pg_initialize_timing(bool allow_tsc_calibration)
{
	if (timing_initialized)
		return;

#if PG_INSTR_TSC_CLOCK
	tsc_initialize(allow_tsc_calibration);
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

/* Hardware clock specific logic (x86 TSC / AArch64 CNTVCT) */

#if PG_INSTR_TSC_CLOCK

bool		use_tsc = false;

static uint32 tsc_frequency_khz = 0;

static void
set_ticks_per_ns_for_tsc(void)
{
	ticks_per_ns_scaled = ((NS_PER_S / 1000) << TICKS_TO_NS_SHIFT) / tsc_frequency_khz;
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#if defined(__x86_64__) || defined(_M_X64)

/*
 * x86-64 TSC specific logic
 */

static uint32 tsc_calibrate(void);

/*
 * Decide whether we use the RDTSC/RDTSCP instructions at runtime, for x86-64,
 * instead of incurring the overhead of a full clock_gettime() call.
 *
 * This can't be reliably determined at compile time, since the
 * availability of an "invariant" TSC (that is not affected by CPU
 * frequency changes) is dependent on the CPU architecture. Additionally,
 * there are cases where TSC availability is impacted by virtualization,
 * where a simple cpuid feature check would not be enough.
 */
static void
tsc_initialize(bool allow_tsc_calibration)
{
	/* Determine speed at which the TSC advances */
	tsc_frequency_khz = x86_tsc_frequency_khz();

	if (tsc_frequency_khz)
	{
		has_usable_tsc = x86_feature_available(PG_RDTSCP);
		return;
	}

	/*
	 * CPUID did not give us the TSC frequency. If TSC is invariant and RDTSCP
	 * is available, we can measure the frequency by comparing TSC ticks
	 * against walltime using a short calibration loop.
	 */
	if (allow_tsc_calibration && x86_feature_available(PG_TSC_INVARIANT) &&
		x86_feature_available(PG_RDTSCP))
	{
		tsc_frequency_khz = tsc_calibrate();
		has_usable_tsc = (tsc_frequency_khz > 0);
	}
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
#endif							/* _SC_NPROCESSORS_CONF / WIN32 */

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

/*
 * Calibrate the TSC frequency by comparing TSC ticks against walltime.
 *
 * Takes initial TSC and system clock snapshots, then loops, recomputing the
 * frequency each iteration from cumulative TSC ticks divided by elapsed time.
 *
 * Once the frequency estimate stabilizes (consecutive iterations agree), we
 * consider it converged and the frequency in KHz is returned. If either too
 * many iterations or a time limit passes without convergence, 0 is returned.
 */
#define TSC_CALIBRATION_MAX_NS		(50 * NS_PER_MS)
#define TSC_CALIBRATION_ITERATIONS	1000000
#define TSC_CALIBRATION_STABLE_CYCLES	3

static uint32
tsc_calibrate(void)
{
	instr_time	initial_wall;
	int64		initial_tsc;
	double		freq_khz = 0;
	double		prev_freq_khz = 0;
	int			stable_count = 0;
	int64		prev_tsc;
	uint32		unused;

	/* Ensure INSTR_* time below work on system time */
	set_ticks_per_ns_system();

	INSTR_TIME_SET_CURRENT(initial_wall);

#ifdef _MSC_VER
	initial_tsc = __rdtscp(&unused);
#else
	initial_tsc = __builtin_ia32_rdtscp(&unused);
#endif
	prev_tsc = initial_tsc;

	for (int i = 0; i < TSC_CALIBRATION_ITERATIONS; i++)
	{
		instr_time	now_wall;
		int64		now_tsc;
		int64		elapsed_ns;
		int64		elapsed_ticks;

		INSTR_TIME_SET_CURRENT(now_wall);

#ifdef _MSC_VER
		now_tsc = __rdtscp(&unused);
#else
		now_tsc = __builtin_ia32_rdtscp(&unused);
#endif

		INSTR_TIME_SUBTRACT(now_wall, initial_wall);
		elapsed_ns = INSTR_TIME_GET_NANOSEC(now_wall);

		/* Safety: bail out if we've taken too long */
		if (elapsed_ns >= TSC_CALIBRATION_MAX_NS)
			break;

		elapsed_ticks = now_tsc - initial_tsc;

		/* Skip if TSC hasn't advanced, or we walked backwards for some reason */
		if (now_tsc == prev_tsc || elapsed_ns <= 0 || elapsed_ticks <= 0)
			continue;

		freq_khz = ((double) elapsed_ticks / elapsed_ns) * 1000 * 1000;

		/*
		* Once freq_khz / prev_freq_khz is small, check if it stays that way.
		* If it does for long enough, we've got a winner frequency.
		*/
		if (prev_freq_khz != 0 && fabs(freq_khz / prev_freq_khz) < 1.0001)
		{
			stable_count++;
			if (stable_count >= TSC_CALIBRATION_STABLE_CYCLES)
				return (uint32) freq_khz;
		}
		else
			stable_count = 0;

		prev_tsc = now_tsc;
		prev_freq_khz = freq_khz;
	}

	/* did not converge */
	return 0;
}

#elif defined(__aarch64__)

/*
 * Check whether this is a heterogeneous Apple Silicon P+E core system
 * where CNTVCT_EL0 may tick at different rates on different core types.
 */
static bool
aarch64_has_heterogeneous_cores(void)
{
#if defined(__APPLE__)
	int			nperflevels = 0;
	size_t		len = sizeof(nperflevels);

	if (sysctlbyname("hw.nperflevels", &nperflevels, &len, NULL, 0) == 0)
		return nperflevels > 1;
#endif

	return false;
}

/*
 * Initialize the AArch64 generic timer as a clock source.
 */
static void
tsc_initialize(bool allow_tsc_calibration)
{
	if (aarch64_has_heterogeneous_cores())
		return;

	tsc_frequency_khz = aarch64_cntvct_frequency_khz();
	if (tsc_frequency_khz != 0)
		has_usable_tsc = true;
}

/*
 * The ARM generic timer is architecturally guaranteed to be monotonic and
 * synchronized across cores of the same type, so we always use it by default
 * when available and cores are homogenous.
 */
static bool
tsc_use_by_default(void)
{
	return true;
}

#endif							/* defined(__aarch64__) */

#endif							/* PG_INSTR_TSC_CLOCK */

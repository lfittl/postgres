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
 * first scale the integer up and after the multiplication by the number
 * of ticks in INSTR_TIME_GET_NANOSEC() we divide again by the same value.
 * We picked the scaler such that it provides enough precision and is a
 * power-of-two which allows for shifting instead of doing an integer
 * division. We utilize unsigned integers even though ticks are stored as a
 * signed value because that encourages compilers to generate better assembly.
 *
 * In all other cases we are using clock_gettime(), which uses nanoseconds
 * as ticks. Hence, we set the multiplier to zero, which causes pg_ticks_to_ns
 * to return the original value.
 */
uint64		ticks_per_ns_scaled = 0;
uint64		max_ticks_no_overflow = 0;

static void set_ticks_per_ns(void);

int			timing_clock_source = TIMING_CLOCK_SOURCE_AUTO;

#if defined(__x86_64__)
/* Indicates if TSC instructions (RDTSC and RDTSCP) are usable. */
extern bool has_usable_tsc;

static void tsc_initialize(void);
static bool tsc_use_by_default(void);
static void set_ticks_per_ns_for_tsc(void);
static bool set_tsc_frequency_khz(void);
static bool is_rdtscp_available(void);
#endif

void
pg_initialize_timing()
{
#if defined(__x86_64__)
	tsc_initialize();
#endif

	set_ticks_per_ns();
}

bool
pg_set_timing_clock_source(TimingClockSourceType source)
{
#if defined(__x86_64__)
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
	ticks_per_ns_scaled = INT64CONST(1000000000) * TICKS_TO_NS_PRECISION / GetTimerFrequency();
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#endif							/* WIN32 */

static void
set_ticks_per_ns()
{
#if defined(__x86_64__)
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
#if defined(__x86_64__)
	pg_initialize_timing();

	if (*newval == TIMING_CLOCK_SOURCE_TSC && !has_usable_tsc)
	{
		GUC_check_errdetail("TSC is not supported as fast clock source");
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
#if defined(__x86_64__)
	TimingClockSourceType effective_source = pg_current_timing_clock_source();

	switch (timing_clock_source)
	{
		case TIMING_CLOCK_SOURCE_AUTO:
			if (effective_source == TIMING_CLOCK_SOURCE_TSC)
				return "auto (tsc)";
			else
				return "auto (system)";
		case TIMING_CLOCK_SOURCE_SYSTEM:
			return "system";
		case TIMING_CLOCK_SOURCE_TSC:
			return "tsc";
	}
#else
	switch (timing_clock_source)
	{
		case TIMING_CLOCK_SOURCE_AUTO:
			return "auto (system)";
		case TIMING_CLOCK_SOURCE_SYSTEM:
			return "system";
	}
#endif

	/* unreachable */
	return "?";
}

#endif							/* !FRONTEND */

/* TSC specific logic */

#if defined(__x86_64__)

#if defined(HAVE__GET_CPUID) || (defined(HAVE__CPUIDEX) && !defined(_MSC_VER))
#include <cpuid.h>
#endif

#if defined(HAVE__CPUID) || (defined(HAVE__CPUIDEX) && defined(_MSC_VER))
#include <intrin.h>
#endif

bool		has_usable_tsc = false;
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
	/*
	 * Compute baseline CPU peformance, determines speed at which the TSC
	 * advances.
	 */
	if (!set_tsc_frequency_khz())
		return;

	has_usable_tsc = is_rdtscp_available();
}

/*
 * Decides whether to use TSC clock source if the user did not specify it
 * one way or the other, and it is available (checked separately).
 *
 * Currently only enabled by default on Linux, since Linux already does a
 * significant amount of work to determine whether TSC is a viable clock
 * source.
 */
static bool
tsc_use_by_default()
{
#if defined(__linux__)
	FILE	   *fp = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
	char		buf[128];

	if (!fp)
		return false;

	if (fgets(buf, sizeof(buf), fp) != NULL && strcmp(buf, "tsc\n") == 0)
	{
		fclose(fp);
		return true;
	}

	fclose(fp);
#endif

	return false;
}

static void
set_ticks_per_ns_for_tsc()
{
	ticks_per_ns_scaled = INT64CONST(1000000) * TICKS_TO_NS_PRECISION / tsc_frequency_khz;
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#define CPUID_HYPERVISOR_VMWARE(words) (words[1] == 0x61774d56 && words[2] == 0x4d566572 && words[3] == 0x65726177) /* VMwareVMware */
#define CPUID_HYPERVISOR_KVM(words) (words[1] == 0x4b4d564b && words[2] == 0x564b4d56 && words[3] == 0x0000004d)	/* KVMKVMKVM */

static bool
set_tsc_frequency_khz()
{
	uint32		r[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID)
	__get_cpuid(0x15, &r[0] /* denominator */ , &r[1] /* numerator */ , &r[2] /* hz */ , &r[3]);
#elif defined(HAVE__CPUID)
	__cpuid(r, 0x15);
#else
#error cpuid instruction not available
#endif

	if (r[2] > 0)
	{
		if (r[0] == 0 || r[1] == 0)
			return false;

		tsc_frequency_khz = r[2] / 1000 * r[1] / r[0];
		return true;
	}

	/* Some CPUs only report frequency in 16H */

#if defined(HAVE__GET_CPUID)
	__get_cpuid(0x16, &r[0] /* base_mhz */ , &r[1], &r[2], &r[3]);
#elif defined(HAVE__CPUID)
	__cpuid(r, 0x16);
#else
#error cpuid instruction not available
#endif

	if (r[0] > 0)
	{
		tsc_frequency_khz = r[0] * 1000;
		return true;
	}

	/*
	 * Check if we have a KVM or VMware Hypervisor passing down TSC frequency
	 * to us in a guest VM
	 *
	 * Note that accessing the 0x40000000 leaf for Hypervisor info requires
	 * use of __cpuidex to set ECX to 0. The similar __get_cpuid_count
	 * function does not work as expected since it contains a check for
	 * __get_cpuid_max, which has been observed to be lower than the special
	 * Hypervisor leaf.
	 */
#if defined(HAVE__CPUIDEX)
	__cpuidex((int32 *) r, 0x40000000, 0);
	if (r[0] >= 0x40000010 && (CPUID_HYPERVISOR_VMWARE(r) || CPUID_HYPERVISOR_KVM(r)))
	{
		__cpuidex((int32 *) r, 0x40000010, 0);
		if (r[0] > 0)
		{
			tsc_frequency_khz = r[0];
			return true;
		}
	}
#endif

	return false;
}

static bool
is_rdtscp_available()
{
	uint32		r[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID)
	if (!__get_cpuid(0x80000001, &r[0], &r[1], &r[2], &r[3]))
		return false;
#elif defined(HAVE__CPUID)
	__cpuid(r, 0x80000001);
#else
#error cpuid instruction not available
#endif

	return (r[3] & (1 << 27)) != 0;
}

#endif							/* defined(__x86_64__) */

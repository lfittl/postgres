/*-------------------------------------------------------------------------
 *
 * instr_time.c
 *	   Non-inline parts of the portable high-precision interval timing
 *	 implementation
 *
 * Portions Copyright (c) 2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/port/instr_time.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#if defined(HAVE__GET_CPUID) || (defined(HAVE__CPUIDEX) && !defined(_MSC_VER))
#include <cpuid.h>
#endif

#if defined(HAVE__CPUID) || (defined(HAVE__CPUIDEX) && defined(_MSC_VER))
#include <intrin.h>
#endif

#include "portability/instr_time.h"

#if !defined(WIN32) && defined(__x86_64__)
static void set_ticks_per_ns(void);
static void pg_initialize_rdtsc(void);
#endif

#ifdef FRONTEND

void
pg_initialize_fast_clock_source()
{
#if !defined(WIN32) && defined(__x86_64__)
	pg_initialize_rdtsc();
	use_tsc = has_rdtsc && has_rdtscp;
	set_ticks_per_ns();
#endif
}

#else

#include "utils/guc_hooks.h"

int			fast_clock_source = FAST_CLOCK_SOURCE_AUTO;

bool
check_fast_clock_source(int *newval, void **extra, GucSource source)
{
#if !defined(WIN32) && defined(__x86_64__)
	if (*newval == FAST_CLOCK_SOURCE_AUTO || *newval == FAST_CLOCK_SOURCE_RDTSC)
		pg_initialize_rdtsc();

	if (*newval == FAST_CLOCK_SOURCE_RDTSC && (!has_rdtsc || !has_rdtscp))
	{
		GUC_check_errdetail("TSC is not supported as fast clock source");
		return false;
	}
#endif

	return true;
}

void
assign_fast_clock_source(int newval, void *extra)
{
#if !defined(WIN32) && defined(__x86_64__)
	use_tsc = has_rdtsc && has_rdtscp && (newval == FAST_CLOCK_SOURCE_RDTSC || (newval == FAST_CLOCK_SOURCE_AUTO && pg_fast_clock_source_default()));

	set_ticks_per_ns();
#endif
}

#endif							/* FRONTEND */

/*
 * Decides whether to use TSC time source if the user did not specify it
 * one way or the other, and it is available (checked separately).
 *
 * Currently only enabled by default on Linux, since Linux already does a
 * significant amount of work to determine whether TSC is a viable clock
 * source.
 */
bool
pg_fast_clock_source_default()
{
#if defined(__x86_64__) && defined(__linux__)
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

#ifndef WIN32
/*
 * Stores what the number of cycles needs to be multiplied with to end up
 * with nanoseconds using integer math. See comment in pg_initialize_rdtsc()
 * for more details.
 *
 * By default assume we are using clock_gettime() as a fallback which uses
 * nanoseconds as ticks. Hence, we set the multiplier to the precision scalar
 * so that the division in INSTR_TIME_GET_NANOSEC() won't change the nanoseconds.
 *
 * When using the RDTSC instruction directly this is filled in during initialization
 * based on the relevant CPUID fields.
 */
int64		ticks_per_ns_scaled = TICKS_TO_NS_PRECISION;
int64		max_ticks_no_overflow = PG_INT64_MAX / TICKS_TO_NS_PRECISION;

#if defined(__x86_64__)

bool		has_rdtsc = false;
bool		has_rdtscp = false;
bool		use_tsc = false;

static uint32 tsc_freq = 0;

static int64
ticks_per_ns_for_system()
{
	return TICKS_TO_NS_PRECISION;
}

/*
 * For TSC, the ticks to nanoseconds conversion requires floating point math
 * because:
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
 * division.
 */
static int64
ticks_per_ns_for_tsc()
{
	return INT64CONST(1000000) * TICKS_TO_NS_PRECISION / tsc_freq;
}

static void
set_ticks_per_ns()
{
	if (use_tsc)
		ticks_per_ns_scaled = ticks_per_ns_for_tsc();
	else
		ticks_per_ns_scaled = ticks_per_ns_for_system();
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#define CPUID_HYPERVISOR_VMWARE(words) (words[1] == 0x61774d56 && words[2] == 0x4d566572 && words[3] == 0x65726177) /* VMwareVMware */
#define CPUID_HYPERVISOR_KVM(words) (words[1] == 0x4b4d564b && words[2] == 0x564b4d56 && words[3] == 0x0000004d)	/* KVMKVMKVM */

static bool
get_tsc_frequency_khz()
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

		tsc_freq = r[2] / 1000 * r[1] / r[0];
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
		tsc_freq = r[0] * 1000;
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
			tsc_freq = r[0];
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

/*
 * Decide whether we use the RDTSC instruction at runtime, for Linux/x86,
 * instead of incurring the overhead of a full clock_gettime() call.
 *
 * This can't be reliably determined at compile time, since the
 * availability of an "invariant" TSC (that is not affected by CPU
 * frequency changes) is dependent on the CPU architecture. Additionally,
 * there are cases where TSC availability is impacted by virtualization,
 * where a simple cpuid feature check would not be enough.
 */
static void
pg_initialize_rdtsc(void)
{
	/*
	 * Compute baseline CPU peformance, determines speed at which RDTSC
	 * advances.
	 */
	if (!get_tsc_frequency_khz())
		return;

	has_rdtsc = true;
	has_rdtscp = is_rdtscp_available();
}
#endif							/* defined(__x86_64__) */

#endif							/* WIN32 */

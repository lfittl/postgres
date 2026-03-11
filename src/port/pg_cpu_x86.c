/*-------------------------------------------------------------------------
 *
 * pg_cpu_x86.c
 *	  Runtime CPU feature detection for x86
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_cpu_x86.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#if defined(USE_SSE2) || defined(__i386__)

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif							/* defined(_MSC_VER) */

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef HAVE_XSAVE_INTRINSICS
#include <immintrin.h>
#endif

#include "port/pg_cpu.h"

/*
 * XSAVE state component bits that we need
 *
 * https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-1-manual.pdf
 * Chapter "MANAGING STATE USING THE XSAVE FEATURE SET"
 */
#define XMM			(1<<1)
#define YMM			(1<<2)
#define OPMASK		(1<<5)
#define ZMM0_15		(1<<6)
#define ZMM16_31	(1<<7)


/* array indexed by enum X86FeatureId */
bool		X86Features[X86FeaturesSize] = {0};

static bool
mask_available(uint32 value, uint32 mask)
{
	return (value & mask) == mask;
}

/* Named indexes for CPUID register array */
#define EAX 0
#define EBX 1
#define ECX 2
#define EDX 3

/*
 * Request CPUID information for the specified leaf.
 */
static inline void
pg_cpuid(int leaf, unsigned int *reg)
{
#if defined(HAVE__GET_CPUID)
	__get_cpuid(leaf, &reg[EAX], &reg[EBX], &reg[ECX], &reg[EDX]);
#elif defined(HAVE__CPUID)
	__cpuid((int *) reg, leaf);
#else
#error cpuid instruction not available
#endif
}

/*
 * Request CPUID information for the specified leaf and subleaf.
 *
 * Returns true if the CPUID leaf/subleaf is supported, false otherwise.
 */
static inline bool
pg_cpuid_subleaf(int leaf, int subleaf, unsigned int *reg)
{
#if defined(HAVE__GET_CPUID_COUNT)
	return __get_cpuid_count(leaf, subleaf, &reg[EAX], &reg[EBX], &reg[ECX], &reg[EDX]) == 1;
#elif defined(HAVE__CPUIDEX)
	__cpuidex((int *) reg, leaf, subleaf);
	return true;
#else
	memset(reg, 0, 4 * sizeof(unsigned int));
	return false;
#endif
}

/*
 * Parse the CPU ID info for runtime checks.
 */
#ifdef HAVE_XSAVE_INTRINSICS
pg_attribute_target("xsave")
#endif
void
set_x86_features(void)
{
	unsigned int reg[4] = {0};
	bool		have_osxsave;

	pg_cpuid(0x01, reg);

	X86Features[PG_SSE4_2] = reg[ECX] >> 20 & 1;
	X86Features[PG_POPCNT] = reg[ECX] >> 23 & 1;
	X86Features[PG_HYPERVISOR] = reg[ECX] >> 31 & 1;
	have_osxsave = reg[ECX] & (1 << 27);

	pg_cpuid_subleaf(0x07, 0, reg);

	X86Features[PG_TSC_ADJUST] = (reg[EBX] & (1 << 1)) != 0;

	/* leaf 7 features that depend on OSXSAVE */
	if (have_osxsave)
	{
		uint32		xcr0_val = 0;

#ifdef HAVE_XSAVE_INTRINSICS
		/* get value of Extended Control Register */
		xcr0_val = _xgetbv(0);
#endif

		/* Are ZMM registers enabled? */
		if (mask_available(xcr0_val, XMM | YMM |
						   OPMASK | ZMM0_15 | ZMM16_31))
		{
			X86Features[PG_AVX512_BW] = reg[EBX] >> 30 & 1;
			X86Features[PG_AVX512_VL] = reg[EBX] >> 31 & 1;

			X86Features[PG_AVX512_VPCLMULQDQ] = reg[ECX] >> 10 & 1;
			X86Features[PG_AVX512_VPOPCNTDQ] = reg[ECX] >> 14 & 1;
		}
	}

	/* Check for other TSC related flags */
	pg_cpuid(0x80000001, reg);
	X86Features[PG_RDTSCP] = reg[EDX] >> 27 & 1;

	pg_cpuid(0x80000007, reg);
	X86Features[PG_TSC_INVARIANT] = reg[EDX] >> 8 & 1;

	X86Features[INIT_PG_X86] = true;
}

/*
 * Return the number of logical processors per physical CPU package (socket).
 *
 * This uses CPUID.0B (Extended Topology Enumeration) to enumerate topology
 * levels. Each sub-leaf reports a level type in ECX[15:8] (1 = SMT, 2 = Core)
 * and the number of logical processors at that level and below in EBX[15:0].
 * The value at the highest level gives us logical processors per package.
 *
 * Vendor-specific leaves (0x1F for Intel, 0x80000026 for AMD) provide
 * finer-grained sub-package topology but are assumed to report the same
 * per-package totals on current hardware.
 *
 * Returns 0 if topology information is not available.
 */
int
x86_logical_processors_per_package(void)
{
	int			logical_per_package = 0;

	for (int subleaf = 0; subleaf < 8; subleaf++)
	{
		unsigned int r[4] = {0};
		uint32		level_type;

		if (!pg_cpuid_subleaf(0x0B, subleaf, r))
			return 0;

		level_type = (r[ECX] >> 8) & 0xff;

		/* level_type 0 means end of enumeration */
		if (level_type == 0)
			break;

		logical_per_package = r[EBX] & 0xffff;
	}

	return logical_per_package;
}

/* TSC (Time-stamp Counter) handling code */

static uint32 x86_hypervisor_tsc_frequency_khz(void);

/*
 * Determine the TSC frequency of the CPU, where supported.
 *
 * Needed to interpret the tick value returned by RDTSC/RDTSCP. Return value of
 * 0 indicates TSC is not invariant, or the frequency information was not
 * accessible and the instructions should not be used.
 */
uint32
x86_tsc_frequency_khz(void)
{
	unsigned int r[4] = {0};

	if (!x86_feature_available(PG_TSC_INVARIANT))
		return 0;

	if (x86_feature_available(PG_HYPERVISOR))
		return x86_hypervisor_tsc_frequency_khz();

	/*
	 * On modern Intel CPUs, the TSC is implemented by invariant timekeeping
	 * hardware, also called "Always Running Timer", or ART. The ART stays
	 * consistent even if the CPU changes frequency due to changing power
	 * levels.
	 *
	 * As documented in "Determining the Processor Base Frequency" in the
	 * "Intel® 64 and IA-32 Architectures Software Developer's Manual",
	 * February 2026 Edition, we can get the TSC frequency as follows:
	 *
	 * Nominal TSC frequency = ( CPUID.15H:ECX[31:0] * CPUID.15H:EBX[31:0] ) /
	 * CPUID.15H:EAX[31:0]
	 *
	 * With CPUID.15H:ECX representing the nominal core crystal clock
	 * frequency, and EAX/EBX representing values used to translate the TSC
	 * value to that frequency, see "Chapter 20.17 "Time-Stamp Counter" of
	 * that manual.
	 *
	 * Older Intel CPUs, and other vendors do not set CPUID.15H:ECX, and as
	 * such we fall back to alternate approaches.
	 */
	pg_cpuid(0x15, r);
	if (r[ECX] > 0)
	{
		/*
		 * EBX not being set indicates invariant TSC is not available. Require
		 * EAX being non-zero too, to avoid a theoretical divide by zero.
		 */
		if (r[EAX] == 0 || r[EBX] == 0)
			return 0;

		return r[ECX] / 1000 * r[EBX] / r[EAX];
	}

	/*
	 * When CPUID.15H is not available/incomplete, but we have verified an
	 * invariant TSC is used, we can instead get the processor base frequency
	 * in MHz from CPUID.16H:EAX, the "Processor Frequency Information Leaf".
	 */
	pg_cpuid(0x16, r);
	if (r[EAX] > 0)
		return r[EAX] * 1000;

	return 0;
}

/*
 * Support for reading TSC frequency for hypervisors passing it to a guest VM.
 *
 * Two Hypervisors (VMware and KVM) are known to make TSC frequency in KHz
 * available at the vendor-specific 0x40000010 leaf in the EAX register.
 *
 * For some other Hypervisors that have an invariant TSC, e.g. HyperV, we would
 * need to access an MSR to get the frequency (which is typically not available
 * for unprivileged processes), so we instead rely on the TSC calibration logic.
 */
#define CPUID_HYPERVISOR_VMWARE(r) (r[EBX] == 0x61774d56 && r[ECX] == 0x4d566572 && r[EDX] == 0x65726177)	/* VMwareVMware */
#define CPUID_HYPERVISOR_KVM(r) (r[EBX] == 0x4b4d564b && r[ECX] == 0x564b4d56 && r[EDX] == 0x0000004d)	/* KVMKVMKVM */
static uint32
x86_hypervisor_tsc_frequency_khz(void)
{
	unsigned int r[4] = {0};

#if defined(HAVE__CPUIDEX)

	/*
	 * The hypervisor is determined using the 0x40000000 Hypervisor
	 * information leaf, which requires use of __cpuidex to set ECX to 0 to
	 * access it.
	 *
	 * The similar __get_cpuid_count function does not work as expected since
	 * it contains a check for __get_cpuid_max, which has been observed to be
	 * lower than the special Hypervisor leaf, despite it being available.
	 */
	__cpuidex((int *) r, 0x40000000, 0);

	if (r[EAX] >= 0x40000010 && (CPUID_HYPERVISOR_VMWARE(r) || CPUID_HYPERVISOR_KVM(r)))
	{
		__cpuidex((int *) r, 0x40000010, 0);
		if (r[EAX] > 0)
			return r[EAX];
	}
#endif							/* HAVE__CPUIDEX */

	return 0;
}


#endif							/* defined(USE_SSE2) || defined(__i386__) */

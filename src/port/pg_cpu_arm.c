/*-------------------------------------------------------------------------
 *
 * pg_cpu_arm.c
 *	  Runtime CPU feature detection for AArch64
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_cpu_arm.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#if defined(__aarch64__) && !defined(WIN32)

#include "port/pg_cpu.h"

/*
 * Return the frequency of the ARM generic timer (CNTVCT_EL0) in kHz.
 *
 * The CNTFRQ_EL0 system register is architecturally guaranteed to be readable
 * from EL0 (userspace) and holds the timer frequency in Hz. The firmware sets
 * this at boot and it does not change.
 *
 * Returns 0 if the frequency is not available (should not happen on conforming
 * implementations).
 */
uint32
aarch64_cntvct_frequency_khz(void)
{
	uint64		freq;

	freq = __builtin_arm_rsr64("cntfrq_el0");

	if (freq == 0)
		return 0;

	return (uint32) (freq / 1000);
}

#endif							/* defined(__aarch64__) */

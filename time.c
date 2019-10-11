/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Wei Chen <Wei.Chen@arm.com>
 *
 * Copyright (c) 2018, Arm Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THIS HEADER MAY NOT BE EXTRACTED OR MODIFIED IN ANY WAY.
 */
#include <stdlib.h>
#include <uk/assert.h>
#include <uk/plat/time.h>
#include <uk/plat/lcpu.h>
#include <uk/plat/irq.h>
#include <cpu.h>
#include <irq.h>
#include <arm/time.h>


#include <raspi/irq.h>
#include <raspi/time.h>
#include <uk/print.h>

static uint64_t boot_ticks;
static uint32_t counter_freq;


/* Shift factor for converting ticks to ns */
static uint8_t counter_shift_to_ns;

/* Shift factor for converting ns to ticks */
static uint8_t counter_shift_to_tick;

/* Multiplier for converting counter ticks to nsecs */
static uint32_t ns_per_tick;

/* Multiplier for converting nsecs to counter ticks */
static uint32_t tick_per_ns;

/*
 * The maximum time range in seconds which can be converted by multiplier
 * and shift factors. This will guarantee the converted value not to exceed
 * 64-bit unsigned integer. Increase the time range will reduce the accuracy
 * of conversion, because we will get smaller multiplier and shift factors.
 * In this case, we selected 3600s as the time range.
 */
#define __MAX_CONVERT_SECS	(3600UL)
#define __MAX_CONVERT_NS	(3600UL*NSEC_PER_SEC)
static uint64_t max_convert_ticks;

/* How many nanoseconds per second */
#define NSEC_PER_SEC ukarch_time_sec_to_nsec(1)

static inline uint64_t ticks_to_ns(uint64_t ticks)
{
	//UK_ASSERT(ticks <= max_convert_ticks);

	return (ns_per_tick * ticks) >> counter_shift_to_ns;
}

static inline uint64_t ns_to_ticks(uint64_t ns)
{
	//UK_ASSERT(ns <= __MAX_CONVERT_NS);

	return (tick_per_ns * ns) >> counter_shift_to_tick;
}

/*
 * Calculate multiplier/shift factors for scaled math.
 */
static void calculate_mult_shift(uint32_t *mult, uint8_t *shift,
		uint64_t from, uint64_t to)
{
	uint64_t tmp;
	uint32_t sft, sftacc = 32;

	/*
	 * Calculate the shift factor which is limiting the conversion
	 * range:
	 */
	tmp = ((uint64_t)__MAX_CONVERT_SECS * from) >> 32;
	while (tmp) {
		tmp >>= 1;
		sftacc--;
	}


	/*
	 * Calculate shift factor (S) and scaling multiplier (M).
	 *
	 * (S) needs to be the largest shift factor (<= max_shift) where
	 * the result of the M calculation below fits into uint32_t
	 * without truncation.
	 *
	 * multiplier = (target << shift) / source
	 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (uint64_t) to << sft;

		/* Ensuring we round to nearest when calculating the
		 * multiplier
		 */
		tmp += from / 2;
		tmp /= from;
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = tmp;
	*shift = sft;
}

static inline void generic_timer_enable(void)
{
	set_el0(cntv_ctl, get_el0(cntv_ctl) | GT_TIMER_ENABLE);

	/* Ensure the write of sys register is visible */
	isb();
}

static inline void generic_timer_disable(void)
{
	set_el0(cntv_ctl, get_el0(cntv_ctl) & ~GT_TIMER_ENABLE);

	/* Ensure the write of sys register is visible */
	isb();
}

static inline void generic_timer_mask_irq(void)
{
	set_el0(cntv_ctl, get_el0(cntv_ctl) | GT_TIMER_MASK_IRQ);

	/* Ensure the write of sys register is visible */
	isb();
}

static inline void generic_timer_unmask_irq(void)
{
	set_el0(cntv_ctl, get_el0(cntv_ctl) & ~GT_TIMER_MASK_IRQ);

	/* Ensure the write of sys register is visible */
	isb();
}

static inline void generic_timer_clear_irq(void)
{
	set_el0(cntv_ctl, get_el0(cntv_ctl) & ~GT_TIMER_IRQ_STATUS);

	/* Ensure the write of sys register is visible */
	isb();
}

static inline void generic_timer_update_compare(uint64_t new_val)
{
	set_el0(cntv_cval, new_val);

	/* Ensure the write of sys register is visible */
	isb();
}

#ifdef CONFIG_ARM64_ERRATUM_858921
/*
 * The errata #858921 describes that Cortex-A73 (r0p0 - r0p2) counter
 * read can return a wrong value when the counter crosses a 32bit boundary.
 * But newer Cortex-A73 are not affected.
 *
 * The workaround involves performing the read twice, compare bit[32] of
 * the two read values. If bit[32] is different, keep the first value,
 * otherwise keep the second value.
 */
static uint64_t generic_timer_get_ticks(void)
{
	uint64_t val_1st, val_2nd;

	val_1st = get_el0(cntvct);
	val_2nd = get_el0(cntvct);
	return (((val_1st ^ val_2nd) >> 32) & 1) ? val_1st : val_2nd;
}
#else
static inline uint64_t generic_timer_get_ticks(void)
{
	return get_el0(cntvct);
}
#endif

static uint32_t generic_timer_get_frequency(void)
{
	return get_el0(cntfrq);
}

/*
 * monotonic_clock(): returns # of nanoseconds passed since
 * generic_timer_time_init()
 */
static __nsec generic_timer_monotonic(void)
{
	return (__nsec)ticks_to_ns(generic_timer_get_ticks() - boot_ticks);
}

/*
 * Return epoch offset (wall time offset to monotonic clock start).
 */
static uint64_t generic_timer_epochoffset(void)
{
	return 0;
}

/*
 * Returns early if any interrupts are serviced, or if the requested delay is
 * too short. Must be called with interrupts disabled, will enable interrupts
 * "atomically" during idle loop.
 *
 * This function must be called only from the scheduler. It will screw
 * your system if you do otherwise. And, there is no reason you
 * actually want to use it anywhere else. THIS IS NOT A YIELD or any
 * kind of mutex_lock. It will simply halt the cpu, not allowing any
 * other thread to execute.
 */
void generic_timer_setup_next_interrupt(uint64_t delta)
{
	uint64_t until_ticks;

	//UK_ASSERT(ukplat_lcpu_irqs_disabled());

	until_ticks = generic_timer_get_ticks() + ns_to_ticks(delta);

	generic_timer_update_compare(until_ticks);
	generic_timer_unmask_irq();
}

static int generic_timer_init(void)
{
	/* Get counter frequency from DTB or register */
	counter_freq = generic_timer_get_frequency();

	/*
	 * Calculate the shift factor and scaling multiplier for
	 * converting ticks to ns.
	 */
	calculate_mult_shift(&ns_per_tick, &counter_shift_to_ns,
				counter_freq, NSEC_PER_SEC);

	/* We disallow zero ns_per_tick */
	UK_BUGON(!ns_per_tick);

	/*
	 * Calculate the shift factor and scaling multiplier for
	 * converting ns to ticks.
	 */
	calculate_mult_shift(&tick_per_ns, &counter_shift_to_tick,
				NSEC_PER_SEC, counter_freq);

	/* We disallow zero ns_per_tick */
	UK_BUGON(!tick_per_ns);

	max_convert_ticks = __MAX_CONVERT_SECS*counter_freq;

	return 0;
}

/*void handle_timer_irq(void)
{
	__u64 timerValue = SYSREG_READ64(cntvct_el0);
	generic_timer_mask_irq();
	generic_timer_clear_irq();

	generic_timer_setup_next_interrupt(NSEC_PER_SEC);
}*/

void handle_timer_irq(void)
{
	__u64 timerValue = raspi_arm_side_timer_get_value();
	raspi_arm_side_timer_irq_disable();
	raspi_arm_side_timer_irq_clear();

	uk_pr_debug("Timer IRQ delay: %lu cycles\n", raspi_arm_side_timer_get_load() - timerValue);

	//raspi_arm_side_timer_irq_enable();
}

void time_block_until(__snsec until)
{
	until = until;
	return;
}

/* return ns since time_init() */
__nsec ukplat_monotonic_clock(void)
{
	return generic_timer_monotonic();
}

/* return wall time in nsecs */
__nsec ukplat_wall_clock(void)
{
	return generic_timer_monotonic() + generic_timer_epochoffset();
}

/* must be called before interrupts are enabled */
void ukplat_time_init(void)
{
	int rc;
	//int rc, irq, fdt_timer;
	//uint32_t irq_type, hwirq;
	//uint32_t trigger_type;


	/*
	 * Monotonic time begins at boot_ticks (first read of counter
	 * before calibration).
	 */
	boot_ticks = generic_timer_get_ticks();

	irq_vector_init();

	/* Currently, we only support 1 timer per system */
	//fdt_timer = fdt_node_offset_by_compatible_list(_libkvmplat_cfg.dtb,
	//			-1, arch_timer_list);
	//if (fdt_timer < 0)
	//	UK_CRASH("Could not find arch timer!\n");


	rc = generic_timer_init();
	if (rc < 0)
		UK_CRASH("Failed to initialize platform time\n");

	//rc = gic_get_irq_from_dtb(_libkvmplat_cfg.dtb, fdt_timer, 2,
	//		&irq_type, &hwirq, &trigger_type);
	//if (rc < 0)
	//	UK_CRASH("Failed to find IRQ number from DTB\n");

	//irq = gic_irq_translate(irq_type, hwirq);
	//if (irq < 0 || irq >= __MAX_IRQ)
	//	UK_CRASH("Failed to translate IRQ number, type=%u, hwirq=%u\n",
	//		irq_type, hwirq);

	//rc = ukplat_irq_register(irq, generic_timer_irq_handler, NULL);
	//if (rc < 0)
	//	UK_CRASH("Failed to register timer interrupt handler\n");

	/*
	 * Mask IRQ before scheduler start working. Otherwise we will get
	 * unexpected timer interrupts when system is booting.
	 */
	generic_timer_mask_irq();

	/* Enable timer */
	generic_timer_enable();

	//enable_irq();
}

/**
 * Get System Timer's counter
 */
__u64 get_system_timer(void)
{
	__u32 h, l;
	// we must read MMIO area as two separate 32 bit reads
	h = *RASPI_SYS_TIMER_CHI;
	l = *RASPI_SYS_TIMER_CLO;
	// we have to repeat it if high word changed during read
	if (h != *RASPI_SYS_TIMER_CHI)
	{
		h = *RASPI_SYS_TIMER_CHI;
		l = *RASPI_SYS_TIMER_CLO;
	}
	// compose long int value
	return ((__u64)h << 32) | l;
}


// SPDX-License-Identifier: GPL-2.0
/*
 * ESP32-P4 Timer Driver for BenVisor Native Linux
 *
 * This replaces timer-clint.c for the ESP32-P4 which has no standard CLINT.
 *
 * Clocksource: reads RISC-V CSR time/timeh directly (360 MHz on P4).
 * Clock events: writes mtimecmp to a shared SRAM location. BenVisor's
 *   systimer ISR polls mtimecmp and injects CLIC int 7 when the deadline
 *   passes. Linux sees a standard machine timer interrupt (cause 7).
 *
 * DT compatible: "benos,esp32p4-timer"
 * DT properties:
 *   reg            = <MTIMECMP_SRAM_ADDR 8>  (lo + hi, 8 bytes)
 *   timebase-frequency = <360000000>
 *   interrupts-extended = <&intc 7>
 *
 * Hardware context (see 21_BENLINUX_SESSION_HANDOFF.md):
 *   - CSR time (0xC01) ticks at 360 MHz (CPU clock)
 *   - No CLINT MMIO exists on P4 (confirmed: store fault at 0x02004000)
 *   - BenVisor owns systimer Alarm 1 (16 MHz, 0x500E2000) and CLIC mtvt
 *   - BenVisor injects timer via CLIC int 7 pending bit (0x2080101C)
 *   - CLIC mcause = 0xB8000007 (needs irq-riscv-intc.c 12-bit mask patch)
 */

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <asm/csr.h>

/* Shared SRAM: BenVisor reads, Linux writes */
static volatile u32 __iomem *mtimecmp_lo;
static volatile u32 __iomem *mtimecmp_hi;

/* Per-CPU clock event device (single CPU for NOMMU) */
static struct clock_event_device esp32p4_ce;

/* ========================================
 * Clocksource: CSR time (64-bit, 360 MHz)
 *
 * Unlike timer-clint.c which reads mtime from MMIO,
 * we read directly from CSR time/timeh. On P4, this
 * returns the CPU cycle counter at 360 MHz.
 * ======================================== */

static u64 esp32p4_read_time(void)
{
	u32 hi, lo;

	/* RV32 64-bit read: re-read hi to detect lo rollover */
	do {
		hi = csr_read(CSR_TIMEH);
		lo = csr_read(CSR_TIME);
	} while (hi != csr_read(CSR_TIMEH));

	return ((u64)hi << 32) | lo;
}

static u64 esp32p4_clocksource_read(struct clocksource *cs)
{
	return esp32p4_read_time();
}

static u64 notrace esp32p4_sched_clock(void)
{
	return esp32p4_read_time();
}

static struct clocksource esp32p4_cs = {
	.name		= "esp32p4-timer",
	.rating		= 400,
	.read		= esp32p4_clocksource_read,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

/* ========================================
 * Clock events: SRAM mtimecmp
 *
 * Linux writes the next deadline to mtimecmp in SRAM.
 * BenVisor polls this value from its systimer ISR.
 * When CSR time >= mtimecmp, BenVisor sets CLIC int 7
 * pending. Hardware delivers the interrupt to Linux.
 * ======================================== */

static int esp32p4_set_next_event(unsigned long delta,
				  struct clock_event_device *ce)
{
	u64 next = esp32p4_read_time() + delta;

	/*
	 * Write lo THEN hi. BenVisor reads hi first in its ISR,
	 * so writing lo first avoids a race where BenVisor sees
	 * new hi with old lo (which could be in the past).
	 *
	 * Worst case: BenVisor sees old hi + new lo → deadline
	 * appears to be in the far past → injects one extra
	 * timer interrupt → Linux handles it (writes max to
	 * mtimecmp in its handler) → harmless.
	 */
	writel(next & 0xFFFFFFFF, mtimecmp_lo);
	writel(next >> 32, mtimecmp_hi);

	return 0;
}

static int esp32p4_timer_shutdown(struct clock_event_device *ce)
{
	/* Write max to disable: time can never reach 0xFFFFFFFF_FFFFFFFF */
	writel(0xFFFFFFFF, mtimecmp_lo);
	writel(0xFFFFFFFF, mtimecmp_hi);
	return 0;
}

/* ========================================
 * Timer interrupt handler
 *
 * Called when CLIC int 7 fires (machine timer).
 * BenVisor injected this by setting CLIC int 7 IP.
 * ======================================== */

static irqreturn_t esp32p4_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = dev_id;

	/*
	 * Disable timer events until the next one is programmed.
	 * This prevents BenVisor from re-injecting while Linux
	 * processes this tick. The clock_event framework will call
	 * set_next_event() to program the next deadline.
	 */
	writel(0xFFFFFFFF, mtimecmp_lo);
	writel(0xFFFFFFFF, mtimecmp_hi);

	ce->event_handler(ce);
	return IRQ_HANDLED;
}

/* ========================================
 * Initialization (from device tree)
 * ======================================== */

static int __init esp32p4_timer_init_dt(struct device_node *np)
{
	int irq, ret;
	u32 freq;
	void __iomem *base;

	/* Map mtimecmp SRAM region from DT (8 bytes: lo + hi) */
	base = of_iomap(np, 0);
	if (!base) {
		pr_err("esp32p4-timer: failed to map mtimecmp\n");
		return -ENOMEM;
	}

	mtimecmp_lo = (volatile u32 __iomem *)base;
	mtimecmp_hi = (volatile u32 __iomem *)(base + 4);

	/* Read timebase frequency from DT */
	if (of_property_read_u32(np, "timebase-frequency", &freq)) {
		pr_err("esp32p4-timer: missing timebase-frequency\n");
		return -EINVAL;
	}

	/* Disable timer initially */
	writel(0xFFFFFFFF, mtimecmp_lo);
	writel(0xFFFFFFFF, mtimecmp_hi);

	/* Register clocksource */
	ret = clocksource_register_hz(&esp32p4_cs, freq);
	if (ret) {
		pr_err("esp32p4-timer: clocksource failed: %d\n", ret);
		return ret;
	}

	sched_clock_register(esp32p4_sched_clock, 64, freq);

	/* Parse timer interrupt (IRQ 7 = machine timer) */
	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		pr_err("esp32p4-timer: no interrupt in DT\n");
		return -EINVAL;
	}

	/* Configure clock event device */
	esp32p4_ce.name		= "esp32p4-timer";
	esp32p4_ce.features	= CLOCK_EVT_FEAT_ONESHOT;
	esp32p4_ce.rating	= 400;
	esp32p4_ce.cpumask	= cpumask_of(0);
	esp32p4_ce.set_next_event	= esp32p4_set_next_event;
	esp32p4_ce.set_state_shutdown	= esp32p4_timer_shutdown;
	esp32p4_ce.set_state_oneshot	= esp32p4_timer_shutdown;

	ret = request_irq(irq, esp32p4_timer_interrupt,
			  IRQF_TIMER | IRQF_IRQPOLL,
			  "esp32p4-timer", &esp32p4_ce);
	if (ret) {
		pr_err("esp32p4-timer: IRQ %d request failed: %d\n", irq, ret);
		return ret;
	}

	clockevents_config_and_register(&esp32p4_ce, freq,
					100,		/* min delta ticks */
					0x7FFFFFFF);	/* max delta ticks */

	pr_info("esp32p4-timer: registered (%u MHz, IRQ %d)\n",
		freq / 1000000, irq);
	return 0;
}

TIMER_OF_DECLARE(esp32p4_timer, "benos,esp32p4-timer", esp32p4_timer_init_dt);

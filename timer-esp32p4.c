// SPDX-License-Identifier: GPL-2.0
/*
 * ESP32-P4 Timer Driver — Direct Systimer (No BenVisor ISR)
 *
 * Architecture: the systimer alarm 1 interrupt is routed through the
 * ESP32-P4 interrupt matrix to CLIC index 24. The kernel handles this
 * interrupt directly — no BenVisor ISR in the loop.
 *
 * Clocksource: CSR time (360 MHz, CPU cycle counter)
 * Clock events: systimer alarm 1 (16 MHz, one-shot mode)
 *
 * BenVisor sets up: interrupt matrix routing, CLIC24 config, initial alarm.
 * This driver takes over: programs alarms, handles interrupts, clears INT_ST.
 *
 * DT compatible: "benos,esp32p4-timer"
 * DT properties:
 *   reg               = <0x500E2000 0x100>  (systimer base)
 *   timebase-frequency = <360000000>         (CPU clock for clocksource)
 *   interrupts-extended = <&intc 24>         (CLIC index 24)
 */

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/sched_clock.h>
#include <asm/csr.h>

/* ========================================
 * Systimer Hardware Registers
 *
 * ESP32-P4 systimer at 16 MHz (XTAL 40 MHz ÷ 2.5).
 * One-shot alarm mode — periodic mode is broken on P4.
 * Addresses hardcoded (chip-specific, M-mode direct access).
 * ======================================== */

#define SYSTIMER_BASE           0x500E2000

#define ST_CONF                 (SYSTIMER_BASE + 0x00)
#define ST_UNIT0_OP             (SYSTIMER_BASE + 0x04)
#define ST_TARGET1_HI           (SYSTIMER_BASE + 0x24)
#define ST_TARGET1_LO           (SYSTIMER_BASE + 0x28)
#define ST_TARGET1_CONF         (SYSTIMER_BASE + 0x38)
#define ST_UNIT0_VAL_HI         (SYSTIMER_BASE + 0x40)
#define ST_UNIT0_VAL_LO         (SYSTIMER_BASE + 0x44)
#define ST_COMP1_LOAD           (SYSTIMER_BASE + 0x54)
#define ST_INT_ENA              (SYSTIMER_BASE + 0x64)
#define ST_INT_CLR              (SYSTIMER_BASE + 0x6C)
#define ST_INT_ST               (SYSTIMER_BASE + 0x70)

#define ST_CONF_TARGET1_EN      (1 << 23)
#define ST_INT_TARGET1          (1 << 1)
#define ST_UNIT0_UPDATE         (1 << 30)

#define SYSTIMER_FREQ_HZ        16000000
#define CPU_FREQ_HZ             360000000

/* Direct MMIO access — M-mode, NOMMU, no ioremap needed */
static inline void st_write(u32 addr, u32 val)
{
	*(volatile u32 *)addr = val;
}

static inline u32 st_read(u32 addr)
{
	return *(volatile u32 *)addr;
}

/* Per-CPU clock event device — must use DEFINE_PER_CPU to match
 * riscv-intc's irq_set_percpu_devid() marking. */
static DEFINE_PER_CPU(struct clock_event_device, esp32p4_ce);

/* ========================================
 * Clocksource: CSR time (64-bit, 360 MHz)
 * ======================================== */

static u64 esp32p4_read_time(void)
{
	u32 hi, lo;

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
 * Clock Events: Systimer Alarm 1 (one-shot)
 *
 * The kernel programs the next event in systimer ticks (16 MHz).
 * The clockevent is registered at 16 MHz so delta is already
 * in systimer ticks — no CPU/systimer conversion needed.
 * ======================================== */

static int esp32p4_set_next_event(unsigned long delta,
				  struct clock_event_device *ce)
{
	u32 conf;

	/* Disable alarm (clear TARGET1_EN, resets internal latch) */
	conf = st_read(ST_CONF);
	st_write(ST_CONF, conf & ~ST_CONF_TARGET1_EN);

	/* Read current systimer counter */
	st_write(ST_UNIT0_OP, ST_UNIT0_UPDATE);
	{
		u32 now_lo = st_read(ST_UNIT0_VAL_LO);
		u32 now_hi = st_read(ST_UNIT0_VAL_HI);

		/* Set target = now + delta (16 MHz ticks) */
		u64 target = ((u64)now_hi << 32) | now_lo;
		target += delta;
		st_write(ST_TARGET1_HI, (u32)(target >> 32));
		st_write(ST_TARGET1_LO, (u32)(target & 0xFFFFFFFF));
	}

	/* Load comparator */
	st_write(ST_COMP1_LOAD, 1);

	/* Clear any stale INT_ST before re-enabling */
	st_write(ST_INT_CLR, ST_INT_TARGET1);

	/* Re-enable alarm (0→1 transition restarts comparison) */
	st_write(ST_CONF, conf | ST_CONF_TARGET1_EN);

	return 0;
}

static int esp32p4_timer_shutdown(struct clock_event_device *ce)
{
	u32 conf = st_read(ST_CONF);
	st_write(ST_CONF, conf & ~ST_CONF_TARGET1_EN);
	st_write(ST_INT_CLR, ST_INT_TARGET1);
	return 0;
}

/* ========================================
 * Timer Interrupt Handler
 *
 * Called when systimer alarm 1 fires via CLIC index 24.
 * Clears the interrupt source and lets the clockevent
 * framework program the next event.
 * ======================================== */

static irqreturn_t esp32p4_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = this_cpu_ptr(&esp32p4_ce);

	/* Clear systimer INT_ST (must clear source before mret
	 * since CLIC24 is edge-triggered — the clear+re-arm in
	 * set_next_event creates the next rising edge) */
	st_write(ST_INT_CLR, ST_INT_TARGET1);

	/* Disable alarm until next event is programmed.
	 * set_next_event will re-enable it. */
	{
		u32 conf = st_read(ST_CONF);
		st_write(ST_CONF, conf & ~ST_CONF_TARGET1_EN);
	}

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

	pr_info("esp32p4-timer: init starting (direct systimer mode)\n");

	/* Read CPU timebase frequency from DT (for clocksource) */
	if (of_property_read_u32(np, "timebase-frequency", &freq)) {
		pr_err("esp32p4-timer: missing timebase-frequency\n");
		return -EINVAL;
	}
	pr_info("esp32p4-timer: CPU freq=%u Hz, systimer freq=%u Hz\n",
		freq, SYSTIMER_FREQ_HZ);

	/* Confirm systimer is running: read counter */
	st_write(ST_UNIT0_OP, ST_UNIT0_UPDATE);
	pr_info("esp32p4-timer: systimer counter = 0x%08x_%08x\n",
		st_read(ST_UNIT0_VAL_HI), st_read(ST_UNIT0_VAL_LO));

	/* Ensure alarm 1 configured: one-shot, counter 0 */
	st_write(ST_TARGET1_CONF, 0);  /* unit_sel=0, period_mode=0 */

	/* Disable alarm initially */
	{
		u32 conf = st_read(ST_CONF);
		st_write(ST_CONF, conf & ~ST_CONF_TARGET1_EN);
	}
	st_write(ST_INT_CLR, ST_INT_TARGET1);

	/* Ensure alarm 1 interrupt is enabled in systimer */
	{
		u32 int_ena = st_read(ST_INT_ENA);
		st_write(ST_INT_ENA, int_ena | ST_INT_TARGET1);
	}
	pr_info("esp32p4-timer: systimer alarm 1 configured (one-shot)\n");

	/* Register clocksource at CPU frequency (360 MHz) */
	ret = clocksource_register_hz(&esp32p4_cs, freq);
	if (ret) {
		pr_err("esp32p4-timer: clocksource failed: %d\n", ret);
		return ret;
	}
	pr_info("esp32p4-timer: clocksource registered (360 MHz)\n");

	sched_clock_register(esp32p4_sched_clock, 64, freq);
	pr_info("esp32p4-timer: sched_clock registered\n");

	/* Parse interrupt (CLIC index 24 = systimer via interrupt matrix) */
	irq = irq_of_parse_and_map(np, 0);
	pr_info("esp32p4-timer: irq_of_parse_and_map returned %d\n", irq);
	if (!irq) {
		pr_err("esp32p4-timer: no interrupt in DT\n");
		return -EINVAL;
	}

	/* Configure per-CPU clock event at SYSTIMER frequency (16 MHz).
	 * Delta values from the clockevent framework are in systimer ticks.
	 * No CPU→systimer conversion needed in set_next_event. */
	{
		struct clock_event_device *ce = this_cpu_ptr(&esp32p4_ce);
		ce->name		= "esp32p4-timer";
		ce->features		= CLOCK_EVT_FEAT_ONESHOT;
		ce->rating		= 400;
		ce->cpumask		= cpumask_of(0);
		ce->set_next_event	= esp32p4_set_next_event;
		ce->set_state_shutdown	= esp32p4_timer_shutdown;
		ce->set_state_oneshot	= esp32p4_timer_shutdown;
	}

	/* Skip calibrate_delay busy-loop */
	lpj_fine = freq / HZ;

	/* Request percpu IRQ (riscv-intc marks all as per_cpu_devid) */
	pr_info("esp32p4-timer: calling request_percpu_irq(%d)\n", irq);
	ret = request_percpu_irq(irq, esp32p4_timer_interrupt,
				 "esp32p4-timer", &esp32p4_ce);
	if (ret) {
		pr_err("esp32p4-timer: percpu IRQ %d request failed: %d\n",
		       irq, ret);
		return ret;
	}

	/* Register clockevent at SYSTIMER frequency */
	{
		struct clock_event_device *ce = this_cpu_ptr(&esp32p4_ce);
		clockevents_config_and_register(ce, SYSTIMER_FREQ_HZ,
						100,		/* min delta */
						0x7FFFFFFF);	/* max delta */
	}

	enable_percpu_irq(irq, IRQ_TYPE_NONE);

	pr_info("esp32p4-timer: fully registered (clocksource=%u MHz, events=%u MHz, IRQ %d)\n",
		freq / 1000000, SYSTIMER_FREQ_HZ / 1000000, irq);

	/* Diagnostic: CLIC24 state from kernel context */
	{
		u32 mtvec_val;
		u8 clic24_ip, clic24_ie, clic24_attr, clic24_ctl;
		asm volatile("csrr %0, mtvec" : "=r"(mtvec_val));
		clic24_ip   = *(volatile u8 *)0x20801060;
		clic24_ie   = *(volatile u8 *)0x20801061;
		clic24_attr = *(volatile u8 *)0x20801062;
		clic24_ctl  = *(volatile u8 *)0x20801063;
		pr_info("esp32p4-timer: mtvec=0x%08x (MODE=%u)\n",
			mtvec_val, mtvec_val & 3);
		pr_info("esp32p4-timer: CLIC24 IP=%u IE=%u ATTR=0x%02x CTL=0x%02x\n",
			clic24_ip, clic24_ie, clic24_attr, clic24_ctl);
		pr_info("esp32p4-timer: systimer int_ena=0x%x int_st=0x%x\n",
			st_read(ST_INT_ENA), st_read(ST_INT_ST));
	}

	return 0;
}

TIMER_OF_DECLARE(esp32p4_timer, "benos,esp32p4-timer", esp32p4_timer_init_dt);

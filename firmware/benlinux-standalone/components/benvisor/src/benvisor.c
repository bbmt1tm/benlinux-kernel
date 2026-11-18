/**
 * @file benvisor.c
 * @brief BenVisor — Hardware setup for native Linux on ESP32-P4 Core 1
 *
 * Standalone version: no BenOS dependencies.
 *
 * Provides:
 *   - benvisor_init():    CLIC, systimer, mtvt, IPC setup (called from Core 1)
 *   - benvisor_cleanup(): Restore hardware state
 *
 * The assembly side (benvisor_asm.S) provides:
 *   - benvisor_timer_isr:        systimer Alarm 1 handler (mtvt[24])
 *   - benvisor_boot_linux:       saves C context, jumps to kernel
 *   - benvisor_return_from_linux: restores C context on shutdown
 *
 * ARCHITECTURE:
 *   Systimer Alarm 1 → CLIC index 24 (SHV=1, level-triggered)
 *   → mtvt[24] → BenVisor timer ISR (10kHz polling)
 *   → polls mtimecmp → injects CLIC int 7 (SHV=0) → kernel handle_exception
 *   This is the proven BenOS approach: BenVisor bridges systimer to
 *   the standard machine timer interrupt (cause 7) that Linux expects.
 */

#include "benvisor.h"
#include <string.h>
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"

/* ========================================
 * Saved Hardware State (for cleanup)
 * ======================================== */

static uint32_t s_saved_clic_int7_ctrl = 0;
static uint32_t s_saved_clic_int24_ctrl = 0;
static uint32_t s_saved_intmtx_entry = 0;
static uint32_t s_saved_systimer_conf = 0;
static uint32_t s_saved_systimer_int_ena = 0;
static uint32_t s_saved_systimer_target1_conf = 0;
static uint32_t s_saved_clic_thresh = 0;

/* ========================================
 * MMIO Helpers
 * ======================================== */

static inline void mmio_write32(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write8(uint32_t addr, uint8_t val)
{
    *(volatile uint8_t *)addr = val;
}

static inline uint8_t mmio_read8(uint32_t addr)
{
    return *(volatile uint8_t *)addr;
}

static inline void mmio_set_bits32(uint32_t addr, uint32_t bits)
{
    mmio_write32(addr, mmio_read32(addr) | bits);
}

static inline void mmio_clear_bits32(uint32_t addr, uint32_t bits)
{
    mmio_write32(addr, mmio_read32(addr) & ~bits);
}

/* ========================================
 * BenVisor Init — Configure All Hardware
 *
 * Must be called from Core 1 with MIE=0.
 * ======================================== */

void benvisor_init(benvisor_ipc_t *ipc)
{
    esp_rom_printf("[BENVISOR] step 1: saving state\n");

    /* ---- Save original hardware state ---- */
    s_saved_clic_int7_ctrl = mmio_read32(0x2080101C);
    s_saved_clic_int24_ctrl = mmio_read32(TIMER_CLIC_CTRL_REG);
    s_saved_intmtx_entry = mmio_read32(INTMTX_SYSTIMER1_CORE1);
    s_saved_systimer_conf = mmio_read32(SYSTIMER_CONF);
    s_saved_systimer_int_ena = mmio_read32(SYSTIMER_INT_ENA);
    s_saved_systimer_target1_conf = mmio_read32(SYSTIMER_TARGET1_CONF);
    s_saved_clic_thresh = mmio_read32(CLIC_THRESH_REG);

    /* Save mtvec and mtvt */
    asm volatile("csrr %0, mtvec" : "=r"(benvisor_saved_mtvec));
    asm volatile("csrr %0, 0x307" : "=r"(benvisor_saved_mtvt));

    esp_rom_printf("[BENVISOR] step 2: IPC + mtvt\n");

    /* ---- Initialize IPC ---- */
    memset((void *)ipc, 0, sizeof(benvisor_ipc_t));
    ipc->mtimecmp_lo = 0xFFFFFFFF;  /* max = no timer events yet */
    ipc->mtimecmp_hi = 0xFFFFFFFF;
    ipc->linux_state = LINUX_STATE_BOOTING;

    /* Write ROM UART function address for kernel earlycon.
     * The kernel earlycon driver calls this directly — works even with MIE=0. */
    ipc->rom_putc_addr = (uint32_t)esp_rom_output_tx_one_char;

    /* Set assembly globals so the ISR can access IPC and mtimecmp */
    benvisor_ipc_ptr = (uint32_t)ipc;
    benvisor_mtimecmp_ptr = (uint32_t)&ipc->mtimecmp_lo;
    benvisor_heartbeat_ctr = 10000;  /* first heartbeat after 1 second */

    /* ---- Install mtvt table ---- */
    uint32_t mtvt_addr = (uint32_t)benvisor_mtvt_table;
    asm volatile("csrw 0x307, %0" :: "r"(mtvt_addr));

    esp_rom_printf("[BENVISOR] step 3: systimer CONF\n");

    /* ---- Configure systimer Alarm 1 (ONE-SHOT mode) ---- */

    /* Disable Alarm 1 first */
    mmio_clear_bits32(SYSTIMER_CONF, SYSTIMER_CONF_TARGET1_EN);

    /* Clear any pending interrupt */
    mmio_write32(SYSTIMER_INT_CLR, SYSTIMER_INT_TARGET1);

    /* Configure: Counter 0, ONE-SHOT mode (periodic mode is broken on P4) */
    mmio_write32(SYSTIMER_TARGET1_CONF,
                 (0 << 31) |    /* unit_sel = 0 (Counter 0) */
                 (0 << 30) |    /* period_mode = 0 (ONE-SHOT) */
                 0);

    /* Read current counter and set target = now + 1600 (100µs at 16MHz) */
    mmio_write32(SYSTIMER_UNIT0_OP, SYSTIMER_UNIT0_UPDATE);
    uint32_t now_lo = mmio_read32(SYSTIMER_UNIT0_VAL_LO);
    uint32_t now_hi = mmio_read32(SYSTIMER_UNIT0_VAL_HI);
    mmio_write32(SYSTIMER_TARGET1_LO, now_lo + 1600);
    mmio_write32(SYSTIMER_TARGET1_HI, now_hi);

    /* Apply config */
    mmio_write32(SYSTIMER_COMP1_LOAD, 1);

    /* Enable Alarm 1 interrupt */
    mmio_set_bits32(SYSTIMER_INT_ENA, SYSTIMER_INT_TARGET1);

    /* Enable Alarm 1 in CONF */
    mmio_set_bits32(SYSTIMER_CONF, SYSTIMER_CONF_TARGET1_EN);

    esp_rom_printf("[BENVISOR] step 4: intmtx + CLIC\n");

    /* ---- Configure interrupt matrix ----
     * Route systimer target1 (source 54) → CLIC index 24 on Core 1. */
    uint32_t reg = mmio_read32(INTMTX_SYSTIMER1_CORE1);
    reg = (reg & ~INTMTX_INT_MASK) | TIMER_CLIC_INDEX;
    mmio_write32(INTMTX_SYSTIMER1_CORE1, reg);

    /* ---- Scrub ALL CLIC interrupts ----
     * FreeRTOS left many CLIC entries enabled. Any stale IE=1 + IP=1
     * will livelock after local_irq_enable(). */
    for (int i = 0; i < 48; i++) {
        uint32_t base = CLIC_CTRL_BASE + i * 4;
        mmio_write8(base + 1, 0x00);   /* IE = 0 */
        mmio_write8(base + 0, 0x00);   /* IP = 0 */
        mmio_write8(base + 2, 0x00);   /* ATTR = 0 (SHV=0, level) */
    }
    esp_rom_printf("[BENVISOR] CLIC scrubbed: all 48 entries disabled\n");

    /* ---- Configure CLIC index 24 (systimer alarm 1 → BenVisor ISR) ----
     * SHV  = 1  : vectored → mtvt[24] = benvisor_timer_isr.
     * TRIG = 01 : EDGE. This is BenVisor's own 10 kHz heartbeat, the engine
     *             that polls TARGET1 and injects the kernel tick (line 7).
     *             With LEVEL, clicintip[24] followed the systimer INT_ST level
     *             — and at the first U-mode return the level failed to latch
     *             into clicintip[24] (INT_ST=0x2 but ip=0), so the ISR never
     *             re-entered and the whole heartbeat (and thus the kernel tick)
     *             died at /bin/sh. That was the original freeze. EDGE makes the
     *             CLIC latch clicintip[24] on the alarm's rising edge regardless
     *             of privilege, and auto-clear it when the vectored ISR is taken
     *             — the same fix that just made line 7 deliver cleanly.
     * Priority = 7 (max). */
    mmio_write8(TIMER_CLIC_CTRL_REG + 1, 0x00);    /* IE: disable first */
    mmio_write8(TIMER_CLIC_CTRL_REG + 0, 0x00);    /* IP: clear */
    mmio_write8(TIMER_CLIC_CTRL_REG + 2, 0x03);    /* ATTR: SHV=1, EDGE (trig=01) */
    mmio_write8(TIMER_CLIC_CTRL_REG + 3, 0xE0);    /* CTL: priority 7 (max) */
    mmio_write8(TIMER_CLIC_CTRL_REG + 1, 0x01);    /* IE: enable last */

    /* ---- Configure CLIC index 7 (kernel timer tick → kernel via mtvt[7]) ----
     * SHV  = 1  : VECTORED. On this P4 CLIC only the vectored path delivers
     *             (SHV=0 lines latch IP but never reach mtvec — proven). The
     *             hart vectors to mtvt[7] = kernel _start_trap (benvisor_asm.S)
     *             → do_irq → the timer driver's handler (DT rebound to hwirq 7).
     * TRIG = 01 : EDGE. BenVisor injects this tick by writing IP=1 once per
     *             kernel deadline. The kernel's handler clears the SYSTIMER
     *             INT_ST, NOT clicintip[7] (it can't — BenVisor is the artificial
     *             source). With LEVEL, clicintip[7] stayed 1 → kernel re-took it
     *             endlessly → livelock (isr collapsed to 271 at /bin/sh). EDGE
     *             makes the CLIC auto-clear clicintip[7] when the vectored trap
     *             is taken, so it fires exactly once per injection.
     * Priority = 6 (below systimer line 24 so BenVisor's ISR is not preempted).
     * IE = 1. */
    mmio_write8(0x2080101C + 1, 0x00);    /* IE: disable first */
    mmio_write8(0x2080101C + 0, 0x00);    /* IP: clear */
    mmio_write8(0x2080101C + 2, 0x03);    /* ATTR: SHV=1, EDGE (trig=01) */
    mmio_write8(0x2080101C + 3, 0xC0);    /* CTL: priority 6 (below systimer) */
    mmio_write8(0x2080101C + 1, 0x01);    /* IE: enable last */

    /* Set CLIC threshold to 0 (allow all priorities) */
    mmio_write32(CLIC_THRESH_REG, 0x00000000);

    /* Re-enable alarm 0 (Core 0 tick) AND alarm 1 (Linux timer).
     * Alarm 0 keeps FreeRTOS running on Core 0 during boot. */
    mmio_write32(SYSTIMER_INT_CLR, 0x5);                     /* Clear alarm 0+2 INT_ST */
    mmio_set_bits32(SYSTIMER_INT_ENA, (1 << 0) | (1 << 1)); /* alarm 0 + alarm 1 */

    /* Diagnostic */
    {
        uint32_t ie = mmio_read32(SYSTIMER_INT_ENA);
        uint32_t st = mmio_read32(SYSTIMER_INT_ST);
        esp_rom_printf("[BENVISOR] SYSTIMER INT_ENA=0x%lx INT_ST=0x%lx (alarm0=%s)\n",
                       (unsigned long)ie, (unsigned long)st,
                       (ie & 0x1) ? "ALIVE" : "DEAD");
    }

    esp_rom_printf("[BENVISOR] init done\n");
}

/* ========================================
 * BenVisor Cleanup — Restore Hardware
 * ======================================== */

void benvisor_cleanup(void)
{
    /* Disable systimer Alarm 1 */
    mmio_clear_bits32(SYSTIMER_CONF, SYSTIMER_CONF_TARGET1_EN);
    mmio_write32(SYSTIMER_INT_CLR, SYSTIMER_INT_TARGET1);

    /* Disable CLIC entries we configured */
    mmio_write8(0x2080101D, 0x00);    /* int 7 IE = 0 */
    mmio_write8(0x2080101C, 0x00);    /* int 7 IP = 0 */
    mmio_write8(TIMER_CLIC_CTRL_REG + 1, 0x00); /* int 24 IE = 0 */
    mmio_write8(TIMER_CLIC_CTRL_REG + 0, 0x00); /* int 24 IP = 0 */

    /* Restore original hardware configs */
    mmio_write32(0x2080101C, s_saved_clic_int7_ctrl);
    mmio_write32(TIMER_CLIC_CTRL_REG, s_saved_clic_int24_ctrl);
    mmio_write32(CLIC_THRESH_REG, s_saved_clic_thresh);
    mmio_write32(INTMTX_SYSTIMER1_CORE1, s_saved_intmtx_entry);
    mmio_write32(SYSTIMER_TARGET1_CONF, s_saved_systimer_target1_conf);
    mmio_write32(SYSTIMER_COMP1_LOAD, 1);
    mmio_write32(SYSTIMER_INT_ENA, s_saved_systimer_int_ena);
    mmio_write32(SYSTIMER_CONF, s_saved_systimer_conf);

    /* Restore mtvec and mtvt */
    asm volatile("csrw mtvec, %0" :: "r"(benvisor_saved_mtvec));
    asm volatile("csrw 0x307, %0" :: "r"(benvisor_saved_mtvt));

    benvisor_ipc_ptr = 0;
}
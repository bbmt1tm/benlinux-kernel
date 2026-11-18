/**
 * @file benvisor.h
 * @brief BenVisor — M-mode shim for native Linux on ESP32-P4
 *
 * Standalone version: no BenOS dependencies. This header defines:
 *   - Hardware register addresses (systimer, CLIC, interrupt matrix)
 *   - IPC shared memory layout (ring buffers for console I/O)
 *   - Function declarations for init/boot/cleanup
 *
 * All addresses confirmed from ESP-IDF SOC headers for ESP32-P4.
 */

#ifndef BENVISOR_H
#define BENVISOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * Systimer Peripheral
 * Base: 0x500E2000
 * Clock: 16 MHz (40 MHz XTAL / 2.5)
 * ======================================== */

#define SYSTIMER_BASE           0x500E2000

#define SYSTIMER_CONF           (SYSTIMER_BASE + 0x00)
#define SYSTIMER_UNIT0_OP       (SYSTIMER_BASE + 0x04)
#define SYSTIMER_TARGET1_HI     (SYSTIMER_BASE + 0x24)
#define SYSTIMER_TARGET1_LO     (SYSTIMER_BASE + 0x28)
#define SYSTIMER_TARGET1_CONF   (SYSTIMER_BASE + 0x38)
#define SYSTIMER_UNIT0_VAL_HI   (SYSTIMER_BASE + 0x40)
#define SYSTIMER_UNIT0_VAL_LO   (SYSTIMER_BASE + 0x44)
#define SYSTIMER_COMP1_LOAD     (SYSTIMER_BASE + 0x54)
#define SYSTIMER_INT_ENA        (SYSTIMER_BASE + 0x64)
#define SYSTIMER_INT_CLR        (SYSTIMER_BASE + 0x6C)
#define SYSTIMER_INT_ST         (SYSTIMER_BASE + 0x70)

/* CONF register bits */
#define SYSTIMER_CONF_TARGET1_EN    (1 << 23)
#define SYSTIMER_CONF_TARGET0_EN    (1 << 24)
#define SYSTIMER_CONF_TARGET2_EN    (1 << 22)
#define SYSTIMER_CONF_UNIT0_EN      (1 << 30)

/* UNIT0_OP bits */
#define SYSTIMER_UNIT0_UPDATE       (1 << 30)
#define SYSTIMER_UNIT0_VALID        (1 << 29)

/* INT bits for Alarm 1 */
#define SYSTIMER_INT_TARGET1        (1 << 1)

/* Systimer frequency */
#define SYSTIMER_FREQ_HZ            16000000

/* ========================================
 * CLIC (Core-Local Interrupt Controller)
 * Per-hart in CPU subsystem region.
 * Code on Core 1 accesses Core 1's CLIC.
 * ======================================== */

#define CLIC_BASE               0x20800000
#define CLIC_CTRL_BASE          0x20801000

/* Config registers */
#define CLIC_INT_CONFIG         (CLIC_BASE + 0x00)
#define CLIC_INT_INFO           (CLIC_BASE + 0x04)
#define CLIC_THRESH_REG         (CLIC_BASE + 0x08)   /* Non-standard on P4: MMIO */

/* Per-interrupt control: 4 bytes per interrupt at CLIC_CTRL_BASE + index*4
 * Byte 0: IP (pending), Byte 1: IE (enable), Byte 2: ATTR (SHV/trig), Byte 3: CTL (priority) */

/* External interrupts start at CLIC index 16 */
#define CLIC_EXT_INT_OFFSET     16

/* Timer uses CPU interrupt line 8 → CLIC index 24 */
#define TIMER_CPU_INT_LINE      8
#define TIMER_CLIC_INDEX        (TIMER_CPU_INT_LINE + CLIC_EXT_INT_OFFSET)  /* = 24 */

/* CLIC control register for timer interrupt */
#define TIMER_CLIC_CTRL_REG     (CLIC_CTRL_BASE + TIMER_CLIC_INDEX * 4)    /* = 0x20801060 */

/* mtvt CSR number */
#define MTVT_CSR                0x307

/* ========================================
 * Interrupt Matrix
 * Register indexed by peripheral source,
 * value = CLIC index.
 * ======================================== */

#define INTMTX_CORE0_BASE      0x500D6000
#define INTMTX_CORE1_BASE      0x500D6800

/* Systimer target1 interrupt source = 54 */
#define SYSTIMER_TARGET1_SRC    54

/* Routing registers for systimer target1 */
#define INTMTX_SYSTIMER1_CORE0 (INTMTX_CORE0_BASE + 4 * SYSTIMER_TARGET1_SRC) /* 0x500D60D8 */
#define INTMTX_SYSTIMER1_CORE1 (INTMTX_CORE1_BASE + 4 * SYSTIMER_TARGET1_SRC) /* 0x500D68D8 */

/* Mask for CPU interrupt number field */
#define INTMTX_INT_MASK         63

/* ========================================
 * UART0 (for standalone console relay)
 * ======================================== */

#define UART0_BASE              0x500CA000
#define UART0_FIFO              (UART0_BASE + 0x00)
#define UART0_STATUS            (UART0_BASE + 0x1C)  /* UART_STATUS_REG */

/* ========================================
 * Memory Layout
 * ======================================== */

#define LINUX_PAGE_OFFSET       0x48200000  /* Must match kernel CONFIG_PAGE_OFFSET */
#define LINUX_RAM_SIZE          (26 * 1024 * 1024)
#define LINUX_HEAP_EXTRA        0x00300000  /* 3MB headroom: covers gap between heap start and PAGE_OFFSET */

/* SRAM uncached alias offset */
#define SOC_NON_CACHEABLE_OFFSET 0x40000000

/* ========================================
 * IPC Shared Memory
 *
 * Lock-free SPSC ring buffers in SRAM.
 * TX: Core 1 (Linux) writes head, Core 0 reads tail
 * RX: Core 0 writes head, Core 1 (Linux) reads tail
 *
 * Buffer sizes MUST be powers of 2.
 * ======================================== */

#define IPC_TX_BUF_SIZE         2048    /* Linux → host */
#define IPC_TX_BUF_MASK         (IPC_TX_BUF_SIZE - 1)

#define IPC_RX_BUF_SIZE         256     /* host → Linux */
#define IPC_RX_BUF_MASK         (IPC_RX_BUF_SIZE - 1)

typedef struct {
    /* Console TX: Linux → host */
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    char              tx_buf[IPC_TX_BUF_SIZE];

    /* Console RX: host → Linux */
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    char              rx_buf[IPC_RX_BUF_SIZE];

    /* State */
    volatile uint32_t linux_state;

    /* Virtual CLINT mtimecmp (Linux writes, BenVisor ISR reads) */
    volatile uint32_t mtimecmp_lo;
    volatile uint32_t mtimecmp_hi;

    /* ROM UART function address (BenVisor writes, kernel earlycon reads).
     * Points to esp_rom_output_tx_one_char. */
    volatile uint32_t rom_putc_addr;

} benvisor_ipc_t;

/* IPC struct field offsets (for assembly access) */
#define IPC_TX_HEAD_OFF         0
#define IPC_TX_TAIL_OFF         4
#define IPC_TX_BUF_OFF          8
#define IPC_RX_HEAD_OFF         (8 + IPC_TX_BUF_SIZE)           /* 2056 */
#define IPC_RX_TAIL_OFF         (IPC_RX_HEAD_OFF + 4)           /* 2060 */
#define IPC_RX_BUF_OFF          (IPC_RX_TAIL_OFF + 4)           /* 2064 */
#define IPC_STATE_OFF           (IPC_RX_BUF_OFF + IPC_RX_BUF_SIZE)  /* 2320 */
#define IPC_MTIMECMP_LO_OFF    (IPC_STATE_OFF + 4)             /* 2324 */
#define IPC_MTIMECMP_HI_OFF    (IPC_MTIMECMP_LO_OFF + 4)      /* 2328 */
#define IPC_ROM_PUTC_ADDR_OFF  (IPC_MTIMECMP_HI_OFF + 4)      /* 2332 */

/* Linux state values */
#define LINUX_STATE_STOPPED     0
#define LINUX_STATE_BOOTING     1
#define LINUX_STATE_RUNNING     2

/* ========================================
 * mtvt Vector Table
 * 48 entries (16 internal + 32 external)
 * Each entry is a 4-byte function pointer.
 * ======================================== */

#define MTVT_ENTRIES            48

/* ========================================
 * Function Declarations
 * ======================================== */

/**
 * @brief Initialize BenVisor hardware on Core 1
 *
 * Must be called from Core 1 with interrupts disabled.
 * Sets up: systimer alarm, CLIC routing, mtvt table, IPC memory.
 *
 * @param ipc Pointer to IPC shared memory (SRAM)
 */
void benvisor_init(benvisor_ipc_t *ipc);

/**
 * @brief Boot Linux kernel on Core 1
 *
 * Sets up registers and jumps to kernel entry point.
 * Blocks until Linux shuts down (or forever in NOMMU).
 *
 * @param entry_addr  Kernel entry point in PSRAM
 * @param dtb_addr    DTB address in PSRAM
 * @param hart_id     Hart ID to pass to kernel (1 for Core 1)
 * @return 0 on clean shutdown, nonzero on error
 */
int benvisor_boot_linux(uint32_t entry_addr, uint32_t dtb_addr, uint32_t hart_id);

/**
 * @brief Restore Core 1 hardware to pre-init state
 */
void benvisor_cleanup(void);

/* Assembly entry points (defined in benvisor_asm.S) */
extern void benvisor_timer_isr(void);
extern void benvisor_unexpected_isr(void);
extern void benvisor_return_from_linux(void);

/* Globals defined in benvisor_asm.S */
extern uint32_t benvisor_saved_mtvec;
extern uint32_t benvisor_saved_mtvt;
extern uint32_t benvisor_mtimecmp_ptr;
extern uint32_t benvisor_ipc_ptr;
extern uint32_t benvisor_heartbeat_ctr;
extern uint32_t benvisor_mtvt_table[];

#ifdef __cplusplus
}
#endif

#endif /* BENVISOR_H */

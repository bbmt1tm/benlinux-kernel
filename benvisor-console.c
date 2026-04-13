// SPDX-License-Identifier: GPL-2.0
/*
 * BenVisor Console Driver — earlycon + console via ROM UART
 *
 * The earlycon path calls the ESP32-P4 ROM output function directly.
 * BenVisor writes the ROM function address into the IPC struct before
 * kernel entry. This works even with MIE=0 (normal during start_kernel),
 * unlike the previous IPC ring buffer approach which required ISR drain.
 *
 * The full console path (registered after boot) still uses the IPC ring
 * buffer for integration with BenOS WebSocket forwarding.
 *
 * IPC layout (from benvisor.h):
 *   +0x000  tx_head   (uint32, Linux writes)
 *   +0x004  tx_tail   (uint32, BenOS writes)
 *   +0x008  tx_buf    (2048 bytes, circular)
 *   +0x808  rx_head   (uint32, BenOS writes)
 *   +0x80C  rx_tail   (uint32, Linux reads)
 *   +0x810  rx_buf    (256 bytes, circular)
 *   +0x910  linux_state
 *   +0x914  mtimecmp_lo
 *   +0x918  mtimecmp_hi
 *   +0x91C  rom_putc_addr (address of esp_rom_output_tx_one_char)
 *
 * DT: compatible = "benos,benvisor-console", reg = <IPC_BASE SIZE>
 * Bootargs: earlycon=benvisor,0xADDRESS (or OF-matched)
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/serial_core.h>

/* Ring buffer parameters — must match benvisor.h */
#define TX_BUF_SIZE     2048
#define TX_BUF_MASK     (TX_BUF_SIZE - 1)
#define TX_HEAD_OFF     0x000
#define TX_TAIL_OFF     0x004
#define TX_BUF_OFF      0x008

/* ROM function address offset in IPC struct */
#define ROM_PUTC_ADDR_OFF  0x91C

/* ROM putc function type: int esp_rom_output_tx_one_char(uint8_t ch) */
typedef int (*rom_putc_fn_t)(unsigned char);

/* Cached ROM function pointer (set during earlycon setup) */
static rom_putc_fn_t rom_putc_fn;

/* ========================================
 * Direct UART output via ROM function
 *
 * Called from both earlycon and full console.
 * Works with MIE=0 — no ISR dependency.
 * ======================================== */

static void benvisor_putchar_direct(unsigned char ch)
{
	if (rom_putc_fn)
		rom_putc_fn(ch);
}

/* IPC ring buffer write — used by full console for BenOS integration */
static void benvisor_putchar_ipc(void __iomem *base, unsigned char ch)
{
	u32 head = readl_relaxed(base + TX_HEAD_OFF);
	u32 next = (head + 1) & TX_BUF_MASK;
	u32 tail = readl_relaxed(base + TX_TAIL_OFF);

	if (next == tail)
		return;		/* buffer full — drop */

	writeb(ch, base + TX_BUF_OFF + head);
	writel_relaxed(next, base + TX_HEAD_OFF);
}

/* ========================================
 * Earlycon — works from first printk
 *
 * Uses ROM function directly. This is critical because
 * start_kernel() calls local_irq_disable() before any
 * printk output, so the ISR-based IPC drain can't work.
 * ======================================== */

static void benvisor_earlycon_write(struct console *con, const char *s,
				    unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (s[i] == '\n')
			benvisor_putchar_direct('\r');
		benvisor_putchar_direct(s[i]);
	}
}

static int __init benvisor_earlycon_setup(struct earlycon_device *device,
					  const char *options)
{
	void __iomem *base = device->port.membase;
	u32 putc_addr;

	if (!base)
		return -ENODEV;

	/* Read ROM function address from IPC struct */
	putc_addr = readl_relaxed(base + ROM_PUTC_ADDR_OFF);
	if (putc_addr) {
		rom_putc_fn = (rom_putc_fn_t)(uintptr_t)putc_addr;
	}

	device->con->write = benvisor_earlycon_write;
	return 0;
}

OF_EARLYCON_DECLARE(benvisor, "benos,benvisor-console",
		    benvisor_earlycon_setup);

/* ========================================
 * Full console — registered after boot
 *
 * Uses ROM function directly (same as earlycon).
 * Also writes to IPC ring buffer for BenOS WebSocket.
 * ======================================== */

static void __iomem *benvisor_console_base;

static void benvisor_console_write(struct console *co, const char *s,
				   unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (s[i] == '\n') {
			benvisor_putchar_direct('\r');
			if (benvisor_console_base)
				benvisor_putchar_ipc(benvisor_console_base, '\r');
		}
		benvisor_putchar_direct(s[i]);
		if (benvisor_console_base)
			benvisor_putchar_ipc(benvisor_console_base, s[i]);
	}
}

static struct console benvisor_console = {
	.name	= "benvisor",
	.write	= benvisor_console_write,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
};

static int __init benvisor_console_init(void)
{
	struct device_node *np;
	u32 reg[2];

	np = of_find_compatible_node(NULL, NULL, "benos,benvisor-console");
	if (!np)
		return -ENODEV;

	if (of_property_read_u32_array(np, "reg", reg, 2)) {
		of_node_put(np);
		return -EINVAL;
	}
	of_node_put(np);

	/* NOMMU: physical == virtual, no ioremap needed */
	benvisor_console_base = (void __iomem *)(uintptr_t)reg[0];

	/* Also pick up ROM function if earlycon didn't set it */
	if (!rom_putc_fn) {
		u32 putc_addr = readl_relaxed(benvisor_console_base + ROM_PUTC_ADDR_OFF);
		if (putc_addr)
			rom_putc_fn = (rom_putc_fn_t)(uintptr_t)putc_addr;
	}

	register_console(&benvisor_console);
	pr_info("benvisor-console: registered at 0x%08x (rom_putc=%p)\n",
		reg[0], rom_putc_fn);
	return 0;
}
console_initcall(benvisor_console_init);

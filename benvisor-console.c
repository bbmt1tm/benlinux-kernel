// SPDX-License-Identifier: GPL-2.0
/*
 * BenVisor Console Driver — earlycon + console via IPC ring buffer
 *
 * Linux writes to a shared SRAM ring buffer. BenOS on Core 0
 * drains it and forwards to serial/WebSocket.
 *
 * IPC layout (from benvisor.h):
 *   +0x000  tx_head   (uint32, Linux writes)
 *   +0x004  tx_tail   (uint32, BenOS writes)
 *   +0x008  tx_buf    (2048 bytes, circular)
 *   +0x808  rx_head   (uint32, BenOS writes)
 *   +0x80C  rx_tail   (uint32, Linux reads)
 *   +0x810  rx_buf    (256 bytes, circular)
 *
 * DT: compatible = "benos,benvisor-console", reg = <IPC_BASE SIZE>
 * Bootargs: earlycon=benvisor,0xADDRESS (or OF-matched)
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/serial_core.h>

/* Ring buffer parameters — must match benvisor.h */
#define TX_BUF_SIZE     2048
#define TX_BUF_MASK     (TX_BUF_SIZE - 1)
#define TX_HEAD_OFF     0x000
#define TX_TAIL_OFF     0x004
#define TX_BUF_OFF      0x008

static void benvisor_putchar(void __iomem *base, unsigned char ch)
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
 * ======================================== */

static void benvisor_earlycon_write(struct console *con, const char *s,
				    unsigned int count)
{
	struct earlycon_device *dev = con->data;
	void __iomem *base = dev->port.membase;
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (s[i] == '\n')
			benvisor_putchar(base, '\r');
		benvisor_putchar(base, s[i]);
	}
}

static int __init benvisor_earlycon_setup(struct earlycon_device *device,
					  const char *options)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = benvisor_earlycon_write;
	return 0;
}

OF_EARLYCON_DECLARE(benvisor, "benos,benvisor-console",
		    benvisor_earlycon_setup);

/* ========================================
 * Full console — registered after boot
 * ======================================== */

static void __iomem *benvisor_console_base;

static void benvisor_console_write(struct console *co, const char *s,
				   unsigned int count)
{
	unsigned int i;

	if (!benvisor_console_base)
		return;

	for (i = 0; i < count; i++) {
		if (s[i] == '\n')
			benvisor_putchar(benvisor_console_base, '\r');
		benvisor_putchar(benvisor_console_base, s[i]);
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
	struct resource res;

	np = of_find_compatible_node(NULL, NULL, "benos,benvisor-console");
	if (!np)
		return -ENODEV;

	if (of_address_to_resource(np, 0, &res)) {
		of_node_put(np);
		return -EINVAL;
	}
	of_node_put(np);

	benvisor_console_base = ioremap(res.start, resource_size(&res));
	if (!benvisor_console_base)
		return -ENOMEM;

	register_console(&benvisor_console);
	pr_info("benvisor-console: registered at 0x%08x\n",
		(unsigned int)res.start);
	return 0;
}
console_initcall(benvisor_console_init);

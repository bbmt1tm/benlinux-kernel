// SPDX-License-Identifier: GPL-2.0
/*
 * BenVisor Console — earlycon + simple TTY driver
 *
 * Phase 1 (console_initcall): printk console via ROM UART + IPC
 * Phase 2 (device_initcall): tty_driver for /dev/ttyBV0 giving
 *   userspace stdin/stdout via /dev/console.
 *
 * Uses a simple tty_driver — NOT the uart/serial_core framework,
 * which hangs on NOMMU during registration.
 *
 * TX: ROM putc direct + IPC ring buffer
 * RX: Timer polls IPC RX ring buffer every 10ms
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/serial_core.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>

/* TX ring buffer */
#define TX_BUF_SIZE     2048
#define TX_BUF_MASK     (TX_BUF_SIZE - 1)
#define TX_HEAD_OFF     0x000
#define TX_TAIL_OFF     0x004
#define TX_BUF_OFF      0x008

/* RX ring buffer */
#define RX_BUF_SIZE     256
#define RX_BUF_MASK     (RX_BUF_SIZE - 1)
#define RX_HEAD_OFF     0x808
#define RX_TAIL_OFF     0x80C
#define RX_BUF_OFF      0x810

#define ROM_PUTC_ADDR_OFF  0x91C
#define RX_POLL_INTERVAL   (HZ / 100 ?: 1)

typedef int (*rom_putc_fn_t)(unsigned char);

static rom_putc_fn_t rom_putc_fn;
static void __iomem *ipc_base;
static struct timer_list rx_timer;
static struct tty_driver *benvisor_tty_driver;
static struct tty_port benvisor_tty_port;

/* ---- Output helpers ---- */

static void benvisor_putchar_direct(unsigned char ch)
{
	if (rom_putc_fn)
		rom_putc_fn(ch);
}

static void benvisor_putchar_ipc(unsigned char ch)
{
	u32 head, next, tail;

	if (!ipc_base)
		return;
	head = readl_relaxed(ipc_base + TX_HEAD_OFF);
	next = (head + 1) & TX_BUF_MASK;
	tail = readl_relaxed(ipc_base + TX_TAIL_OFF);
	if (next == tail)
		return;
	writeb(ch, ipc_base + TX_BUF_OFF + head);
	writel_relaxed(next, ipc_base + TX_HEAD_OFF);
}

static void benvisor_putchar(unsigned char ch)
{
	benvisor_putchar_direct(ch);
	benvisor_putchar_ipc(ch);
}

/* ---- Earlycon ---- */

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
	putc_addr = readl_relaxed(base + ROM_PUTC_ADDR_OFF);
	if (putc_addr)
		rom_putc_fn = (rom_putc_fn_t)(uintptr_t)putc_addr;
	device->con->write = benvisor_earlycon_write;
	return 0;
}

OF_EARLYCON_DECLARE(benvisor, "benos,benvisor-console",
		    benvisor_earlycon_setup);

/* ---- Phase 1: printk console ---- */

static void benvisor_console_write(struct console *co, const char *s,
				   unsigned int count)
{
	unsigned int i;
	for (i = 0; i < count; i++) {
		if (s[i] == '\n') {
			benvisor_putchar_direct('\r');
			benvisor_putchar_ipc('\r');
		}
		benvisor_putchar_direct(s[i]);
		benvisor_putchar_ipc(s[i]);
	}
}

static struct console benvisor_boot_console = {
	.name    = "benvisor",
	.write   = benvisor_console_write,
	.flags   = CON_PRINTBUFFER,
	.index   = -1,
};

static int __init benvisor_early_console_init(void)
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

	ipc_base = (void __iomem *)(uintptr_t)reg[0];

	if (!rom_putc_fn) {
		u32 putc_addr = readl_relaxed(ipc_base + ROM_PUTC_ADDR_OFF);
		if (putc_addr)
			rom_putc_fn = (rom_putc_fn_t)(uintptr_t)putc_addr;
	}

	register_console(&benvisor_boot_console);
	pr_info("benvisor-console: registered at 0x%08x (rom_putc=%p)\n",
		reg[0], rom_putc_fn);
	return 0;
}
console_initcall(benvisor_early_console_init);

/* ---- Phase 2: TTY driver for userspace ---- */

static int benvisor_tty_open(struct tty_struct *tty, struct file *filp)
{
	mod_timer(&rx_timer, jiffies + RX_POLL_INTERVAL);
	if (rom_putc_fn) {
		rom_putc_fn('['); rom_putc_fn('O'); rom_putc_fn('P');
		rom_putc_fn('E'); rom_putc_fn('N'); rom_putc_fn(']');
		rom_putc_fn('\r'); rom_putc_fn('\n');
	}
	return 0;
}

static void benvisor_tty_close(struct tty_struct *tty, struct file *filp)
{
}

static ssize_t benvisor_tty_write(struct tty_struct *tty,
				  const u8 *buf, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		benvisor_putchar(buf[i]);
	return count;
}

static unsigned int benvisor_tty_write_room(struct tty_struct *tty)
{
	return TX_BUF_SIZE;
}

static const struct tty_operations benvisor_tty_ops = {
	.open       = benvisor_tty_open,
	.close      = benvisor_tty_close,
	.write      = benvisor_tty_write,
	.write_room = benvisor_tty_write_room,
};

static void benvisor_rx_poll(struct timer_list *t)
{
	u32 head, tail;
	int count = 0;
	static int ticks;

	if (!ipc_base)
		goto resched;

	head = readl_relaxed(ipc_base + RX_HEAD_OFF);
	tail = readl_relaxed(ipc_base + RX_TAIL_OFF);

	/* ROM-direct heartbeat every ~5s: proves timer is firing */
	if (++ticks >= 500 && rom_putc_fn) {
		rom_putc_fn('['); rom_putc_fn('T');
		rom_putc_fn(':'); rom_putc_fn('h'); rom_putc_fn('=');
		rom_putc_fn("0123456789abcdef"[(head >> 4) & 0xf]);
		rom_putc_fn("0123456789abcdef"[head & 0xf]);
		rom_putc_fn(' '); rom_putc_fn('t'); rom_putc_fn('=');
		rom_putc_fn("0123456789abcdef"[(tail >> 4) & 0xf]);
		rom_putc_fn("0123456789abcdef"[tail & 0xf]);
		rom_putc_fn(']');
		rom_putc_fn('\r'); rom_putc_fn('\n');
		ticks = 0;
	}

	while (tail != head && count < 64) {
		unsigned char ch = readb(ipc_base + RX_BUF_OFF + tail);
		tail = (tail + 1) & RX_BUF_MASK;
		count++;
		tty_insert_flip_char(&benvisor_tty_port, ch, TTY_NORMAL);
	}

	if (count) {
		writel_relaxed(tail, ipc_base + RX_TAIL_OFF);
		tty_flip_buffer_push(&benvisor_tty_port);
		if (rom_putc_fn) {
			rom_putc_fn('['); rom_putc_fn('R'); rom_putc_fn('X');
			rom_putc_fn(':'); rom_putc_fn('0' + count / 10);
			rom_putc_fn('0' + count % 10); rom_putc_fn(']');
			rom_putc_fn('\r'); rom_putc_fn('\n');
		}
	}

resched:
	mod_timer(&rx_timer, jiffies + RX_POLL_INTERVAL);
}

static struct tty_driver *benvisor_console_device(struct console *co, int *index)
{
	*index = 0;
	return benvisor_tty_driver;
}

static struct console benvisor_tty_console = {
	.name    = "ttyBV",
	.write   = benvisor_console_write,
	.device  = benvisor_console_device,
	.flags   = CON_PRINTBUFFER,
	.index   = 0,
};

static int __init benvisor_tty_init(void)
{
	int ret;

	if (!ipc_base) {
		pr_err("benvisor: ipc_base not set\n");
		return -ENODEV;
	}

	benvisor_tty_driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW);
	if (IS_ERR(benvisor_tty_driver)) {
		pr_err("benvisor: tty_alloc_driver failed\n");
		return PTR_ERR(benvisor_tty_driver);
	}

	benvisor_tty_driver->driver_name  = "benvisor";
	benvisor_tty_driver->name         = "ttyBV";
	benvisor_tty_driver->major        = 204;
	benvisor_tty_driver->minor_start  = 209;
	benvisor_tty_driver->type         = TTY_DRIVER_TYPE_SERIAL;
	benvisor_tty_driver->subtype      = SERIAL_TYPE_NORMAL;
	benvisor_tty_driver->init_termios = tty_std_termios;
	benvisor_tty_driver->init_termios.c_cflag =
		B115200 | CS8 | CREAD | HUPCL | CLOCAL;

	tty_set_operations(benvisor_tty_driver, &benvisor_tty_ops);

	tty_port_init(&benvisor_tty_port);
	tty_port_link_device(&benvisor_tty_port, benvisor_tty_driver, 0);

	timer_setup(&rx_timer, benvisor_rx_poll, 0);

	ret = tty_register_driver(benvisor_tty_driver);
	if (ret) {
		pr_err("benvisor: tty_register_driver failed: %d\n", ret);
		tty_driver_kref_put(benvisor_tty_driver);
		return ret;
	}

	register_console(&benvisor_tty_console);

	/* Unregister the phase 1 boot console now that the tty console
	 * has taken over. Both use benvisor_console_write → ROM UART + IPC,
	 * so having both registered causes every printk line to appear twice.
	 * The earlycon (CON_BOOT) was already auto-unregistered when the
	 * boot console registered in console_initcall. */
	unregister_console(&benvisor_boot_console);

	pr_info("benvisor-console: ttyBV0 registered\n");
	return 0;
}
device_initcall(benvisor_tty_init);

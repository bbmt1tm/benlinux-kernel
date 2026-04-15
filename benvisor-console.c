// SPDX-License-Identifier: GPL-2.0
/*
 * BenVisor Console Driver — full UART port with TX and RX
 *
 * Provides a proper TTY device (ttyBV0) so userspace can open
 * /dev/console for stdin/stdout/stderr. Without this, the kernel
 * boots but BusyBox can't get a shell.
 *
 * TX path:
 *   - ROM UART direct call (works with MIE=0)
 *   - IPC ring buffer for BenOS WebSocket forwarding
 *
 * RX path:
 *   - Polls IPC RX ring buffer every 10ms via timer
 *   - BenOS (Core 0) writes keyboard input into rx_buf
 *
 * IPC layout (from benvisor.h):
 *   +0x000  tx_head   (uint32, Linux writes)
 *   +0x004  tx_tail   (uint32, BenOS reads)
 *   +0x008  tx_buf    (2048 bytes, circular)
 *   +0x808  rx_head   (uint32, BenOS writes)
 *   +0x80C  rx_tail   (uint32, Linux reads)
 *   +0x810  rx_buf    (256 bytes, circular)
 *   +0x910  linux_state
 *   +0x914  mtimecmp_lo
 *   +0x918  mtimecmp_hi
 *   +0x91C  rom_putc_addr
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
#include <linux/timer.h>
#include <linux/tty_flip.h>

/* TX ring buffer — must match benvisor.h */
#define TX_BUF_SIZE     2048
#define TX_BUF_MASK     (TX_BUF_SIZE - 1)
#define TX_HEAD_OFF     0x000
#define TX_TAIL_OFF     0x004
#define TX_BUF_OFF      0x008

/* RX ring buffer — must match benvisor.h */
#define RX_BUF_SIZE     256
#define RX_BUF_MASK     (RX_BUF_SIZE - 1)
#define RX_HEAD_OFF     0x808
#define RX_TAIL_OFF     0x80C
#define RX_BUF_OFF      0x810

/* ROM function address offset in IPC struct */
#define ROM_PUTC_ADDR_OFF  0x91C

/* RX poll interval in jiffies (~10ms) */
#define RX_POLL_INTERVAL   (HZ / 100 ?: 1)

/* Driver identity */
#define BENVISOR_DRIVER_NAME  "ttyBV"
#define BENVISOR_MAJOR        204
#define BENVISOR_MINOR        209

/* ROM putc function type */
typedef int (*rom_putc_fn_t)(unsigned char);

/* Cached ROM function pointer */
static rom_putc_fn_t rom_putc_fn;

/* IPC base for full console (set during probe) */
static void __iomem *ipc_base;

/* RX poll timer */
static struct timer_list rx_timer;
static struct uart_port benvisor_port;

/* ========================================
 * Direct UART output via ROM function
 * Works with MIE=0 — no ISR dependency.
 * ======================================== */

static void benvisor_putchar_direct(unsigned char ch)
{
	if (rom_putc_fn)
		rom_putc_fn(ch);
}

/* IPC ring buffer write — for BenOS WebSocket forwarding */
static void benvisor_putchar_ipc(unsigned char ch)
{
	u32 head, next, tail;

	if (!ipc_base)
		return;

	head = readl_relaxed(ipc_base + TX_HEAD_OFF);
	next = (head + 1) & TX_BUF_MASK;
	tail = readl_relaxed(ipc_base + TX_TAIL_OFF);

	if (next == tail)
		return;		/* buffer full — drop */

	writeb(ch, ipc_base + TX_BUF_OFF + head);
	writel_relaxed(next, ipc_base + TX_HEAD_OFF);
}

/* Write one character to both ROM and IPC */
static void benvisor_putchar(unsigned char ch)
{
	benvisor_putchar_direct(ch);
	benvisor_putchar_ipc(ch);
}

/* ========================================
 * Earlycon — works from first printk
 *
 * Uses ROM function directly. Critical because
 * start_kernel() calls local_irq_disable() before
 * printk, so ISR-based IPC drain can't work.
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

	putc_addr = readl_relaxed(base + ROM_PUTC_ADDR_OFF);
	if (putc_addr)
		rom_putc_fn = (rom_putc_fn_t)(uintptr_t)putc_addr;

	device->con->write = benvisor_earlycon_write;
	return 0;
}

OF_EARLYCON_DECLARE(benvisor, "benos,benvisor-console",
		    benvisor_earlycon_setup);

/* ========================================
 * UART port operations
 * ======================================== */

static unsigned int benvisor_uart_tx_empty(struct uart_port *port)
{
	return TIOCSER_TEMT;
}

static void benvisor_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int benvisor_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void benvisor_uart_stop_tx(struct uart_port *port)
{
}

static void benvisor_uart_start_tx(struct uart_port *port)
{
	unsigned char ch;

	uart_port_tx(port, ch, true, benvisor_putchar(ch));
}

static void benvisor_uart_stop_rx(struct uart_port *port)
{
}

static void benvisor_uart_break_ctl(struct uart_port *port, int break_state)
{
}

static int benvisor_uart_startup(struct uart_port *port)
{
	/* Start RX polling timer */
	mod_timer(&rx_timer, jiffies + RX_POLL_INTERVAL);
	return 0;
}

static void benvisor_uart_shutdown(struct uart_port *port)
{
	del_timer_sync(&rx_timer);
}

static void benvisor_uart_set_termios(struct uart_port *port,
				      struct ktermios *new,
				      const struct ktermios *old)
{
	/* Accept any termios — we're a virtual console */
	tty_termios_copy_hw(new, old);
}

static const char *benvisor_uart_type(struct uart_port *port)
{
	return "benvisor";
}

static void benvisor_uart_release_port(struct uart_port *port)
{
}

static int benvisor_uart_request_port(struct uart_port *port)
{
	return 0;
}

static void benvisor_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_UNKNOWN;
}

static int benvisor_uart_verify_port(struct uart_port *port,
				     struct serial_struct *ser)
{
	return 0;
}

static const struct uart_ops benvisor_uart_ops = {
	.tx_empty     = benvisor_uart_tx_empty,
	.set_mctrl    = benvisor_uart_set_mctrl,
	.get_mctrl    = benvisor_uart_get_mctrl,
	.stop_tx      = benvisor_uart_stop_tx,
	.start_tx     = benvisor_uart_start_tx,
	.stop_rx      = benvisor_uart_stop_rx,
	.break_ctl    = benvisor_uart_break_ctl,
	.startup      = benvisor_uart_startup,
	.shutdown     = benvisor_uart_shutdown,
	.set_termios  = benvisor_uart_set_termios,
	.type         = benvisor_uart_type,
	.release_port = benvisor_uart_release_port,
	.request_port = benvisor_uart_request_port,
	.config_port  = benvisor_uart_config_port,
	.verify_port  = benvisor_uart_verify_port,
};

/* ========================================
 * RX polling timer
 *
 * Checks IPC RX ring buffer for input from
 * BenOS (keyboard via BLE/WebSocket).
 * ======================================== */

static void benvisor_rx_poll(struct timer_list *t)
{
	u32 head, tail;
	int count = 0;

	if (!ipc_base)
		goto reschedule;

	head = readl_relaxed(ipc_base + RX_HEAD_OFF);
	tail = readl_relaxed(ipc_base + RX_TAIL_OFF);

	while (tail != head && count < 64) {
		unsigned char ch = readb(ipc_base + RX_BUF_OFF + tail);

		tail = (tail + 1) & RX_BUF_MASK;
		count++;

		if (tty_insert_flip_char(&benvisor_port.state->port, ch, TTY_NORMAL) == 0)
			break;
	}

	if (count) {
		writel_relaxed(tail, ipc_base + RX_TAIL_OFF);
		tty_flip_buffer_push(&benvisor_port.state->port);
	}

reschedule:
	mod_timer(&rx_timer, jiffies + RX_POLL_INTERVAL);
}

/* ========================================
 * Console write — attached to UART port
 * ======================================== */

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

static int benvisor_console_setup(struct console *co, char *options)
{
	return 0;
}

static struct uart_driver benvisor_uart_driver = {
	.owner        = THIS_MODULE,
	.driver_name  = "benvisor",
	.dev_name     = BENVISOR_DRIVER_NAME,
	.major        = BENVISOR_MAJOR,
	.minor        = BENVISOR_MINOR,
	.nr           = 1,
	.cons         = NULL,  /* Set below after declaration */
};

static struct console benvisor_console = {
	.name    = BENVISOR_DRIVER_NAME,
	.write   = benvisor_console_write,
	.device  = uart_console_device,
	.setup   = benvisor_console_setup,
	.flags   = CON_PRINTBUFFER,
	.index   = 0,
	.data    = &benvisor_uart_driver,
};

/* ========================================
 * Init — register UART driver + port
 * ======================================== */

static int __init benvisor_console_init(void)
{
	struct device_node *np;
	u32 reg[2];
	int ret;

	np = of_find_compatible_node(NULL, NULL, "benos,benvisor-console");
	if (!np)
		return -ENODEV;

	if (of_property_read_u32_array(np, "reg", reg, 2)) {
		of_node_put(np);
		return -EINVAL;
	}
	of_node_put(np);

	/* NOMMU: physical == virtual, no ioremap needed */
	ipc_base = (void __iomem *)(uintptr_t)reg[0];

	/* Pick up ROM function if earlycon didn't set it */
	if (!rom_putc_fn) {
		u32 putc_addr = readl_relaxed(ipc_base + ROM_PUTC_ADDR_OFF);
		if (putc_addr)
			rom_putc_fn = (rom_putc_fn_t)(uintptr_t)putc_addr;
	}

	/* Set console on the UART driver */
	benvisor_uart_driver.cons = &benvisor_console;

	/* Register UART driver */
	ret = uart_register_driver(&benvisor_uart_driver);
	if (ret) {
		pr_err("benvisor: uart_register_driver failed: %d\n", ret);
		return ret;
	}

	/* Set up the port */
	memset(&benvisor_port, 0, sizeof(benvisor_port));
	benvisor_port.type     = PORT_UNKNOWN;
	benvisor_port.iotype   = UPIO_MEM;
	benvisor_port.mapbase  = reg[0];
	benvisor_port.membase  = ipc_base;
	benvisor_port.irq      = 0;  /* Polled */
	benvisor_port.uartclk  = 115200 * 16;
	benvisor_port.fifosize = TX_BUF_SIZE;
	benvisor_port.ops      = &benvisor_uart_ops;
	benvisor_port.flags    = UPF_BOOT_AUTOCONF;
	benvisor_port.line     = 0;
	spin_lock_init(&benvisor_port.lock);

	/* Set up RX polling timer */
	timer_setup(&rx_timer, benvisor_rx_poll, 0);

	/* Add the port */
	ret = uart_add_one_port(&benvisor_uart_driver, &benvisor_port);
	if (ret) {
		pr_err("benvisor: uart_add_one_port failed: %d\n", ret);
		uart_unregister_driver(&benvisor_uart_driver);
		return ret;
	}

	/* Register as a console */
	register_console(&benvisor_console);

	pr_info("benvisor-console: registered ttyBV0 at 0x%08x (rom_putc=%p)\n",
		reg[0], rom_putc_fn);

	return 0;
}
console_initcall(benvisor_console_init);

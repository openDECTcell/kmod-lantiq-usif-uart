// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lantiq VR9 USIF UART driver: one serial port, /dev/ttyUSIF0.
 *
 * Derived from Infineon's USIF UART driver ifxmips_usif_uart.c (IFX UEIP,
 * "USIF module for uart driver"), as shipped in AVM's FRITZ!Box 7362 SL GPL
 * source (FRITZ!OS 07.12):
 *   Copyright (c) 2009 Infineon Technologies AG
 *   Am Campeon 1-12, 85579 Neubiberg, Germany
 *
 * Rewritten lean for the Linux 6.12 serial core. Kept from Infineon: the
 * register sequence that brings the USIF up in asynchronous mode, the FIFO
 * setup (flow-control mode, byte aligned, bursts of four words, byte swap),
 * and the meaning of the receive request interrupts. Left out: console,
 * modem lines, hardware flow control, autobaud, PMCU hooks, procfs, and the
 * fixed baud table (src/usif_baud.c computes any rate instead).
 *
 * Only 8N1 is offered. Received bytes go to the tty layer from the RX
 * interrupt; the tail of a burst follows after the inter-character timeout.
 * docs/hardware.md has the register and clock facts and their sources.
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#include "usif_baud.h"
#include "usif_regs.h"
#include "usif_uart.h"

#define USIF_TTY_NAME		"ttyUSIF"
/* serial_core.h has no entry for this port; private, like lantiq.c's ASC */
#define USIF_PORT_TYPE		124
#define USIF_BAUD_DEFAULT	9600
#define USIF_BURST_WORDS	4	/* FIFO burst size set in FIFO_CFG */
#define USIF_BURST_4W		2	/* FIFO_CFG burst code for 4 words */
#define USIF_FIFO_WORDS_DEF	8	/* if FIFO_ID reads implausibly */
#define USIF_CLC_POLL_US	100	/* mode switch handshake bound */

/*
 * Module clock. Linux 6.12 registers a gate for the USIF but no rate. The
 * Infineon VR9 code derives the USIF clock as PLL0 / 10 (a fixed divider),
 * with PLL0 = 36 MHz * 2 * (N + 1) / (M + 1) from CGU_PLL0_CFG; that is
 * 100 MHz for the usual 1 GHz PLL0. The register is only read.
 */
#define VR9_XTAL_HZ		36000000ULL
#define VR9_CGU_PLL0_CFG	0x004
#define  VR9_PLL0_EN		BIT(0)
#define  VR9_PLL0_BYPASS	BIT(30)
#define  VR9_PLL0_M(x)		(((x) >> 2) & 0xf)
#define  VR9_PLL0_N(x)		(((x) >> 6) & 0xff)
#define VR9_USIF_PLL0_DIV	10
#define USIF_CLK_DEFAULT_HZ	100000000U
#define USIF_CLK_MIN_HZ		50000000U
#define USIF_CLK_MAX_HZ		150000000U

/* registers outside the USIF shown in the debugfs dump, read only */
#define VR9_PMU_PWDCR		0x01c
#define VR9_PMU_PWDSR		0x020
#define VR9_PMU_USIF_ECO	0x20c	/* AVM's kernel sets bit 30 at boot */
#define VR9_RCU_AHB_ENDIAN	0x04c
/* GPIO port 0 (GPIO 0..15) at the pinmux base, see pinctrl-xway.c */
#define XWAY_P0_OUT		0x00
#define XWAY_P0_IN		0x04
#define XWAY_P0_DIR		0x08	/* 1 = output */
#define XWAY_P0_ALT0		0x0c
#define XWAY_P0_ALT1		0x10
#define XWAY_P0_OD		0x14	/* 0 = open drain */
#define XWAY_P0_PUDSEL		0x1c
#define XWAY_P0_PUDEN		0x20
#define USIF_GPIO_RX		11
#define USIF_GPIO_TX		12

static unsigned int clock_hz;
module_param(clock_hz, uint, 0444);
MODULE_PARM_DESC(clock_hz,
		 "USIF module clock in Hz (default: PLL0 / 10 from the CGU)");

/*
 * Test aid: TX looped back to RX inside the USIF, and the pads left in
 * their "sleep" state (inputs), so nothing reaches the pins.
 */
static bool loopback;
module_param(loopback, bool, 0444);
MODULE_PARM_DESC(loopback,
		 "test only: internal loopback, pads not driven (default 0)");

struct usif_port {
	struct uart_port port;
	struct usif_baud_cfg baud;	/* divider setting in use */
	unsigned int baud_req;		/* the rate it was computed for */
	unsigned int char_us;		/* one 10-bit character, rounded up */
	unsigned int tx_words;		/* FIFO depths in 32-bit words */
	unsigned int rx_words;
	unsigned long tx_quiet;		/* jiffies: TX FIFO must be empty */
	bool rx_drop;			/* CREAD off */
	bool sta_on;			/* status interrupt requested */
	bool tx_hold;			/* set_termios: no TX refills */
	int irq_tx, irq_rx, irq_err, irq_sta;
	const char *clk_src;
	void __iomem *cgu, *pmu, *rcu;	/* read only */
	void __iomem *gpio;		/* read only, port 0 shown */
	u32 id, pll0_cfg;		/* read at probe */
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_sleep;
	struct dentry *debugfs;
	/* diagnostics, shown in debugfs */
	unsigned int n_rx_irq, n_tx_irq, n_err_irq, n_tx_fin;
	unsigned int n_rx_no_rme, n_rxur, n_txur, n_txof;
	unsigned int n_tx_forced, n_mode_timeout;
};

static struct uart_driver usif_uart_driver;

static inline struct usif_port *to_usif(struct uart_port *port)
{
	return container_of(port, struct usif_port, port);
}

static inline u32 usif_r(struct usif_port *u, unsigned int off)
{
	return __raw_readl(u->port.membase + off);
}

static inline void usif_w(struct usif_port *u, u32 val, unsigned int off)
{
	__raw_writel(val, u->port.membase + off);
}

static void usif_imsc_update(struct usif_port *u, u32 clear, u32 set)
{
	usif_w(u, (usif_r(u, USIF_IMSC) & ~clear) | set, USIF_IMSC);
}

/* ------------------------------------------------------------------ */
/* hardware state                                                     */
/* ------------------------------------------------------------------ */

/*
 * Request configuration or run mode and wait for CLC_STAT to confirm it.
 * Atomic context; the handshake takes a few module clock cycles.
 */
static int usif_set_mode(struct usif_port *u, u32 req, u32 want)
{
	unsigned int i;

	/* a read of a configuration register lets earlier writes land first */
	usif_r(u, USIF_MODE_CFG);
	usif_w(u, req, USIF_CLC);
	for (i = 0; i < USIF_CLC_POLL_US; i++) {
		if ((usif_r(u, USIF_CLC_STAT) & USIF_CLC_STAT_RUN_DONE) == want)
			return 0;
		udelay(1);
	}
	u->n_mode_timeout++;
	return -ETIMEDOUT;
}

static int usif_cfg_mode(struct usif_port *u)
{
	return usif_set_mode(u, USIF_CLC_CFG, USIF_CLC_STAT_CFG_DONE);
}

static int usif_run_mode(struct usif_port *u)
{
	return usif_set_mode(u, USIF_CLC_RUN, USIF_CLC_STAT_RUN_DONE);
}

/* registers that every switch to run mode resets */
static void usif_run_setup(struct usif_port *u)
{
	/* no receive packet size limit: the inter-character timeout ends it */
	usif_w(u, 0, USIF_MRPS_CTRL);
	usif_w(u, 0, USIF_TXD_SB);
	/* no DMA in either direction */
	usif_w(u, USIF_DPLUS_CTRL_TX_DIS | USIF_DPLUS_CTRL_RX_MASK |
		  USIF_DPLUS_CTRL_SET_MASK, USIF_DPLUS_CTRL);
}

/* configuration mode only */
static void usif_write_baud(struct usif_port *u)
{
	usif_w(u, usif_fdiv_word(&u->baud), USIF_FDIV_CFG);
	usif_w(u, usif_bc_word(&u->baud), USIF_BC_CFG);
	usif_w(u, u->baud.tmo, USIF_ICTMO_CFG);
}

/* throw away what sits in the receive FIFO; bounded */
static void usif_rx_discard(struct usif_port *u)
{
	unsigned int i;

	for (i = 0; i < 2 * u->rx_words + 4; i++) {
		if (!USIF_FIFO_STAT_RX_FFS(usif_r(u, USIF_FIFO_STAT)))
			break;
		usif_r(u, USIF_RXD);
	}
}

static unsigned int usif_fifo_words(unsigned int v)
{
	return v && v <= 64 ? v : USIF_FIFO_WORDS_DEF;
}

static void usif_set_char_time(struct usif_port *u, unsigned int baud)
{
	u->baud_req = baud;
	u->char_us = DIV_ROUND_UP(10 * USEC_PER_SEC, baud);
}

/*
 * Bring the USIF up as an asynchronous master (internal rate generator),
 * 8N1, both directions enabled, all interrupts masked, at the default
 * rate. It stays in run mode from here until removal, so TX idles at the
 * mark level whether or not the port is open.
 */
static int usif_hw_init(struct usif_port *u)
{
	int ret;

	usif_w(u, USIF_CLC_CNT_RMC(1) | USIF_CLC_CNT_ORMC(1), USIF_CLC_CNT);
	usif_w(u, USIF_CLC_MOD_EN, USIF_CLC);
	ret = usif_cfg_mode(u);
	if (ret)
		return ret;

	usif_w(u, 0, USIF_IMSC);
	usif_w(u, USIF_INT_ALL, USIF_ICR);
	usif_w(u, USIF_MODE_CFG_MA | USIF_MODE_CFG_TX_EN | USIF_MODE_CFG_RX_EN |
		  (loopback ? USIF_MODE_CFG_LB : 0), USIF_MODE_CFG);
	usif_w(u, USIF_PRTC_CFG_CLEN8, USIF_PRTC_CFG);
	usif_write_baud(u);
	usif_w(u, USIF_FIFO_CFG_RXBS(USIF_BURST_4W) | USIF_FIFO_CFG_RXFC |
		  USIF_FIFO_CFG_RXFA(0) | USIF_FIFO_CFG_RX_SWAP |
		  USIF_FIFO_CFG_TXBS(USIF_BURST_4W) | USIF_FIFO_CFG_TXFC |
		  USIF_FIFO_CFG_TXFA(0) | USIF_FIFO_CFG_TX_SWAP, USIF_FIFO_CFG);
	/* after FIFO_CFG and still in configuration mode (Infineon) */
	usif_w(u, USIF_FIFO_CTRL_RX_AR_ON, USIF_FIFO_CTRL);

	ret = usif_run_mode(u);
	if (ret)
		return ret;
	usif_run_setup(u);

	u->id = usif_r(u, USIF_ID);
	u->tx_words = usif_fifo_words(USIF_FIFO_ID_TX(usif_r(u, USIF_FIFO_ID)));
	u->rx_words = usif_fifo_words(USIF_FIFO_ID_RX(usif_r(u, USIF_FIFO_ID)));
	usif_rx_discard(u);
	usif_w(u, USIF_INT_ALL, USIF_ICR);
	return 0;
}

/* ------------------------------------------------------------------ */
/* transmit                                                           */
/* ------------------------------------------------------------------ */

static void usif_tx_word(struct usif_port *u, const u8 *buf, unsigned int n,
			 bool last)
{
	/*
	 * The side band applies to the next TXD write: the last word of what
	 * is queued ends the message and says how many of its bytes count.
	 * Written for every word so no stale end mark is left behind.
	 */
	usif_w(u, last ? USIF_TXD_SB_XME | USIF_TXD_SB_BE(n) : 0, USIF_TXD_SB);
	usif_w(u, usif_pack_word(buf, n), USIF_TXD);
}

/* refill the TX FIFO from the xmit buffer; port lock held */
static void usif_tx_fill(struct usif_port *u)
{
	struct uart_port *port = &u->port;
	struct tty_port *tport = &port->state->port;
	unsigned int fill, room;
	bool wrote = false;
	u8 buf[4];

	if (u->tx_hold) {
		usif_imsc_update(u, USIF_INT_TX, 0);
		return;
	}
	fill = USIF_FIFO_STAT_TX_FFS(usif_r(u, USIF_FIFO_STAT));
	room = fill < u->tx_words ? u->tx_words - fill : 0;

	if (port->x_char && room) {
		buf[0] = port->x_char;
		usif_tx_word(u, buf, 1, true);
		port->icount.tx++;
		port->x_char = 0;
		room--;
		wrote = true;
	}

	while (room && !uart_tx_stopped(port)) {
		unsigned int n = uart_fifo_out(port, buf, sizeof(buf));

		if (!n)
			break;
		usif_tx_word(u, buf, n, kfifo_is_empty(&tport->xmit_fifo));
		room--;
		wrote = true;
	}

	if (wrote)	/* the FIFO and shifter drain within this bound */
		u->tx_quiet = jiffies + 2 +
			usecs_to_jiffies(u->char_us * (4 * u->tx_words + 4));

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);
	if (kfifo_is_empty(&tport->xmit_fifo) || uart_tx_stopped(port))
		usif_imsc_update(u, USIF_INT_TX, 0);
}

/*
 * FIFO empty. The last word may still be in the shifter (up to four
 * characters); set_termios waits that out separately. The time bound keeps
 * a misread fill level from blocking tcdrain() for ever, and is counted.
 */
static unsigned int usif_tx_empty(struct uart_port *port)
{
	struct usif_port *u = to_usif(port);

	if (!USIF_FIFO_STAT_TX_FFS(usif_r(u, USIF_FIFO_STAT)))
		return TIOCSER_TEMT;
	if (time_after(jiffies, u->tx_quiet)) {
		u->n_tx_forced++;
		return TIOCSER_TEMT;
	}
	return 0;
}

static void usif_start_tx(struct uart_port *port)
{
	struct usif_port *u = to_usif(port);

	if (u->tx_hold)
		return;		/* set_termios restarts it */
	usif_imsc_update(u, 0, USIF_INT_TX);
	usif_tx_fill(u);
}

static void usif_stop_tx(struct uart_port *port)
{
	usif_imsc_update(to_usif(port), USIF_INT_TX, 0);
}

/* ------------------------------------------------------------------ */
/* receive                                                            */
/* ------------------------------------------------------------------ */

/*
 * Read @words words from RXD. The last word of a packet carries 1 to 4
 * valid bytes, given by DPLUS_STAT, which must be read before the word.
 */
static void usif_rx_words(struct usif_port *u, unsigned int words, bool last)
{
	struct tty_port *tport = &u->port.state->port;
	u8 buf[4 * USIF_BURST_WORDS];
	unsigned int i, n = 0;

	for (i = 0; i < words && i < USIF_BURST_WORDS; i++) {
		unsigned int cnt = 4;

		if (last && i == words - 1) {
			u32 ds = usif_r(u, USIF_DPLUS_STAT);

			if (ds & USIF_DPLUS_STAT_RME) {
				cnt = USIF_DPLUS_STAT_RX_BE(ds) ?: 4;
			} else {
				cnt = 0;	/* as Infineon: drop it */
				u->n_rx_no_rme++;
			}
		}
		usif_unpack_word(usif_r(u, USIF_RXD), buf + n, cnt);
		n += cnt;
	}

	u->port.icount.rx += n;
	if (n && !u->rx_drop)
		tty_insert_flip_string(tport, buf, n);
}

/* port lock held */
static void usif_rx(struct usif_port *u, u32 mis)
{
	u->n_rx_irq++;
	if (mis & USIF_INT_RX_BREQ) {
		usif_rx_words(u, USIF_BURST_WORDS, false);
		usif_w(u, USIF_INT_RX_BREQ, USIF_ICR);
	}
	if (mis & USIF_INT_RX_LBREQ) {
		usif_rx_words(u, USIF_BURST_WORDS, true);
		usif_w(u, USIF_INT_RX_LBREQ, USIF_ICR);
	}
	if (mis & USIF_INT_RX_SREQ) {
		usif_rx_words(u, 1, false);
		usif_w(u, USIF_INT_RX_SREQ, USIF_ICR);
	}
	if (mis & USIF_INT_RX_LSREQ) {
		usif_rx_words(u, 1, true);
		usif_w(u, USIF_INT_RX_LSREQ, USIF_ICR);
	}
}

static void usif_stop_rx(struct uart_port *port)
{
	usif_imsc_update(to_usif(port), USIF_INT_RX, 0);
}

/* ------------------------------------------------------------------ */
/* interrupts                                                         */
/* ------------------------------------------------------------------ */

/* TX and RX request lines share this handler */
static irqreturn_t usif_data_irq(int irq, void *dev_id)
{
	struct usif_port *u = dev_id;
	struct uart_port *port = &u->port;
	unsigned long flags;
	unsigned int loops;
	bool rx = false;
	u32 mis;

	uart_port_lock_irqsave(port, &flags);
	mis = usif_r(u, USIF_MIS) & (USIF_INT_RX | USIF_INT_TX);
	if (!mis) {
		uart_port_unlock_irqrestore(port, flags);
		return IRQ_NONE;
	}
	for (loops = 0; mis && loops < 8; loops++) {
		if (mis & USIF_INT_RX) {
			usif_rx(u, mis);
			rx = true;
		}
		if (mis & USIF_INT_TX) {
			u->n_tx_irq++;
			usif_w(u, mis & USIF_INT_TX, USIF_ICR);
			usif_tx_fill(u);
		}
		/* also makes the clears land before the ICU is acknowledged */
		mis = usif_r(u, USIF_MIS) & (USIF_INT_RX | USIF_INT_TX);
	}
	uart_port_unlock_irqrestore(port, flags);

	if (rx)
		tty_flip_buffer_push(&port->state->port);
	return IRQ_HANDLED;
}

static irqreturn_t usif_err_irq(int irq, void *dev_id)
{
	struct usif_port *u = dev_id;
	struct uart_port *port = &u->port;
	unsigned long flags;
	bool push = false;
	u32 mis;

	uart_port_lock_irqsave(port, &flags);
	mis = usif_r(u, USIF_MIS) & USIF_INT_ERR;
	if (!mis) {
		uart_port_unlock_irqrestore(port, flags);
		return IRQ_NONE;
	}
	usif_w(u, mis, USIF_ICR);
	u->n_err_irq++;
	if (mis & USIF_INT_RXOF) {
		port->icount.overrun++;
		if (!u->rx_drop) {
			tty_insert_flip_char(&port->state->port, 0,
					     TTY_OVERRUN);
			push = true;
		}
	}
	if (mis & USIF_INT_FE)
		port->icount.frame++;
	if (mis & USIF_INT_PE)
		port->icount.parity++;
	if (mis & USIF_INT_RXUR)
		u->n_rxur++;
	if (mis & USIF_INT_TXUR)
		u->n_txur++;
	if (mis & USIF_INT_TXOF)
		u->n_txof++;
	usif_r(u, USIF_MIS);
	uart_port_unlock_irqrestore(port, flags);

	if (push)
		tty_flip_buffer_push(&port->state->port);
	return IRQ_HANDLED;
}

/* status line: only "transmission finished" is unmasked, and only counted */
static irqreturn_t usif_sta_irq(int irq, void *dev_id)
{
	struct usif_port *u = dev_id;
	unsigned long flags;
	u32 mis;

	uart_port_lock_irqsave(&u->port, &flags);
	mis = usif_r(u, USIF_MIS) & USIF_INT_TX_FIN;
	if (mis) {
		usif_w(u, mis, USIF_ICR);
		u->n_tx_fin++;
		usif_r(u, USIF_MIS);
	}
	uart_port_unlock_irqrestore(&u->port, flags);
	return mis ? IRQ_HANDLED : IRQ_NONE;
}

/* ------------------------------------------------------------------ */
/* serial core operations                                             */
/* ------------------------------------------------------------------ */

static int usif_startup(struct uart_port *port)
{
	struct usif_port *u = to_usif(port);
	const char *name = dev_name(port->dev);
	unsigned long flags;
	u32 imsc;
	int ret;

	uart_port_lock_irqsave(port, &flags);
	usif_w(u, 0, USIF_IMSC);
	usif_rx_discard(u);	/* whatever arrived while the port was closed */
	usif_w(u, USIF_INT_ALL, USIF_ICR);
	uart_port_unlock_irqrestore(port, flags);

	ret = request_irq(u->irq_tx, usif_data_irq, 0, name, u);
	if (ret)
		return ret;
	ret = request_irq(u->irq_rx, usif_data_irq, 0, name, u);
	if (ret)
		goto err_tx;
	ret = request_irq(u->irq_err, usif_err_irq, 0, name, u);
	if (ret)
		goto err_rx;
	imsc = USIF_INT_RX | USIF_INT_ERR;
	u->sta_on = u->irq_sta > 0 &&
		    !request_irq(u->irq_sta, usif_sta_irq, 0, name, u);
	if (u->sta_on)
		imsc |= USIF_INT_TX_FIN;

	uart_port_lock_irqsave(port, &flags);
	usif_w(u, imsc, USIF_IMSC);
	uart_port_unlock_irqrestore(port, flags);
	return 0;

err_rx:
	free_irq(u->irq_rx, u);
err_tx:
	free_irq(u->irq_tx, u);
	return ret;
}

/*
 * The USIF stays in run mode: TX keeps idling at the mark level while the
 * port is closed. Only the interrupts go.
 */
static void usif_shutdown(struct uart_port *port)
{
	struct usif_port *u = to_usif(port);
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	usif_w(u, 0, USIF_IMSC);
	usif_w(u, USIF_INT_ALL, USIF_ICR);
	uart_port_unlock_irqrestore(port, flags);

	if (u->sta_on)
		free_irq(u->irq_sta, u);
	u->sta_on = false;
	free_irq(u->irq_err, u);
	free_irq(u->irq_rx, u);
	free_irq(u->irq_tx, u);
}

/*
 * Wait until the TX FIFO is empty (bounded), then for the shifter: the
 * last FIFO word may hold four characters, plus margin. Refills are held
 * off meanwhile, so what is still in the xmit buffer goes out at the new
 * rate afterwards.
 */
static void usif_wait_tx_idle(struct usif_port *u)
{
	unsigned long end = jiffies + 2 +
		usecs_to_jiffies(2 * u->char_us * (4 * u->tx_words + 4));

	while (USIF_FIFO_STAT_TX_FFS(usif_r(u, USIF_FIFO_STAT)) &&
	       time_before(jiffies, end))
		usleep_range(100, 200);
	fsleep(6 * u->char_us);
}

static void usif_set_termios(struct uart_port *port, struct ktermios *termios,
			     const struct ktermios *old)
{
	struct usif_port *u = to_usif(port);
	struct usif_baud_cfg cfg;
	unsigned long flags;
	unsigned int baud;
	u32 imsc;

	/* the only format offered: 8 data bits, no parity, 1 stop bit */
	termios->c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD | CMSPAR |
			      CRTSCTS);
	termios->c_cflag |= CS8;

	baud = uart_get_baud_rate(port, termios, old, USIF_BAUD_MIN,
				  USIF_BAUD_MAX(port->uartclk));
	if (usif_baud_calc(port->uartclk, baud, &cfg)) {
		/* cannot happen within the limits above: keep the old rate */
		cfg = u->baud;
		baud = u->baud_req;
	}
	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);

	/*
	 * Everything is masked for the whole change: no TX refills, and what
	 * arrives meanwhile stays in the receive FIFO and is dropped below
	 * with the rest (tcsetattr(TCSAFLUSH) flushes only the tty buffers,
	 * and only before it waits for output to drain).
	 */
	uart_port_lock_irqsave(port, &flags);
	u->tx_hold = true;
	imsc = usif_r(u, USIF_IMSC) & ~USIF_INT_TX;
	usif_w(u, 0, USIF_IMSC);
	uart_port_unlock_irqrestore(port, flags);

	usif_wait_tx_idle(u);

	uart_port_lock_irqsave(port, &flags);
	uart_update_timeout(port, termios->c_cflag, baud);
	u->rx_drop = !(termios->c_cflag & CREAD);

	/* an empty RX FIFO also lets configuration mode report idle */
	usif_rx_discard(u);
	if (memcmp(&cfg, &u->baud, sizeof(cfg))) {
		u->baud = cfg;
		if (usif_cfg_mode(u))
			dev_err_ratelimited(port->dev,
					    "no configuration mode (CLC_STAT %08x)\n",
					    usif_r(u, USIF_CLC_STAT));
		usif_write_baud(u);
		if (usif_run_mode(u))
			dev_err_ratelimited(port->dev,
					    "no run mode (CLC_STAT %08x)\n",
					    usif_r(u, USIF_CLC_STAT));
		usif_run_setup(u);
	}
	usif_set_char_time(u, baud);
	usif_w(u, USIF_INT_ALL, USIF_ICR);
	usif_w(u, imsc, USIF_IMSC);
	u->tx_hold = false;
	if (port->state &&
	    !kfifo_is_empty(&port->state->port.xmit_fifo))
		usif_start_tx(port);
	uart_port_unlock_irqrestore(port, flags);
}

static unsigned int usif_get_mctrl(struct uart_port *port)
{
	/* no modem lines: report them asserted */
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void usif_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static const char *usif_type(struct uart_port *port)
{
	return port->type == USIF_PORT_TYPE ? "USIF" : NULL;
}

static void usif_release_port(struct uart_port *port)
{
}

static int usif_request_port(struct uart_port *port)
{
	return 0;	/* mapped at probe */
}

static void usif_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = USIF_PORT_TYPE;
}

static int usif_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != USIF_PORT_TYPE)
		return -EINVAL;
	return 0;
}

static const struct uart_ops usif_uart_ops = {
	.tx_empty	= usif_tx_empty,
	.set_mctrl	= usif_set_mctrl,
	.get_mctrl	= usif_get_mctrl,
	.stop_tx	= usif_stop_tx,
	.start_tx	= usif_start_tx,
	.stop_rx	= usif_stop_rx,
	.startup	= usif_startup,
	.shutdown	= usif_shutdown,
	.set_termios	= usif_set_termios,
	.type		= usif_type,
	.release_port	= usif_release_port,
	.request_port	= usif_request_port,
	.config_port	= usif_config_port,
	.verify_port	= usif_verify_port,
};

/* ------------------------------------------------------------------ */
/* debugfs                                                            */
/* ------------------------------------------------------------------ */
/*
 * Split by how well each read is established, so a doubtful one can be
 * tried alone. Reading CLC (offset 0) hung the bus on the first hardware
 * run; neither Infineon's driver nor its SPI sibling ever reads it, and
 * no file here does.
 *
 *   state       no hardware access: cached values and counters
 *   regs        USIF registers our probe read on the box, or Infineon's
 *               driver reads in its normal (non-debug) path
 *   regs-debug  USIF registers only Infineon's debug dump reads
 *   soc         PMU, RCU and CGU words through separate mappings
 *   pads        GPIO port 0 (pads 11 and 12 decoded)
 */
struct usif_dump_reg {
	const char *name;
	unsigned int off;
};

static const struct usif_dump_reg usif_regs_known[] = {
	/* read by our probe on the box (2026-11-01, session 3) */
	{ "ID", USIF_ID }, { "FIFO_ID", USIF_FIFO_ID },
	{ "CLC_STAT", USIF_CLC_STAT }, { "MODE_CFG", USIF_MODE_CFG },
	{ "FIFO_STAT", USIF_FIFO_STAT },
	/* read in Infineon's normal path */
	{ "CLC_CNT", USIF_CLC_CNT }, { "PRTC_CFG", USIF_PRTC_CFG },
	{ "FDIV_CFG", USIF_FDIV_CFG }, { "BC_CFG", USIF_BC_CFG },
	{ "FIFO_CFG", USIF_FIFO_CFG }, { "DPLUS_STAT", USIF_DPLUS_STAT },
	{ "IMSC", USIF_IMSC }, { "MIS", USIF_MIS },
};

static const struct usif_dump_reg usif_regs_debug[] = {
	{ "PRTC_STAT", USIF_PRTC_STAT }, { "ICTMO_CFG", USIF_ICTMO_CFG },
	{ "TXD_SB", USIF_TXD_SB }, { "MRPS_CTRL", USIF_MRPS_CTRL },
	{ "RIS", USIF_RIS },
};

/* no port lock: none of these reads has a side effect */
static void usif_show_list(struct seq_file *s, struct usif_port *u,
			   const struct usif_dump_reg *r, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		seq_printf(s, "USIF %-10s 0x%03x  %08x\n", r[i].name, r[i].off,
			   usif_r(u, r[i].off));
}

static int usif_regs_show(struct seq_file *s, void *unused)
{
	usif_show_list(s, s->private, usif_regs_known,
		       ARRAY_SIZE(usif_regs_known));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usif_regs);

static int usif_regs_debug_show(struct seq_file *s, void *unused)
{
	usif_show_list(s, s->private, usif_regs_debug,
		       ARRAY_SIZE(usif_regs_debug));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usif_regs_debug);

static int usif_soc_show(struct seq_file *s, void *unused)
{
	struct usif_port *u = s->private;

	if (u->cgu)
		seq_printf(s, "CGU  PLL0_CFG   0x%03x  %08x\n", VR9_CGU_PLL0_CFG,
			   __raw_readl(u->cgu + VR9_CGU_PLL0_CFG));
	if (u->pmu) {
		seq_printf(s, "PMU  PWDCR      0x%03x  %08x\n", VR9_PMU_PWDCR,
			   __raw_readl(u->pmu + VR9_PMU_PWDCR));
		seq_printf(s, "PMU  PWDSR      0x%03x  %08x\n", VR9_PMU_PWDSR,
			   __raw_readl(u->pmu + VR9_PMU_PWDSR));
		seq_printf(s, "PMU  USIF_ECO   0x%03x  %08x\n", VR9_PMU_USIF_ECO,
			   __raw_readl(u->pmu + VR9_PMU_USIF_ECO));
	}
	if (u->rcu)
		seq_printf(s, "RCU  AHB_ENDIAN 0x%03x  %08x\n",
			   VR9_RCU_AHB_ENDIAN,
			   __raw_readl(u->rcu + VR9_RCU_AHB_ENDIAN));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usif_soc);

/* mux, direction and driver of the two UART pads (port 0 is read only) */
static void usif_show_pads(struct seq_file *s, void __iomem *gpio)
{
	static const unsigned int pins[] = { USIF_GPIO_RX, USIF_GPIO_TX };
	u32 in = __raw_readl(gpio + XWAY_P0_IN);
	u32 dir = __raw_readl(gpio + XWAY_P0_DIR);
	u32 alt0 = __raw_readl(gpio + XWAY_P0_ALT0);
	u32 alt1 = __raw_readl(gpio + XWAY_P0_ALT1);
	u32 od = __raw_readl(gpio + XWAY_P0_OD);
	u32 puden = __raw_readl(gpio + XWAY_P0_PUDEN);
	u32 pudsel = __raw_readl(gpio + XWAY_P0_PUDSEL);
	unsigned int i;

	seq_printf(s, "GPIO P0 OUT %08x IN %08x DIR %08x ALT0 %08x ALT1 %08x OD %08x PUDSEL %08x PUDEN %08x\n",
		   __raw_readl(gpio + XWAY_P0_OUT), in, dir, alt0, alt1, od,
		   pudsel, puden);
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		unsigned int b = pins[i];

		seq_printf(s, "pad  io%u: level %u, %s, altsel %u/%u, %s, pull %s\n",
			   b, (in >> b) & 1,
			   (dir >> b) & 1 ? "output" : "input",
			   (alt0 >> b) & 1, (alt1 >> b) & 1,
			   (od >> b) & 1 ? "push-pull" : "open-drain",
			   !((puden >> b) & 1) ? "off" :
			   (pudsel >> b) & 1 ? "up" : "down");
	}
}

static int usif_pads_show(struct seq_file *s, void *unused)
{
	struct usif_port *u = s->private;

	if (u->gpio)
		usif_show_pads(s, u->gpio);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usif_pads);

/* software state only: safe to read at any time */
static int usif_state_show(struct seq_file *s, void *unused)
{
	struct usif_port *u = s->private;
	struct uart_icount *ic = &u->port.icount;
	u32 mbd_frac;
	u64 bd;

	seq_printf(s, "id         %08x, CGU_PLL0_CFG %08x (at probe)\n",
		   u->id, u->pll0_cfg);
	seq_printf(s, "clock      %u Hz (%s)%s\n", u->port.uartclk, u->clk_src,
		   loopback ? ", loopback" : "");
	bd = div_u64_rem(u->baud.rate_mbd, 1000, &mbd_frac);
	seq_printf(s, "baud       %u requested, %llu.%03u achieved (%+ld ppm)\n",
		   u->baud_req, bd, mbd_frac,
		   usif_baud_error_ppm(&u->baud, u->baud_req));
	seq_printf(s, "divider    INC %u DEC %u BCRV %u, timeout %u bits\n",
		   u->baud.inc, u->baud.dec, u->baud.bcrv, u->baud.tmo);
	seq_printf(s, "fifo       TX %u words, RX %u words\n",
		   u->tx_words, u->rx_words);
	seq_printf(s, "count      rx %u tx %u overrun %u frame %u parity %u\n",
		   ic->rx, ic->tx, ic->overrun, ic->frame, ic->parity);
	seq_printf(s, "irq        rx %u tx %u err %u tx_fin %u\n",
		   u->n_rx_irq, u->n_tx_irq, u->n_err_irq, u->n_tx_fin);
	seq_printf(s, "anomaly    rx_no_rme %u rxur %u txur %u txof %u tx_forced %u mode_timeout %u\n",
		   u->n_rx_no_rme, u->n_rxur, u->n_txur, u->n_txof,
		   u->n_tx_forced, u->n_mode_timeout);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usif_state);

/* ------------------------------------------------------------------ */
/* probe / remove                                                     */
/* ------------------------------------------------------------------ */

static void usif_iounmap(void *addr)
{
	iounmap(addr);
}

/* map a SoC block by compatible, read only; NULL if unavailable */
static void __iomem *usif_map_soc(struct device *dev, const char *compat)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, compat);
	void __iomem *base;

	if (!np)
		return NULL;
	base = of_iomap(np, 0);
	of_node_put(np);
	if (base && devm_add_action_or_reset(dev, usif_iounmap,
					     (void __force *)base))
		return NULL;
	return base;
}

static unsigned int usif_module_clock(struct usif_port *u, struct device *dev)
{
	u32 cfg;
	u64 pll0;

	if (clock_hz) {
		u->clk_src = "module parameter";
		return clock_hz;
	}
	u->clk_src = "default";
	if (!u->cgu)
		return USIF_CLK_DEFAULT_HZ;

	cfg = __raw_readl(u->cgu + VR9_CGU_PLL0_CFG);
	u->pll0_cfg = cfg;
	if (cfg & VR9_PLL0_EN) {
		if (cfg & VR9_PLL0_BYPASS)
			pll0 = VR9_XTAL_HZ;
		else
			pll0 = div_u64(VR9_XTAL_HZ * 2 * (VR9_PLL0_N(cfg) + 1),
				       VR9_PLL0_M(cfg) + 1);
		pll0 = div_u64(pll0, VR9_USIF_PLL0_DIV);
		if (pll0 >= USIF_CLK_MIN_HZ && pll0 <= USIF_CLK_MAX_HZ) {
			u->clk_src = "CGU PLL0 / 10";
			return pll0;
		}
	}
	dev_warn(dev, "CGU_PLL0_CFG %08x implausible, assuming %u Hz\n",
		 cfg, USIF_CLK_DEFAULT_HZ);
	return USIF_CLK_DEFAULT_HZ;
}

static void usif_clk_off(void *clk)
{
	clk_disable_unprepare(clk);
}

static int usif_clocks_on(struct device *dev)
{
	struct clk *clk;
	int ret;

	/*
	 * Linux 6.12's xway sysctrl registers the USIF gate for device
	 * "1da00000.usif" with the connection id spelt "NULL" (a string);
	 * try that first, then a plain NULL in case it is ever corrected.
	 * Not devm_clk_get_enabled(): see kmod-lantiq-ssc-pcm, the legacy
	 * xway clk_enable() fails on a NULL clock.
	 */
	clk = devm_clk_get(dev, "NULL");
	if (IS_ERR(clk))
		clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "USIF clock gate\n");
	ret = clk_prepare_enable(clk);
	if (ret)
		return dev_err_probe(dev, ret, "USIF clock enable\n");
	ret = devm_add_action_or_reset(dev, usif_clk_off, clk);
	if (ret)
		return ret;

	/*
	 * The AHB master/slave gates (Infineon enables AHBM with the USIF)
	 * are shared with USB and PCIe and the legacy xway clocks keep no
	 * enable count: turn them on, never off.
	 */
	clk = clk_get(dev, "ahb");
	if (IS_ERR(clk)) {
		dev_warn(dev, "no AHB clock gate: %ld\n", PTR_ERR(clk));
		return 0;
	}
	ret = clk_prepare_enable(clk);
	clk_put(clk);
	if (ret)
		dev_warn(dev, "AHB clock enable: %d\n", ret);
	return 0;
}

static int usif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct usif_port *u;
	struct resource *res;
	void __iomem *base;
	int ret;

	u = devm_kzalloc(dev, sizeof(*u), GFP_KERNEL);
	if (!u)
		return -ENOMEM;

	/*
	 * Reserve and map the USIF's own window only, whatever size the
	 * node gives (vr9.dtsi's 16 MiB would cover the FPI peripherals).
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -EINVAL, "no register window\n");
	if (resource_size(res) < USIF_MAP_SIZE)
		return dev_err_probe(dev, -EINVAL, "register window too small\n");
	if (!devm_request_mem_region(dev, res->start, USIF_MAP_SIZE,
				     dev_name(dev)))
		return dev_err_probe(dev, -EBUSY, "registers %pa busy\n",
				     &res->start);
	base = devm_ioremap(dev, res->start, USIF_MAP_SIZE);
	if (!base)
		return -ENOMEM;

	u->irq_tx = platform_get_irq(pdev, 0);
	if (u->irq_tx < 0)
		return u->irq_tx;
	u->irq_rx = platform_get_irq(pdev, 1);
	if (u->irq_rx < 0)
		return u->irq_rx;
	u->irq_err = platform_get_irq(pdev, 2);
	if (u->irq_err < 0)
		return u->irq_err;
	u->irq_sta = platform_get_irq_optional(pdev, 3);

	u->cgu = usif_map_soc(dev, "lantiq,cgu-xway");
	u->pmu = usif_map_soc(dev, "lantiq,pmu-xway");
	u->rcu = usif_map_soc(dev, "lantiq,xrx200-rcu");
	u->gpio = usif_map_soc(dev, "lantiq,xrx200-pinctrl");

	/* clocks before the first register access */
	ret = usif_clocks_on(dev);
	if (ret)
		return ret;

	u->port.membase = base;
	u->port.uartclk = usif_module_clock(u, dev);
	if (usif_baud_calc(u->port.uartclk, USIF_BAUD_DEFAULT, &u->baud))
		return dev_err_probe(dev, -EINVAL, "no divider for %u Bd\n",
				     USIF_BAUD_DEFAULT);
	usif_set_char_time(u, USIF_BAUD_DEFAULT);
	u->tx_quiet = jiffies;

	ret = usif_hw_init(u);
	if (ret)
		return dev_err_probe(dev, ret,
				     "USIF does not respond (CLC_STAT %08x)\n",
				     usif_r(u, USIF_CLC_STAT));

	u->port.dev = dev;
	u->port.type = USIF_PORT_TYPE;
	u->port.iotype = UPIO_MEM32BE;
	u->port.mapbase = res->start;
	u->port.mapsize = USIF_MAP_SIZE;
	u->port.irq = u->irq_rx;
	u->port.fifosize = 4 * u->tx_words;
	u->port.flags = UPF_FIXED_PORT | UPF_FIXED_TYPE;
	u->port.ops = &usif_uart_ops;
	u->port.line = 0;

	/* the "sleep" state, if described, releases the pads on removal */
	u->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(u->pinctrl)) {
		u->pinctrl = NULL;
	} else {
		u->pins_sleep = pinctrl_lookup_state(u->pinctrl,
						     PINCTRL_STATE_SLEEP);
		if (IS_ERR(u->pins_sleep))
			u->pins_sleep = NULL;
	}

	/*
	 * Loopback must not drive the pins: select "sleep" now, which also
	 * makes the core skip "default" after probe. No such state, no port.
	 */
	if (loopback && (!u->pins_sleep ||
			 pinctrl_select_state(u->pinctrl, u->pins_sleep))) {
		usif_cfg_mode(u);
		return dev_err_probe(dev, -ENODEV,
				     "loopback needs the \"sleep\" pin state\n");
	}

	platform_set_drvdata(pdev, u);
	ret = uart_add_one_port(&usif_uart_driver, &u->port);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		usif_cfg_mode(u);
		return ret;
	}

	u->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("state", 0400, u->debugfs, u, &usif_state_fops);
	debugfs_create_file("regs", 0400, u->debugfs, u, &usif_regs_fops);
	debugfs_create_file("regs-debug", 0400, u->debugfs, u,
			    &usif_regs_debug_fops);
	debugfs_create_file("soc", 0400, u->debugfs, u, &usif_soc_fops);
	debugfs_create_file("pads", 0400, u->debugfs, u, &usif_pads_fops);

	dev_info(dev, "%s0: ID %08x, FIFO %u/%u words, clock %u Hz (%s), irq %d %d %d %d%s\n",
		 USIF_TTY_NAME, u->id, u->tx_words, u->rx_words,
		 u->port.uartclk, u->clk_src, u->irq_tx, u->irq_rx,
		 u->irq_err, u->irq_sta, loopback ? ", loopback" : "");
	return 0;
}

static void usif_remove(struct platform_device *pdev)
{
	struct usif_port *u = platform_get_drvdata(pdev);

	debugfs_remove_recursive(u->debugfs);
	uart_remove_one_port(&usif_uart_driver, &u->port);

	/* let go of the pads first, then stop the USIF; the gate follows */
	if (u->pins_sleep)
		pinctrl_select_state(u->pinctrl, u->pins_sleep);
	usif_w(u, 0, USIF_IMSC);
	usif_w(u, USIF_INT_ALL, USIF_ICR);
	usif_cfg_mode(u);
}

static const struct of_device_id usif_of_match[] = {
	{ .compatible = "lantiq,usif" },
	{ }
};
MODULE_DEVICE_TABLE(of, usif_of_match);

static struct platform_driver usif_platform_driver = {
	.probe	= usif_probe,
	.remove	= usif_remove,
	.driver	= {
		.name		= USIF_DRV_NAME,
		.of_match_table	= usif_of_match,
		/* the port's lifetime is tied to the module, not to sysfs */
		.suppress_bind_attrs = true,
	},
};

static struct uart_driver usif_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "lantiq_usif",
	.dev_name	= USIF_TTY_NAME,
	.major		= 0,	/* dynamic: ttyS 4:64 belongs to others */
	.minor		= 0,
	.nr		= 1,
};

static int __init usif_init(void)
{
	int ret;

	ret = uart_register_driver(&usif_uart_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&usif_platform_driver);
	if (ret)
		goto err_uart;
	ret = usif_fallback_init();
	if (ret) {
		pr_err(USIF_DRV_NAME ": fallback failed: %d\n", ret);
		goto err_drv;
	}
	return 0;

err_drv:
	platform_driver_unregister(&usif_platform_driver);
err_uart:
	uart_unregister_driver(&usif_uart_driver);
	return ret;
}

static void __exit usif_exit(void)
{
	usif_fallback_exit();
	platform_driver_unregister(&usif_platform_driver);
	uart_unregister_driver(&usif_uart_driver);
}

module_init(usif_init);
module_exit(usif_exit);

MODULE_AUTHOR("openDECTcell project (derived from Infineon IFX UEIP USIF UART)");
MODULE_DESCRIPTION("Lantiq VR9 USIF UART driver (ttyUSIF)");
MODULE_LICENSE("GPL");

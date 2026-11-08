/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Lantiq VR9 USIF register map, UART subset.
 *
 * Derived from Infineon's ifxmips_usif_reg.h (IFX UEIP, "USIF module
 * register definition", as shipped in AVM's FRITZ!Box 7362 SL GPL source,
 * FRITZ!OS 07.12):
 *   Copyright (c) 2008 ~ 2009 Infineon Technologies AG
 *   Am Campeon 1-12, 85579 Neubiberg, Germany
 *
 * Only what the UART driver uses is kept; offsets are from the USIF base
 * (physical 0x1da00000 on VR9). All registers are 32 bits wide and
 * native-endian on this big-endian SoC: access them with
 * __raw_readl()/__raw_writel().
 */
#ifndef USIF_REGS_H
#define USIF_REGS_H

#include <linux/bits.h>

/* clock control: mode requests are set/clear pairs, 0 leaves a bit alone */
#define USIF_CLC		0x000
#define  USIF_CLC_RUN		BIT(0)	/* request run mode */
#define  USIF_CLC_CFG		BIT(1)	/* request configuration mode */
#define  USIF_CLC_MOD_EN	BIT(2)	/* enable the module */
#define USIF_CLC_CNT		0x004
#define  USIF_CLC_CNT_RMC(x)	((x) & 0xff)		/* clock divider */
#define  USIF_CLC_CNT_ORMC(x)	(((x) & 0xff) << 8)
#define USIF_CLC_STAT		0x008
#define  USIF_CLC_STAT_RUN	BIT(0)
#define  USIF_CLC_STAT_MODEN	BIT(1)
#define  USIF_CLC_STAT_CNT_DONE	BIT(6)	/* counter update done */
#define  USIF_CLC_STAT_KERN_IDL	BIT(7)
#define  USIF_CLC_STAT_FIFO_IDL	BIT(8)
/* state reached after a configuration / run mode request */
#define  USIF_CLC_STAT_CFG_DONE	(USIF_CLC_STAT_MODEN | USIF_CLC_STAT_CNT_DONE | \
				 USIF_CLC_STAT_KERN_IDL | USIF_CLC_STAT_FIFO_IDL)
#define  USIF_CLC_STAT_RUN_DONE	(USIF_CLC_STAT_CFG_DONE | USIF_CLC_STAT_RUN)
#define USIF_ID			0x00c
#define USIF_FIFO_ID		0x010
#define  USIF_FIFO_ID_TX(x)	((x) & 0xff)		/* depth in words */
#define  USIF_FIFO_ID_RX(x)	(((x) >> 8) & 0xff)

/* FIFOs */
#define USIF_DPLUS_CTRL		0x02c
#define  USIF_DPLUS_CTRL_SET_MASK	BIT(7)
#define  USIF_DPLUS_CTRL_RX_MASK	(0xff << 8)
#define  USIF_DPLUS_CTRL_TX_DIS		BIT(23)
#define USIF_FIFO_CFG		0x030
#define  USIF_FIFO_CFG_RXBS(x)	((x) & 0x7)		/* burst: 2 = 4 words */
#define  USIF_FIFO_CFG_RXFC	BIT(3)			/* flow control mode */
#define  USIF_FIFO_CFG_TXBS(x)	(((x) & 0x7) << 4)
#define  USIF_FIFO_CFG_TXFC	BIT(7)
#define  USIF_FIFO_CFG_RXFA(x)	(((x) & 0x7) << 8)	/* 0 = byte aligned */
#define  USIF_FIFO_CFG_RX_SWAP	BIT(11)
#define  USIF_FIFO_CFG_TXFA(x)	(((x) & 0x7) << 12)
#define  USIF_FIFO_CFG_TX_SWAP	BIT(15)
#define USIF_FIFO_CTRL		0x034
#define  USIF_FIFO_CTRL_RX_AR_ON	BIT(6)	/* RX auto request on */
#define USIF_MRPS_CTRL		0x038	/* max. receive packet size, 0 = none */
#define USIF_FIFO_STAT		0x044
#define  USIF_FIFO_STAT_RX_FFS(x)	((x) & 0xff)		/* words */
#define  USIF_FIFO_STAT_TX_FFS(x)	(((x) >> 16) & 0xff)	/* words */
#define USIF_TXD_SB		0x048	/* side band for the next TXD word */
#define  USIF_TXD_SB_BE(n)	(((n) & 0x3) << 4)	/* valid bytes, 0 = 4 */
#define  USIF_TXD_SB_XME	BIT(7)			/* end of message */
#define USIF_DPLUS_STAT		0x04c
#define  USIF_DPLUS_STAT_RX_BE(x)	(((x) >> 4) & 0x3)	/* 0 = 4 */
#define  USIF_DPLUS_STAT_RME		BIT(7)	/* last word of a packet */

/* interrupts: raw status, mask, masked status, set, clear */
#define USIF_RIS		0x080
#define USIF_IMSC		0x084
#define USIF_MIS		0x088
#define USIF_ISR		0x090
#define USIF_ICR		0x098
#define  USIF_INT_RX_LSREQ	BIT(0)	/* last single word of a packet */
#define  USIF_INT_RX_SREQ	BIT(1)	/* single word */
#define  USIF_INT_RX_LBREQ	BIT(2)	/* last burst of a packet */
#define  USIF_INT_RX_BREQ	BIT(3)	/* burst */
#define  USIF_INT_TX_SREQ	BIT(5)
#define  USIF_INT_TX_BREQ	BIT(7)
#define  USIF_INT_RXUR		BIT(11)	/* RX FIFO underrun */
#define  USIF_INT_RXOF		BIT(12)	/* RX FIFO overflow */
#define  USIF_INT_TXUR		BIT(13)
#define  USIF_INT_TXOF		BIT(14)
#define  USIF_INT_PE		BIT(15)	/* parity error */
#define  USIF_INT_FE		BIT(16)	/* framing error */
#define  USIF_INT_TX_FIN	BIT(29)	/* transmission finished */
#define  USIF_INT_ALL		0xffbbfbaf	/* every defined source */
#define  USIF_INT_RX		(USIF_INT_RX_LSREQ | USIF_INT_RX_SREQ | \
				 USIF_INT_RX_LBREQ | USIF_INT_RX_BREQ)
#define  USIF_INT_TX		(USIF_INT_TX_SREQ | USIF_INT_TX_BREQ)
#define  USIF_INT_ERR		(USIF_INT_RXUR | USIF_INT_RXOF | USIF_INT_TXUR | \
				 USIF_INT_TXOF | USIF_INT_PE | USIF_INT_FE)

/* UART protocol */
#define USIF_MODE_CFG		0x110
#define  USIF_MODE_CFG_MA	BIT(1)	/* master: internal rate generator */
#define  USIF_MODE_CFG_RX_EN	BIT(2)
#define  USIF_MODE_CFG_LB	BIT(4)	/* internal loopback */
#define  USIF_MODE_CFG_TX_EN	BIT(16)
#define USIF_PRTC_CFG		0x114
#define  USIF_PRTC_CFG_CLEN8	0x08	/* 8 data bits, no parity, 1 stop */
#define USIF_PRTC_STAT		0x118
#define USIF_FDIV_CFG		0x140	/* DEC in 31..16, INC in 8..0 */
#define USIF_BC_CFG		0x144	/* BCRV in 12..0 */
#define USIF_ICTMO_CFG		0x148	/* inter-character timeout, bits */

/* data windows */
#define USIF_TXD		0x40000
#define USIF_RXD		0x80000

/*
 * The USIF's register window: 1 MiB (Infineon), reaching past RXD. The
 * driver reserves and maps only this much; OpenWrt's vr9.dtsi gives the
 * node 16 MiB, which would reach over the FPI peripherals at 0x1E100000.
 */
#define USIF_MAP_SIZE		0x100000

#endif /* USIF_REGS_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Lantiq VR9 USIF UART: baud divider search and FIFO word packing.
 *
 * Derived from Infineon's USIF UART driver (ifxmips_usif_uart.c and
 * ifxmips_usif_uart.h, IFX UEIP), which states the rate formula and the
 * byte order used here:
 *   Copyright (c) 2008 ~ 2009 Infineon Technologies AG
 *   Am Campeon 1-12, 85579 Neubiberg, Germany
 *
 * This file has no kernel dependencies beyond fixed-width integer types so
 * that tests/ can build it on the host.
 */
#ifndef USIF_BAUD_H
#define USIF_BAUD_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/*
 * Rate generator: a fractional divider passes INC out of every INC + DEC
 * module clock cycles, a baud counter divides that by BCRV + 1, and the
 * receiver samples 16 times per bit:
 *
 *   baud = clk / 16 * INC / ((INC + DEC) * (BCRV + 1))
 *
 * The search stays inside the region of Infineon's tables for silicon
 * (100 MHz and 20 MHz clocks): DEC >= INC, i.e. the fractional divider
 * passes at most every other cycle, and BCRV >= 2. A setting more than
 * USIF_BAUD_TOL_PPM off is refused.
 */
#define USIF_FDIV_INC_MAX	511U	/* FDIV_CFG.INC, 9 bits */
#define USIF_FDIV_DEC_MAX	65535U	/* FDIV_CFG.DEC, 16 bits */
#define USIF_BC_DIV_MIN		3U	/* BCRV + 1 */
#define USIF_BC_DIV_MAX		8192U	/* BC_CFG.BCRV, 13 bits, plus one */

/* lowest and highest rate the driver offers for a given module clock */
#define USIF_BAUD_MIN		300U
#define USIF_BAUD_MAX(clk)	((clk) / 96U)	/* 16 * BC_DIV_MIN * 2 */
#define USIF_BAUD_TOL_PPM	10000	/* 1 %: refuse anything worse */

/* inter-character timeout, in bit periods (ICTMO_CFG.TMO, 16 bits) */
#define USIF_ICTMO_MIN_BITS	40U	/* four characters of 10 bits */
#define USIF_ICTMO_MAX		0xffffU

struct usif_baud_cfg {
	u32 inc;	/* FDIV_CFG.INC */
	u32 dec;	/* FDIV_CFG.DEC */
	u32 bcrv;	/* BC_CFG.BCRV, divider minus one */
	u32 tmo;	/* ICTMO_CFG.TMO */
	u64 rate_mbd;	/* achieved rate in milli-baud */
};

/* achieved rate of a divider setting, in milli-baud; 0 if invalid */
u64 usif_baud_rate_mbd(u32 clk_hz, u32 inc, u32 dec, u32 bcrv);

/*
 * Find the divider setting closest to @baud for module clock @clk_hz.
 * Returns 0 and fills @cfg, or -1 if no setting within the field limits
 * comes within USIF_BAUD_TOL_PPM (rate too high or too low for the clock).
 */
int usif_baud_calc(u32 clk_hz, u32 baud, struct usif_baud_cfg *cfg);

/* signed rate error of @cfg against @baud, in parts per million */
long usif_baud_error_ppm(const struct usif_baud_cfg *cfg, u32 baud);

/* register images */
static inline u32 usif_fdiv_word(const struct usif_baud_cfg *cfg)
{
	return (cfg->dec << 16) | (cfg->inc & USIF_FDIV_INC_MAX);
}

static inline u32 usif_bc_word(const struct usif_baud_cfg *cfg)
{
	return cfg->bcrv & (USIF_BC_DIV_MAX - 1);
}

/*
 * FIFO words. With the FIFOs byte-aligned and the TX/RX byte swap enabled
 * (as Infineon configures them on this big-endian SoC), the character that
 * goes on the line first sits in bits 31..24 of the 32-bit word written to
 * TXD, and the first received character in bits 31..24 of the word read
 * from RXD. A partial last word uses its leading bytes.
 */
static inline u32 usif_pack_word(const u8 *buf, unsigned int n)
{
	u32 w = 0;
	unsigned int i;

	for (i = 0; i < 4; i++)
		w = (w << 8) | (i < n ? buf[i] : 0);
	return w;
}

static inline void usif_unpack_word(u32 w, u8 *buf, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n && i < 4; i++)
		buf[i] = w >> (24 - 8 * i);
}

#endif /* USIF_BAUD_H */

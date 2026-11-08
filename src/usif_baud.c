// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lantiq VR9 USIF UART: baud divider search.
 *
 * Derived from Infineon's USIF UART driver (ifxmips_usif_uart.c, IFX UEIP),
 * whose fixed divider table and rate read-back define the rate formula:
 *   Copyright (c) 2009 Infineon Technologies AG
 *   Am Campeon 1-12, 85579 Neubiberg, Germany
 *
 * Infineon's driver only knew table entries up to 115200 Bd; this file
 * computes a setting for any rate instead.
 */
#include "usif_baud.h"

#ifdef __KERNEL__
#include <linux/math64.h>
#define usif_div64(a, b)	div64_u64((a), (b))
#else
#define usif_div64(a, b)	((a) / (b))
#endif

static u64 usif_div_round(u64 a, u64 b)
{
	return usif_div64(a + b / 2, b);
}

u64 usif_baud_rate_mbd(u32 clk_hz, u32 inc, u32 dec, u32 bcrv)
{
	u64 den;

	if (!clk_hz || !inc || inc > USIF_FDIV_INC_MAX ||
	    dec > USIF_FDIV_DEC_MAX || bcrv >= USIF_BC_DIV_MAX)
		return 0;

	den = 16ULL * (inc + dec) * (bcrv + 1);
	return usif_div_round((u64)clk_hz * inc * 1000, den);
}

/*
 * For each INC, the product of the baud counter divider n = BCRV + 1 and
 * the fractional denominator d = INC + DEC must come close to
 * clk * INC / (16 * baud). The smallest n that keeps d inside its field
 * gives the largest d and so the finest step; n + 1 covers rounding at the
 * field limit. About a thousand candidates in all, no floating point.
 */
int usif_baud_calc(u32 clk_hz, u32 baud, struct usif_baud_cfg *cfg)
{
	u64 target = (u64)baud * 1000;
	u64 unit = 16ULL * baud;
	u64 best_err = ~0ULL;
	u32 inc;

	if (!clk_hz || !baud)
		return -1;

	for (inc = 1; inc <= USIF_FDIV_INC_MAX; inc++) {
		u64 num = (u64)clk_hz * inc;
		u64 dmax = inc + USIF_FDIV_DEC_MAX;
		u64 n_lo = usif_div64(num + unit * dmax - 1, unit * dmax);
		u64 n;

		if (n_lo < USIF_BC_DIV_MIN)
			n_lo = USIF_BC_DIV_MIN;

		for (n = n_lo; n <= n_lo + 1 && n <= USIF_BC_DIV_MAX; n++) {
			u64 d = usif_div_round(num, unit * n);
			u64 rate, err;

			if (d < 2ULL * inc || d > dmax)
				continue;
			rate = usif_baud_rate_mbd(clk_hz, inc, d - inc, n - 1);
			err = rate > target ? rate - target : target - rate;
			if (err < best_err) {
				best_err = err;
				cfg->inc = inc;
				cfg->dec = d - inc;
				cfg->bcrv = n - 1;
				cfg->rate_mbd = rate;
			}
		}
	}

	if (best_err == ~0ULL ||
	    usif_div64(best_err * 1000, baud) > USIF_BAUD_TOL_PPM)
		return -1;

	/*
	 * Inter-character timeout: a receive packet ends, and its last
	 * partial FIFO word is handed over, after this many idle bit periods.
	 * Infineon used about half a millisecond; keep that, but never less
	 * than four characters.
	 */
	cfg->tmo = baud / 2000;
	if (cfg->tmo < USIF_ICTMO_MIN_BITS)
		cfg->tmo = USIF_ICTMO_MIN_BITS;
	if (cfg->tmo > USIF_ICTMO_MAX)
		cfg->tmo = USIF_ICTMO_MAX;
	return 0;
}

long usif_baud_error_ppm(const struct usif_baud_cfg *cfg, u32 baud)
{
	u64 target = (u64)baud * 1000;
	u64 diff;
	long ppm;

	if (!baud)
		return 0;
	diff = cfg->rate_mbd > target ? cfg->rate_mbd - target :
					target - cfg->rate_mbd;
	ppm = (long)usif_div_round(diff * 1000, baud);
	return cfg->rate_mbd >= target ? ppm : -ppm;
}

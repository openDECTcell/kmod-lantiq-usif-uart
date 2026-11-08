// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host unit test for src/usif_baud.c (USIF baud divider search and FIFO
 * word packing). Build and run with `make -C tests`.
 *
 * The reference divider settings below are entries of the 100 MHz table in
 * Infineon's ifxmips_usif_uart.c (IFX UEIP), used to check that our rate
 * formula agrees with Infineon's:
 *   Copyright (c) 2009 Infineon Technologies AG
 *   Am Campeon 1-12, 85579 Neubiberg, Germany
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usif_baud.h"

static int failures;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		failures++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

static long labs_l(long v)
{
	return v < 0 ? -v : v;
}

/* the rate formula against settings Infineon shipped for a 100 MHz clock */
static void test_formula(void)
{
	static const struct { u32 inc, dec, bc, baud; } ref[] = {
		{ 1, 92, 6, 9600 },
		{ 25, 601, 12, 19200 },
		{ 25, 288, 12, 38400 },
		{ 75, 551, 12, 57600 },
		{ 75, 238, 12, 115200 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(ref) / sizeof(ref[0]); i++) {
		u64 r = usif_baud_rate_mbd(100000000, ref[i].inc, ref[i].dec,
					   ref[i].bc);
		long ppm = (long)(((long long)r - ref[i].baud * 1000LL) *
				  1000 / ref[i].baud);

		CHECK(labs_l(ppm) < 200, "Infineon %u Bd entry gives %llu mBd",
		      ref[i].baud, (unsigned long long)r);
	}
	CHECK(usif_baud_rate_mbd(100000000, 0, 5, 5) == 0, "inc 0 accepted");
	CHECK(usif_baud_rate_mbd(100000000, 512, 5, 5) == 0, "inc 512 accepted");
	CHECK(usif_baud_rate_mbd(100000000, 1, 5, 8192) == 0, "bcrv 8192 accepted");
}

static void check_cfg(u32 clk, u32 baud, const struct usif_baud_cfg *c)
{
	CHECK(c->inc >= 1 && c->inc <= USIF_FDIV_INC_MAX, "inc %u", c->inc);
	CHECK(c->dec >= c->inc && c->dec <= USIF_FDIV_DEC_MAX,
	      "dec %u (inc %u)", c->dec, c->inc);
	CHECK(c->bcrv >= USIF_BC_DIV_MIN - 1 && c->bcrv < USIF_BC_DIV_MAX,
	      "bcrv %u", c->bcrv);
	CHECK(c->rate_mbd == usif_baud_rate_mbd(clk, c->inc, c->dec, c->bcrv),
	      "rate_mbd inconsistent at %u Bd", baud);
	CHECK(c->tmo >= USIF_ICTMO_MIN_BITS && c->tmo <= USIF_ICTMO_MAX,
	      "tmo %u", c->tmo);
	CHECK((usif_fdiv_word(c) & 0xffff) == c->inc &&
	      (usif_fdiv_word(c) >> 16) == c->dec, "fdiv word");
	CHECK(usif_bc_word(c) == c->bcrv, "bc word");
}

/* exhaustive search over every n and INC, for comparison */
static u64 brute_force_err(u32 clk, u32 baud)
{
	u64 target = (u64)baud * 1000, best = ~0ULL;
	u32 inc, n;

	for (n = USIF_BC_DIV_MIN; n <= USIF_BC_DIV_MAX; n++) {
		for (inc = 1; inc <= USIF_FDIV_INC_MAX; inc++) {
			u64 num = (u64)clk * inc, unit = 16ULL * baud * n;
			u64 d0 = num / unit, d;

			for (d = d0; d <= d0 + 1; d++) {
				u64 r, e;

				if (d < 2ULL * inc ||
				    d > inc + USIF_FDIV_DEC_MAX)
					continue;
				r = usif_baud_rate_mbd(clk, inc, d - inc,
						       n - 1);
				e = r > target ? r - target : target - r;
				if (e < best)
					best = e;
			}
		}
	}
	return best;
}

static void print_table(u32 clk, const u32 *rates, unsigned int n)
{
	unsigned int i;

	printf("\nmodule clock %u Hz\n", clk);
	printf("  %8s %4s %6s %5s %14s %9s %5s\n", "baud", "INC", "DEC",
	       "BCRV", "achieved", "error", "TMO");
	for (i = 0; i < n; i++) {
		struct usif_baud_cfg c;

		if (usif_baud_calc(clk, rates[i], &c)) {
			printf("  %8u   (no setting)\n", rates[i]);
			continue;
		}
		printf("  %8u %4u %6u %5u %10llu.%03llu %+7.4f%% %5u\n",
		       rates[i], c.inc, c.dec, c.bcrv,
		       (unsigned long long)(c.rate_mbd / 1000),
		       (unsigned long long)(c.rate_mbd % 1000),
		       usif_baud_error_ppm(&c, rates[i]) / 10000.0, c.tmo);
	}
}

static void test_rates(void)
{
	/* the rates openDECTcell asked for (openDECTcell's list) */
	static const u32 needed[] = { 9600, 19200, 115200, 230400, 460800 };
	static const u32 standard[] = {
		300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200,
		230400, 460800, 921600, 1000000, 1500000,
	};
	/*
	 * 100 MHz is the expected module clock (PLL0 1 GHz / 10); the others
	 * cover a mis-derived or differently configured PLL.
	 */
	static const u32 clocks[] = {
		100000000, 98304000, 96000000, 83333333, 125000000, 50000000,
	};
	unsigned int i, j;

	for (j = 0; j < sizeof(clocks) / sizeof(clocks[0]); j++) {
		for (i = 0; i < sizeof(standard) / sizeof(standard[0]); i++) {
			struct usif_baud_cfg c;
			u32 b = standard[i];

			if (b > USIF_BAUD_MAX(clocks[j]))
				continue;
			CHECK(!usif_baud_calc(clocks[j], b, &c),
			      "no setting for %u Bd at %u Hz", b, clocks[j]);
			check_cfg(clocks[j], b, &c);
			/* 0.1 % for every rate the driver offers */
			CHECK(labs_l(usif_baud_error_ppm(&c, b)) <= 1000,
			      "%u Bd at %u Hz: %ld ppm", b, clocks[j],
			      usif_baud_error_ppm(&c, b));
		}
	}

	for (i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
		struct usif_baud_cfg c;

		usif_baud_calc(100000000, needed[i], &c);
		CHECK(labs_l(usif_baud_error_ppm(&c, needed[i])) <= 10,
		      "%u Bd at 100 MHz: %ld ppm", needed[i],
		      usif_baud_error_ppm(&c, needed[i]));
	}

	/* the search must find (nearly) the global optimum */
	for (i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
		struct usif_baud_cfg c;
		u64 got, best;

		usif_baud_calc(100000000, needed[i], &c);
		got = c.rate_mbd > needed[i] * 1000ULL ?
			c.rate_mbd - needed[i] * 1000ULL :
			needed[i] * 1000ULL - c.rate_mbd;
		best = brute_force_err(100000000, needed[i]);
		/* within 1 ppm of the exhaustive optimum */
		CHECK(got <= best + needed[i] / 1000,
		      "%u Bd: %llu mBd off, optimum %llu",
		      needed[i], (unsigned long long)got,
		      (unsigned long long)best);
	}

	/* out of range */
	{
		struct usif_baud_cfg c;

		CHECK(usif_baud_calc(100000000, 2000000, &c) != 0,
		      "2 MBd at 100 MHz should be impossible");
		CHECK(usif_baud_calc(100000000, USIF_BAUD_MAX(100000000), &c)
		      == 0, "USIF_BAUD_MAX at 100 MHz refused");
		CHECK(usif_baud_calc(0, 9600, &c) != 0, "clock 0 accepted");
		CHECK(usif_baud_calc(100000000, 0, &c) != 0, "baud 0 accepted");
	}

	print_table(100000000, standard, sizeof(standard) / sizeof(standard[0]));
	print_table(98304000, needed, sizeof(needed) / sizeof(needed[0]));
}

static void test_packing(void)
{
	const u8 msg[] = { 0x11, 0x22, 0x33, 0x44 };
	u8 out[4];

	CHECK(usif_pack_word(msg, 4) == 0x11223344, "pack 4");
	CHECK(usif_pack_word(msg, 1) == 0x11000000, "pack 1");
	CHECK(usif_pack_word(msg, 3) == 0x11223300, "pack 3");

	memset(out, 0, sizeof(out));
	usif_unpack_word(0xa1b2c3d4, out, 4);
	CHECK(out[0] == 0xa1 && out[1] == 0xb2 && out[2] == 0xc3 &&
	      out[3] == 0xd4, "unpack 4");
	memset(out, 0, sizeof(out));
	usif_unpack_word(0xa1b2c3d4, out, 2);
	CHECK(out[0] == 0xa1 && out[1] == 0xb2 && out[2] == 0, "unpack 2");
}

int main(void)
{
	test_formula();
	test_rates();
	test_packing();

	if (failures) {
		printf("\n%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("\nall checks passed\n");
	return EXIT_SUCCESS;
}

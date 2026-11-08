# VR9 USIF in UART mode: hardware facts

What the driver relies on, in our own words, with the source of each fact
and how it is known. "Infineon" means the IFX UEIP USIF UART driver and its
register header in AVM's FRITZ!Box 7362 SL GPL source (FRITZ!OS 07.12,
`kernel/linux/drivers/tty/serial/ifxmips_usif_uart.c`, `ifxmips_usif_uart.h`,
`ifxmips_usif_reg.h`). "6.12" means Linux v6.12.112 as used by OpenWrt
25.12.6. Facts observed on the 7362 SL are marked as such; everything else
is derived statically from those sources.

## Location, interrupts

| Fact | Value | Source |
|---|---|---|
| Physical base | `0x1DA00000` | Infineon header; OpenWrt `vr9.dtsi` `usif@da00000` under the FPI bus (`ranges` 0x10000000) |
| Window | **1 MiB** (`0x1DA00000`–`0x1DAFFFFF`), registers up to `0x80000` (RXD) used. The DT node says 16 MiB, which would reach over the FPI peripherals at `0x1E100000`; the driver reserves and maps only 1 MiB, and the fallback registers its device with 1 MiB | Infineon header (`IFX_USIF_SIZE`), `vr9.dtsi`; the 16 MiB problem observed on hardware |
| Device name in 6.12 | `1da00000.usif` | derived from the DT node; matches 6.12's clock lookup entry |
| Interrupts (OpenWrt ICU numbering) | TX request 29, RX request 125, error 107, status 108, autobaud 109, wake-up 110 | `vr9.dtsi` order; Infineon's VR9 `irq.h` (IM0 line 21, IM3 lines 21, 3, 4, 5, 6) maps to the same numbers with the ICU's offset of 8 |
| DT node state on a stock 7362 SL | present, `status = "disabled"` | `vr9.dtsi`, `vr9_avm_fritz7362sl.dts` |

## Registers used

All 32 bits, native-endian on this big-endian SoC: `__raw_readl` /
`__raw_writel`. Offsets from the base.

| Offset | Name | What the driver does with it |
|---|---|---|
| 0x000 | CLC | mode requests: bit 0 run, bit 1 configuration, bit 2 module enable (set/clear pairs; a 0 leaves a bit alone). **Write only:** no vendor code reads it, and reading it most likely hung the bus on the box; never read it |
| 0x004 | CLC_CNT | clock divider RMC (bits 7..0) and ORMC (15..8); both set to 1 before enabling |
| 0x008 | CLC_STAT | handshake: configuration mode reached when MODEN (1), counter update done (6), kernel idle (7) and FIFO idle (8) are set; run mode additionally sets RUN (0) |
| 0x00C | ID | logged at probe |
| 0x010 | FIFO_ID | TX depth in words (7..0), RX depth (15..8) |
| 0x02C | DPLUS_CTRL | written once per run-mode entry with TX DMA disabled, all RX classes masked: no DMA |
| 0x030 | FIFO_CFG | RX/TX burst size (code 2 = 4 words), flow-control mode, byte alignment, byte swap (bits 11 and 15) |
| 0x034 | FIFO_CTRL | RX auto request on (bit 6), written in configuration mode after FIFO_CFG |
| 0x038 | MRPS_CTRL | maximum receive packet size; 0 = unlimited, rewritten after every switch to run mode (it resets) |
| 0x044 | FIFO_STAT | RX fill in words (7..0), TX fill in words (23..16) |
| 0x048 | TXD_SB | side band for the next TXD write: end of message (bit 7) and valid byte count (5..4, 0 = four) |
| 0x04C | DPLUS_STAT | for the last word of a receive packet: RME (bit 7) and valid byte count (5..4, 0 = four); read before that word |
| 0x080..0x098 | RIS, IMSC, MIS, ISR, ICR | raw status, mask, masked status, set, clear |
| 0x110 | MODE_CFG | master (1), RX enable (2), loopback (4), TX enable (16) |
| 0x114 | PRTC_CFG | character length 8 (value 8), no parity, one stop bit |
| 0x118 | PRTC_STAT | dumped only |
| 0x140 | FDIV_CFG | fractional divider: DEC in 31..16, INC in 8..0 |
| 0x144 | BC_CFG | baud counter reload BCRV in 12..0 |
| 0x148 | ICTMO_CFG | inter-character timeout (15..0), in bit periods (see below) |
| 0x40000 | TXD | transmit data window |
| 0x80000 | RXD | receive data window |

Interrupt bits (IMSC/MIS/ICR): RX last single 0, RX single 1, RX last burst
2, RX burst 3, TX single 5, TX burst 7; RX underrun 11, RX overflow 12, TX
underrun 13, TX overflow 14, parity 15, framing 16; transmission finished
29. The data requests arrive on the TX (29) and RX (125) lines, the error
bits on 107, the status bits on 108.

Source: Infineon header and driver, derived statically.

**Observed on hardware** (7362 SL, 2026-11-01, see
`docs/hardware-test.md` "Results"): the module clock is 100 MHz
(CGU_PLL0_CFG `00b01f23`; loopback transfers ran within −0.04 % of
nominal at 9600..460800 Bd), the FIFOs are 8 words each, the byte-lane
order is right (1..9 byte echoes), and the `regs` and `pads` files read
safely. On the real pins, openDECTcell's
firmware upload ran through the driver at 9600 and 230400 Bd, both stages
on the first attempt, 0 overruns. Reads by probe: ID
(`02045190`), FIFO_ID (8/8 words), CLC_STAT (handshakes completed),
MODE_CFG, FIFO_STAT, and CGU PLL0_CFG (giving 100 MHz). Infineon's normal
path also reads CLC_CNT, PRTC_CFG, FDIV_CFG, BC_CFG, FIFO_CFG, DPLUS_STAT,
IMSC, MIS and RXD; only its debug dump reads PRTC_STAT, ICTMO_CFG, TXD_SB,
MRPS_CTRL, RIS and TXD.

### Notes and open points

- **FIFO_STAT field overlap.** Infineon's header gives the TX fill level as
  bits 23..16 but also places RX_AR_ON, RX_EOP, RX_THR_EXC and MRPS_WE at
  bits 16..19. Infineon's driver uses only the TX fill level (and would hang
  in its own `set_termios` if bit 16 were stuck), so the driver takes 23..16
  as the TX fill. To be safe, `tx_empty` also gives up after the time the
  FIFO needs to drain and counts that (`tx_forced` in debugfs).
- **Byte order in FIFO words.** With byte alignment and both swap bits set
  (as Infineon configures), Infineon's driver puts the first character into
  the byte at the lowest address of the word, which on this big-endian CPU
  is bits 31..24, and reads received characters the same way. The driver
  does the same. `tools/usif-loop` checks this in loopback with 1..9 byte
  writes: a wrong lane would lose the single bytes. **Observed on
  hardware (2026-11-01):** 1..9 byte echoes and bulk data byte-exact at all
  five rates in loopback.
- **Receive requests.** BREQ: a full burst (four words) is ready. SREQ: one
  word. LBREQ: the last burst of a packet, its last word partial. LSREQ: the
  last word of a packet, partial. A packet ends after the inter-character
  timeout; its last word's byte count is in DPLUS_STAT. If RME is not set
  there, Infineon drops that word; the driver does the same and counts it
  (`rx_no_rme`).
- **Configuration mode with a non-empty FIFO.** Infineon refuses to change
  termios while either FIFO holds data. The driver drains TX first and
  discards RX before requesting configuration mode, and bounds the
  handshake (100 µs).
- **AVM boot-time tweaks not done by OpenWrt** (AVM's `arch/mips/lantiq/vr9/vr9.c`):
  bit 30 of the PMU word at `0x1F10220C` ("USIF ECO fix", purpose unknown)
  and bit 3 of the RCU word at `0x1F20304C` (AHB slave port towards the XBAR
  in big-endian mode; the same register has a separate bit 2 for the USIF's
  AHB slave port, which AVM leaves alone). Both are dumped read-only in
  debugfs. If the USIF misbehaves on hardware, these are the first
  suspects.

## Clock

- **Gate.** 6.12's `arch/mips/lantiq/xway/sysctrl.c` registers, for VR9, a
  PMU clock gate for device `1da00000.usif` on PMU power-down bit 7, and
  switches it off at boot. Its connection id is the *string* `"NULL"`, not a
  null pointer, so `clk_get(dev, NULL)` does not find it; the driver asks
  for `"NULL"` first. A shared gate `"ahb"` (AHB master and slave, PMU bits
  15 and 13) is also switched off at boot and switched on by the USB and
  PCIe drivers; Infineon enables the AHB master together with the USIF. The
  driver enables `"ahb"` and never disables it, because the legacy xway
  clocks keep no enable count. Source: 6.12 sysctrl.c and clk.c, derived
  statically.
- **Rate.** 6.12 attaches no rate to the USIF gate (`clk_get_rate` returns
  0). Infineon's VR9 clock code (AVM `arch/mips/lantiq/vr9/ifxmips_clk.c`)
  defines the USIF module clock as **PLL0 / 10**, with
  PLL0 = 36 MHz × 2 (N + 1) / (M + 1) from `CGU_PLL0_CFG` (CGU base
  `0x1F103000` + 4; enable bit 0, M in 5..2, N in 13..6, bypass bit 30, a
  fractional part it ignores). With CLC_CNT RMC = 1 the USIF kernel clock
  equals the module clock. Infineon's divider table for "silicon" is the
  100 MHz one, which matches the usual 1 GHz PLL0. The driver reads
  `CGU_PLL0_CFG` (read only), computes PLL0 / 10, accepts 50..150 MHz and
  otherwise falls back to 100 MHz; `clock_hz=` overrides it.
  **Observed on hardware (2026-11-01):** `CGU_PLL0_CFG` = `00b01f23` gives
  100 MHz, and the loopback rates came out within 0.04 % of nominal, so
  the clock is confirmed to that accuracy. Originally: confidence medium. The formula and the 36 MHz crystal are Infineon's
  (derived statically); that this box's PLL0 gives 100 MHz is expected but
  unobserved. `tools/usif-loop` measures the effective rate in loopback,
  which confirms or refutes it to a fraction of a percent.

## Baud rate generator

Infineon's rate read-back and table imply

    baud = clk / 16 × INC / ((INC + DEC) × (BCRV + 1))

The driver searches INC (1..511) with the smallest baud-counter divider that
keeps INC + DEC inside its 16-bit field, staying inside the region of
Infineon's silicon tables (DEC ≥ INC, BCRV ≥ 2) and refusing anything more
than 1 % off. Rates from 300 Bd to clk / 96 (1.04 MBd at 100 MHz).

At 100 MHz (host unit test, `make -C tests`):

| Requested | INC | DEC | BCRV | Achieved | Error |
|---|---|---|---|---|---|
| 9600 | 72 | 15553 | 2 | 9600.000 | 0 |
| 19200 | 144 | 15481 | 2 | 19200.000 | 0 |
| 115200 | 509 | 8696 | 2 | 115200.072 | +0.6 ppm |
| 230400 | 71 | 571 | 2 | 230399.792 | −0.9 ppm |
| 460800 | 71 | 250 | 2 | 460799.585 | −0.9 ppm |

The fractional divider passes INC of every INC + DEC clock cycles, so the
sampling clock jitters by one module clock period (10 ns); at 460800 Bd
that is under 0.5 % of a bit, well inside what a UART receiver tolerates.

The inter-character timeout (end of a receive packet, after which the last
partial FIFO word is handed over) is set to max(40, baud / 2000) bit periods:
four characters at low rates, about half a millisecond at high rates, which
is what Infineon's table used. Its unit (bit periods vs. oversampling
ticks) is inferred from Infineon's values, not documented.

## FIFOs and timing at high rates

Infineon's header describes the TX FIFO as 32 bytes (8 words); the driver
reads both depths from FIFO_ID at probe. With bursts of four words the RX
interrupt comes every 16 bytes: every 0.7 ms at 230400 Bd, every 0.35 ms at
460800 Bd. A full 32-byte RX FIFO lasts 1.4 ms at 230400 Bd and 0.7 ms at
460800 Bd of interrupt latency.

**Known risk on this platform:** kmod-lantiq-ssc-pcm measured that
mainline `xway_nand.c` keeps interrupts off on one CPU for 26..28 ms while
reading a 128 KiB NAND block (`ubihealthd` does that every 120 s). If the
USIF RX interrupt is served by that CPU at that moment, receive data beyond
the FIFO is lost and counted as an overrun. Transmit only pauses. Observed
(session 8): an upload with `ubihealthd` running and 498 NAND reads looping
on both CPUs passed, 0 overruns; the peer sends only ~8 bytes during an
upload, so receive was hardly exposed. A kernel console line (115200 Bd)
also holds the printing CPU ~87 µs per character (kmod-lantiq-tdm-pcm). Mitigations
for a user: keep bursts from the peer short, retry on overrun (visible via
`TIOCGICOUNT` and debugfs), or keep `ubihealthd` from scanning during an
upload.

## Pins (FRITZ!Box 7362 SL)

| GPIO | Use | Mux (ALTSEL0/1) | Pad | Source |
|---|---|---|---|---|
| 11 | USIF UART RX | 1/0, group `usif uart_rx`, function `usif` | input | pinctrl-xway xrx200 table; A-0001 §3 (stock board setup) |
| 12 | USIF UART TX | 1/0, group `usif uart_tx`, function `usif` | output, **push-pull** (as the stock board, presumably; see below) | same; A-0004 §5 corrects A-0001 §3 on the pad mode |
| 30 | DECT reset | — | **never touched by this driver** | A-0001 §3 |

pinctrl-xway sets only the mux from the group; pad direction and open drain
are separate pad settings (`lantiq,output`, `lantiq,open-drain`; packed
config parameters 4 and 1). The xway open-drain register bit reads 1 for
push-pull and 0 for open drain. pinctrl-xway has no hook that undoes a mux
when a pin is released, so the mux registers keep the USIF function after
unload; the driver's "sleep" state makes both pads inputs before it lets go.

**TX pad mode.** The driver drives the TX pad **push-pull** (since
session 8; `pads` reads `output, altsel 1/0, push-pull`). [CR 7362SL/A-0004]
§5 corrected the earlier "open drain" reading of the stock board setup
(A-0001 §3): the stock flag sets the push-pull bit (derived statically).
Observed on hardware, 2026-11-01: open drain (session 5) and push-pull
(session 8) both carried openDECTcell's upload at 230400 Bd without error, so
the line also has a pull-up; "released" (pad an input after unload) then
still reads high.

# kmod-lantiq-usif-uart

> **Note:** this module was written by an AI coding agent under the direction of the openDECTcell maintainers; only smaller
> parts have been reviewed by a human. It is provided without any warranty, is purely experimental and has been tested only on
> the hardware named below.
> The Status section says what was verified.

A Linux serial driver for the **Lantiq VR9 USIF in UART mode**, for
OpenWrt 25.12.x (Linux 6.12, `lantiq/xrx200`). It registers one port,
`/dev/ttyUSIF0`, and is meant for the AVM FRITZ!Box 7362 SL, where the USIF
UART on GPIO 11 (RX) and GPIO 12 (TX) leads to the DECT chip. The driver
itself is a plain serial port and knows nothing about DECT.

## Status

Compile-tested with the OpenWrt 25.12.6 SDK (W=1 clean, checkpatch clean),
OpenWrt package builds, host unit test of the baud divider passes.
**Tested on a FRITZ!Box 7362 SL** (2026-11-01): loads on
the stock image, all five rates byte-exact in internal loopback within
0.04 % of nominal, pins 11/12 claimed and released correctly, clean
unload. Details in `docs/hardware-test.md`, "Results".
**First real use on the pins** (2026-11-01): openDECTcell's
DECT firmware upload ran through `/dev/ttyUSIF0`, stage 1 at 9600 Bd and
stage 2 at 230400 Bd, both on the first attempt, 0 overruns, 1 framing
error from openDECTcell's boot-strap window on the RX pin (expected). **Since session 8** the TX pad is push-pull
like the stock board's; the upload passed again with it, also with
`ubihealthd` running and NAND reads looping on both CPUs.

## What it does

- Serial-core driver, tty name `ttyUSIF`, one port, dynamic major (it does
  not take `ttyS` 4:64).
- 8N1 raw only; no hardware flow control, modem lines, console, autobaud,
  power management or procfs.
- Any rate from 300 Bd to module clock / 96, computed from the fractional
  divider and baud counter; 9600, 19200, 115200, 230400 and 460800 Bd come
  out within 1 ppm at the 100 MHz module clock (observed on the 7362 SL).
- Rate changes at runtime through termios. Every change that reaches the
  driver also drops what is in the receive FIFO and what arrives while the
  rate changes, so after `tcsetattr(TCSAFLUSH)` no old-rate input is left.
- RX and TX by interrupt (TX request, RX request, error and status lines).
- Platform driver for `compatible = "lantiq,usif"`. On a stock 7362 SL
  image, where that node is disabled, a small fallback (only on
  `avm,fritz7362sl`) creates the device from the disabled node and registers
  pinctrl maps for GPIO 11/12. No other pin is touched; never GPIO 30.
- Not loaded at boot; the user (openDECTcell's service) loads it.
- debugfs in `/sys/kernel/debug/1da00000.usif/`: `state` (no hardware
  access: clock, divider, counters), `regs`, `regs-debug`, `soc`, `pads`
  (register reads graded by risk, see `docs/hardware-test.md`).
- Module parameters: `clock_hz=` (override the derived module clock),
  `loopback=1` (test only: internal loopback, pads left as inputs).

## Layout

| Path | Contents |
|------|----------|
| `Makefile` | OpenWrt package `kmod-lantiq-usif-uart` → `lantiq_usif_uart.ko` |
| `src/usif_uart.c` | serial-core driver |
| `src/usif_regs.h` | USIF register map (UART subset) |
| `src/usif_baud.[ch]` | divider search, FIFO word packing (host-testable) |
| `src/usif_fallback.c` | bring-up fallback for the stock 7362 SL image |
| `tests/` | host unit test (`make -C tests`) |
| `tools/build.sh` | SDK package build → `deploy/` |
| `tools/check.sh` | unit test, W=1 kbuild, checkpatch |
| `tools/usif-loop.c` | loopback exercise tool for the box |
| `docs/hardware.md` | register, clock and pin facts with sources |
| `docs/hardware-test.md` | hardware test card |

## Build

    tools/check.sh [sdk]     # host test + W=1 kbuild + checkpatch
    tools/build.sh [sdk]     # deploy/lantiq_usif_uart.ko, .apk, usif-loop

Default SDK: `build/openwrt-sdk-*lantiq-xrx200*` in this repository
(gitignored). Direct kbuild, as `check.sh` does it:

    PATH=$SDK/staging_dir/toolchain-*/bin:$PATH \
    STAGING_DIR=$SDK/staging_dir/target-mips_24kc_musl \
    make -C $SDK/build_dir/target-mips_24kc_musl/linux-lantiq_xrx200/linux-6.12.112 \
         M=<copy of src> ARCH=mips CROSS_COMPILE=mips-openwrt-linux-musl- W=1 modules

## Use

    apk add --allow-untrusted kmod-lantiq-usif-uart-*.apk
    modprobe lantiq_usif_uart        # or: insmod lantiq_usif_uart.ko
    stty -F /dev/ttyUSIF0 raw -echo 230400

## Licence

This driver is a **derivative work** of Infineon's GPL-2.0-or-later USIF UART driver (IFX UEIP, published in the vendor's GPL
source drop for the 7362 SL). The files derived from it (`src/usif_uart.c`, `src/usif_regs.h`, `src/usif_baud.[ch]`,
`tests/test_baud.c`) keep Infineon's copyright notice and are GPL-2.0-or-later; they are **not** clean-room work. The
fallback, scripts and tool are GPL-2.0-only, and the module as a whole is GPL-2.0-only. The texts are in `LICENSES/`.

## Provenance

The hardware facts in `docs/` come from public sources (data sheets, the kernel's own drivers, the vendor's published GPL
source drops) and from observing the hardware; where a fact comes from the clean-room record of the openDECTcell project it is
labelled `[CR ...]` or `A-nnnn`, see [opendectcell's clean-room-process.md](https://github.com/openDECTcell/opendectcell/blob/main/docs/clean-room-process.md).
No vendor firmware, binaries or kernel modules are in this repository.

## Relationship to openDECTcell

This is a plain serial driver that knows nothing about DECT; [openDECTcell](https://github.com/openDECTcell/opendectcell) uses it to
upload the DECT chip's firmware on the 7362 SL.

# Hardware test card: FRITZ!Box 7362 SL, first load

Purpose: the first run of `lantiq_usif_uart.ko` on real hardware. It checks
that the module loads on a stock OpenWrt 25.12.6 image, creates
`/dev/ttyUSIF0`, claims GPIO 11/12 and nothing else, takes rate changes,
moves data correctly through the FIFOs at every rate (in internal loopback,
so **nothing goes out on the pins**), and unloads cleanly.

**Only with the maintainer's agreement.** Run the steps in order; stop at
the first unexpected result and send back what you have.

## Ground rules

- **GPIO 30 is the DECT chip's reset. Nothing in this test drives it, and
  the DECT chip must not be released from reset.** Do not use
  `/sys/class/gpio`, do not load or start anything from openDECTcell, do not
  unload openDECTcell's module if it happens to be loaded (stop and ask instead).
- Data is written to `/dev/ttyUSIF0` **only** in test A (`loopback=1`,
  pads not driven). In test B nothing is written to the port.
- If the box stops answering: wait 15 s (panic reboot, see step 1); if it
  does not come back, power-cycle it.

## 0. Build (workstation)

    cd kmod-lantiq-usif-uart
    tools/check.sh          # host unit test, W=1 build, checkpatch
    tools/build.sh          # deploy/lantiq_usif_uart.ko, .apk, usif-loop
    git log --oneline -1    # note the commit
    scp -O deploy/lantiq_usif_uart.ko deploy/usif-loop root@192.168.1.1:/tmp/

## 1. Baseline (box)

**Reboot the box first if an earlier build (`e561d32` or older) was ever
loaded on it.** That build's failed probe left the kernel's I/O memory
resource tree damaged; only a reboot
repairs it.

    cat /etc/openwrt_release | grep DISTRIB_RELEASE   # 25.12.6
    uname -r                                          # 6.12.112
    lsmod | grep -i -e usif -e dect                   # expect nothing; else stop
    mount | grep -q debugfs || mount -t debugfs debugfs /sys/kernel/debug
    sysctl -w kernel.panic=10
    P=/sys/kernel/debug/pinctrl/1e100b10.pinmux/pinmux-pins
    grep -E '^pin (11|12|30) ' $P
    cat /sys/kernel/debug/gpio
    cat /proc/iomem > /tmp/iomem.before
    grep -c . /tmp/iomem.before          # note the count
    chmod +x /tmp/usif-loop

Expected: pins 11 and 12 `(MUX UNCLAIMED) (GPIO UNCLAIMED)`. Note what pin
30 shows; it must show the same at the end.

## 2. Test A: internal loopback (pads not driven)

The debugfs directory `/sys/kernel/debug/1da00000.usif/` has five files,
graded by risk (see "debugfs files" below). **Read only `state` until
`usif-loop` has run.** On the second run (`62a4528`) a read of the old
`regs` file hung the bus and the box rebooted; the register it most likely
hung on is no longer read at all.

    insmod /tmp/lantiq_usif_uart.ko loopback=1
    dmesg | tail -n 5
    ls -l /dev/ttyUSIF0
    grep usif /proc/tty/drivers
    grep -i usif /proc/iomem
    grep -E '^pin (11|12) ' $P
    U=/sys/kernel/debug/1da00000.usif
    ls $U
    cat $U/state

Expected:

- dmesg: `lantiq-usif-uart: fallback device for avm,fritz7362sl registered`,
  `1da00000.usif: ttyUSIF0 at MMIO 0x1da00000 ... is a USIF`, and
  `ttyUSIF0: ID 02045190, FIFO 8/8 words, clock 100000000 Hz (CGU PLL0 / 10),
  irq 29 125 107 108, loopback` (as seen on the second run).
- `/dev/ttyUSIF0`: character device, major as in `/proc/tty/drivers`
  (`lantiq_usif /dev/ttyUSIF <major> 0 serial`), not 4.
- `/proc/iomem`: `1da00000-1dafffff : usif@da00000` with
  `1da00000-1dafffff : 1da00000.usif` nested; nothing at `1e100000`+.
- pins 11/12 still `(MUX UNCLAIMED)`.
- `ls $U`: `pads regs regs-debug soc state`.
- `state`: no hardware access; id, clock, `baud 9600 requested`, divider,
  FIFO depths, counters all zero.

Then the exercise (about 15 s):

    /tmp/usif-loop
    cat $U/state

Expected from `usif-loop`, for 9600, 19200, 115200, 230400, 460800, 9600:

- `TCSAFLUSH left 0 stale bytes`, at every step. (The build used on the
  2026-11-01 run reported 5 at the first step: it wrote before switching
  ECHO off, and the loopback kept the echoes circulating; fixed in the tool.)
- `1 byte echoed after ... ms`: about one character plus the receive
  timeout (≈ 5 ms at 9600, ≈ 1 ms at 230400)
- `1..9 byte echoes ok` (a wrong byte-lane assumption fails here)
- `bulk N/N bytes back, 0 differ, effective R Bd (±x %)`: R within ±0.5 % of
  the nominal rate. **If every rate is off by the same factor, the module
  clock is not 100 MHz**: note the factor; a retest with
  `insmod ... loopback=1 clock_hz=<100000000 × factor>` should then pass.
- `icount ... overrun +0 frame +0`, `PASS` per rate, `ALL PASS` at the end.
- `state` afterwards: `anomaly` all zero; `irq` counters non-zero;
  `tx_fin` tells whether the status line fires per message.

Register files, **one per command, in this order; if the box hangs or
reboots, note which file it was and stop**:

    cat $U/regs           # USIF registers probe already read, or Infineon's normal path reads
    cat $U/pads           # GPIO port 0, same registers pinctrl-xway reads
    cat $U/soc            # PMU/RCU/CGU words
    cat $U/regs-debug     # USIF registers only Infineon's debug dump read

Expected in `regs`: `CLC_STAT` ending in `1c3`, `MODE_CFG` with bit 4
(loopback); in `pads`: `pad io11: ... input`, `pad io12: ... input`.

Unload:

    rmmod lantiq_usif_uart
    dmesg | tail -n 3
    ls /dev/ttyUSIF0                     # expect: no such file
    grep -E '^pin (11|12) ' $P
    cat /proc/iomem | diff /tmp/iomem.before - && echo iomem unchanged

**Fallback if a `cat` of a register file reboots the box:** after the
reboot (no module loaded, `kernel.panic=10` set again), run only

    insmod /tmp/lantiq_usif_uart.ko loopback=1
    /tmp/usif-loop
    cat /sys/kernel/debug/1da00000.usif/state
    rmmod lantiq_usif_uart

and send that back together with the name of the file that hung. Skip
test B's register reads in that case except `state`.

## 3. Test B: normal mode, pins claimed, nothing sent

    insmod /tmp/lantiq_usif_uart.ko
    dmesg | tail -n 4
    grep -E '^pin (11|12|30) ' $P
    cat $U/state

Expected:

- pin 11: `1da00000.usif (GPIO UNCLAIMED) function usif group usif uart_rx`
- pin 12: `1da00000.usif (GPIO UNCLAIMED) function usif group usif uart_tx`
- pin 30: unchanged from step 1.

Rate changes (OpenWrt 25.12.6's busybox has **no `stty`**; `usif-loop -r`
opens the port the way dect-fwload does, sets raw mode first and applies
each rate with `TCSAFLUSH`, **never writing a byte**):

    /tmp/usif-loop -r 9600 230400 460800 230400 9600

Expected per rate: `accepted, reads back R/R Bd`; the driver's lines, e.g.
`baud 230400 requested, 230399.792 achieved (-1 ppm)` with
`divider INC 71 DEC 571 BCRV 2` (460800: `INC 71 DEC 250 BCRV 2`, 9600:
`INC 72 DEC 15553 BCRV 2`); and how many bytes arrived during one second
(0 expected while the DECT chip stays in reset); `ALL PASS`.

Passive listen, read only (shows whether the chip sends anything as it is;
`usif-loop -r` above has left the port raw, so nothing is echoed):

    cat /dev/ttyUSIF0 > /tmp/usif-rx.bin & sleep 1
    grep 1da00000.usif /proc/interrupts
    sleep 4; kill $!
    wc -c /tmp/usif-rx.bin; hexdump -C /tmp/usif-rx.bin | head -n 8
    cat $U/state

Pad state (only if `pads` was harmless in test A):

    cat $U/pads

Expected: `pad io11: level ?, input, altsel 1/0, ...`;
`pad io12: ..., output, altsel 1/0, push-pull, ...` (open-drain before
session 8). The level of io11
tells whether the DECT chip's TX idles high.

Unload and reload:

    rmmod lantiq_usif_uart
    grep -E '^pin (11|12|30) ' $P        # 11/12 (MUX UNCLAIMED), 30 unchanged
    dmesg | tail -n 3
    insmod /tmp/lantiq_usif_uart.ko && rmmod lantiq_usif_uart
    dmesg | tail -n 6                    # a second clean probe and removal
    cat /sys/kernel/debug/gpio           # same as step 1
    cat /proc/iomem | diff /tmp/iomem.before - && echo iomem unchanged

## debugfs files

| File | Reads | Status |
|---|---|---|
| `state` | nothing from hardware (values cached at probe, counters) | safe |
| `regs` | USIF ID, FIFO_ID, CLC_STAT, MODE_CFG, FIFO_STAT (all read by probe on the box), CLC_CNT, PRTC_CFG, FDIV_CFG, BC_CFG, FIFO_CFG, DPLUS_STAT, IMSC, MIS (read in Infineon's normal path) | safe (read on run 4, 2026-11-01) |
| `pads` | GPIO port 0 at 0x1E100B10 (+0x00..0x20), the registers pinctrl-xway itself reads | safe (read on run 4, 2026-11-01) |
| `soc` | PMU 0x1F10201C/20/20C, RCU 0x1F20304C, CGU 0x1F103004 (the last read by probe) | uncertain (PMU 0x20C, RCU 0x4C) |
| `regs-debug` | USIF PRTC_STAT, ICTMO_CFG, TXD_SB, MRPS_CTRL, RIS (only Infineon's debug dump reads them) | uncertain |

The USIF's CLC register (offset 0, write-only in all vendor code) is not
read by any file or by the driver.

## 4. Send back

Everything printed, in order, in particular:

1. the commit from step 0 and the baseline of step 1 (including pin 30),
2. all `dmesg` excerpts,
3. every debugfs output, with the file name (and, if a read hung, which),
4. the full `usif-loop` output,
5. the pinmux lines after each load/unload, the `/proc/iomem` lines and
   diffs, the `usif-loop -r` output,
6. the `/proc/interrupts` lines and the passive-listen size and hexdump,
7. anything unexpected (warnings, `anomaly` counters not zero, a slow
   command).

## Results

**Run 4, 2026-11-01** (observed on hardware, by the maintainers;
`lantiq_usif_uart.ko` sha256 `5fcd68e9…`, `usif-loop`
`e59d277e…`): tests A and B passed.

- Load: rc 0; ID `02045190`, FIFO 8/8 words, 100 MHz from CGU_PLL0_CFG
  `00b01f23`; `/dev/ttyUSIF0` major 248; `/proc/iomem` only the 1 MiB
  window; five debugfs files.
- `usif-loop` (loopback): ALL PASS at 9600, 19200, 115200, 230400, 460800,
  9600; bulk byte-exact; effective rates 0.00 / −0.00 / −0.01 / −0.03 /
  −0.04 %; no overrun or framing error; one-byte latency 5.59 / 2.87 /
  0.80 / 0.77 / 0.75 ms. First step "left 5 stale bytes" (tool artefact,
  see above). Counters afterwards: rx = tx = 84886, irq rx 5428 tx 42138
  err 0 tx_fin 92, anomalies 0.
- `regs`, `pads`: read safely (`CLC_STAT 1c3`, `MODE_CFG 00010016`,
  `FDIV_CFG 3cc10048`, `BC_CFG 2`, `FIFO_CFG 882a`, `DPLUS_STAT 8000ff80`).
  `soc` and `regs-debug` not read.
- Test B: pins 11/12 claimed as `usif uart_rx` / `usif uart_tx`, pin 30
  unchanged; `pads`: io11 level 1 input altsel 1/0, io12 level 1 output
  altsel 1/0 open-drain; passive listen at 9600 for 5 s: 0 bytes. The
  `stty` step could not run (no `stty` in busybox); now `usif-loop -r`.
- Unload, reload, unload: clean; pins, `/sys/kernel/debug/gpio` and
  `/proc/iomem` identical to the baseline; no reboot.

Still open after run 4 (all done in run 5): the rate step of test B with
`usif-loop -r`, a rerun of test A with the fixed tool, and `soc` /
`regs-debug`.

**Run 5, 2026-11-01** (observed on hardware, maintainer-approved; working
tree after `3a119a9` with the TX pad push-pull, ko `3b0f87ec…`, `usif-loop`
`892b4b49…`; the box had not loaded an old build since its last reboot):
tests A and B passed.

- Test A: load as in run 4; `usif-loop` ALL PASS at all six steps with
  **0 stale bytes at every step**; bulk byte-exact, effective rates
  +0.01 / −0.00 / −0.01 / −0.03 / −0.04 / +0.00 %, latency 5.59 / 2.92 /
  0.83 / 0.78 / 0.76 / 5.57 ms; counters rx = tx = 84786, irq rx 5388
  tx 42135 err 0 tx_fin 72, anomalies 0.
- Register files, one per command, all **safe**: `regs` as run 4
  (`FIFO_STAT 8000b000`, `CLC_CNT 101`, `PRTC_CFG 8`, `IMSC`/`MIS` 0);
  `pads` (loopback: io12 still an input); **`soc`** first read: PMU
  PWDCR `00010802`, PWDSR `00010800`, 0x20C `a5c3960f` (bit 30 clear),
  RCU 0x4C `000000d1` (bit 3 clear), CGU PLL0 `00b01f23`;
  **`regs-debug`** first read: PRTC_STAT `00020008`, ICTMO_CFG `28`,
  TXD_SB 0, MRPS_CTRL 0, RIS `000000a0`.
- Test B: pins 11/12 claimed as `usif uart_rx` / `uart_tx`, pin 30
  unchanged; `pads`: io11 `level 1, input, altsel 1/0`, io12 **`level 1,
  output, altsel 1/0, push-pull`**. `usif-loop -r 9600 230400 460800
  230400 9600`: ALL PASS, dividers exactly as expected above, 0 bytes in
  each second; passive listen 5 s: 0 bytes; anomalies 0.
- Unload, reload, unload: clean; pins unclaimed, `/sys/kernel/debug/gpio`
  and `/proc/iomem` identical to the baseline (compared by md5sum; busybox
  has no `diff`).
- After the card, the real upload twice.

## What this test does not cover

- Bytes on the real TX/RX pins (test A loops back inside the USIF, test B
  sends nothing). Those were first exercised by openDECTcell's firmware upload on
  2026-11-01 (9600 and 230400 Bd, both stages first try, 0 overruns; see
  the work log), outside this card; with the push-pull TX pad and
  with `ubihealthd` running plus looping NAND reads in session 8.
- The pad state after unload is set by the driver's "sleep" state (both
  pads inputs) and read back only while the module is loaded; after unload
  it can be checked only electrically.

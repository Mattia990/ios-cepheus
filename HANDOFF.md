# Handoff: iOS on Xiaomi Mi 9 (cepheus)

Last updated **2026-11-09** (newest work first under "Start here"). Read this first, then [NOTES.md](NOTES.md)
(hardware facts, test loop, bootloader rules).

## Start here (next session)

### Touch runs touch-on2..on4: chip answers, mutual sensing fails, water mode silences it (2026-11-09)

The bit-bang transport and the `fts_521` command/event layer work: the chip resets, reports
`ready`, returns sysinfo (`fw=0060 chip0=3639`), ACKs `a0/00/01`, and delivers 8-byte events.
But the event data is invalid, and after ~0.3 s of "MS invalid" the chip enters water mode
and stops reporting touches.

- `touch-on2` (delay 30, warm boot, kernel start 440 s): display on 480.2 s, touch init
  470.2 s (before display-on), sense on 470.25 s. First events 473.8 s: **ten** simultaneous
  `enter` contacts on panel edges. `43 05 30` (MS invalid mutual strength) at 474.08,
  `43 06 01` water enter 474.20, `43 06 00` exit 474.37. No further events.
- `touch-on3` (delay 100): the driver was scheduled at 109.5 s but the XNU log ended at
  103 s; the user's swipes happened before init. Inconclusive.
- `touch-on4` (delay 55, cold boot, kernel start 9.5 s): display on 53.06 s, touch init
  64.55 s (**after** display-on), sense on 64.66 s. Burst at 65.52 s: (58,176), (43,1754),
  (40,1479). Burst at 68.60-68.69 s: `x=1079` at `y=0/170/1754/2182` plus `y~=176` at
  `x=0/20/300/436/575/717` and single points elsewhere. `43 05 30` 68.96, water
  69.07-69.29, then **no events at all** until the log ends at 129 s. Zero FIFO read errors.
  Re-initialising after display-on therefore does not fix it.

The contacts sit on complete panel rows and columns, which is not a hand grip: it is what a
failed mutual-capacitance measurement looks like, and the chip's own status event says the
same (`MS invalid Mutual Strength soft Force cal`). TWRP on the same phone is clean: one
finger at (498,1243), a smooth 213 px swipe, and only the normal
`Mutual negative detect Force cal` after touch-down; no water events.

**Ruled out today**

- Touchmode/grip setup is not the difference: `fts_read_touchmode_data` only reads
  (`0xc1 05` / `0xc1 08`) and `fts_init_touchmode_data` only sets host-side defaults.
- The resume path is exactly `setScanMode(SCAN_MODE_ACTIVE, 0x01)` (`a0/00/01`); the real
  driver sends nothing else at screen-on, only logs `0x43` status events, and does not act
  on water mode either.
- Panel/AFE config: TWRP reads `AFE VER: CFG = 03 - CX = 03 - PANEL = 03`; the chip loads
  this from its own flash, so our reset should expose the same values.
- Power: `ldoc1` (AVDD 1.8 V) and the GPIO-84 `touch_vddio_vreg` (VDDIO) are referenced by
  the FTS node only, not shared with the display. Our order matches `fts_chip_powercycle`:
  reset held low, VDDIO, 4 ms, AVDD, 20 ms, reset high. Pre-init dump shows both rails off,
  so touch-on4 was a real cold power-up of the chip.
- Pin config matches `pmx_ts_active` (reset 12 + IRQ 122: gpio, pull-up, 8 mA).
- FIFO stack read (1 event + `event[7]&0x1f` more, 8 bytes each) matches
  `fts_event_handler`; coordinate decode matches `fts_enter_pointer_event_handler` exactly.

**Next test (no rebuild):** reproduce the edge-contact pattern in TWRP while the working
driver runs. Hold the phone the way it is held during use (fingers wrapping the edges) and
swipe the centre. If TWRP also logs `43 05 30`/water and stops reporting, the trigger is
contact handling; if TWRP stays clean, the chip is in a different state under XNU and the
next step is to log `si[28..31]` AFE/protocol, `si[84..85]` TX/RX plus a FIFO/IRQ heartbeat,
and try a full `fts_chip_powercycle` at init (`cepheustouchpc=1`).

### SE17 FIFO is disabled in hardware; touch moved to bit-banged I2C (2026-11-09)

**Phone run `touch-on1` (kernel built from the GENI stack).** SpringBoard booted with the
touch-enabled kernel; touch stayed dead and the log stopped at SE17's FIFO-disable check:

```
[  74.630] SWEET FTS: se17 fw=0x303 proto=3 status=0 tx=0x20102964 rx=0x20104900 ios=0x3 qup=0x10030000 depth=16/16
[  74.630] SWEET FTS: need ABL-loaded I2C firmware and 32-bit FIFOs; stopping
```

All other SE registers were valid. TWRP's debug log
(`/sys/kernel/debug/ipc_logging/i2c-2/log`) says `i2c in GSI ONLY mode`: on cepheus
`GENI_IF_FIFO_DISABLE_RO` (0x64) reads 0x64 and the QUP runs I2C through GPI DMA, not the
legacy FIFO. Porting GSI rings, DMA descriptors and IOMMU/firmware work to XNU for one slow
bus is not practical, so the FIFO path is abandoned. The GENI driver never reached the
chip: the FIFO check stops before the first transfer.

**Decision: bit-bang the same two pins.** `0085` keeps the FTS power/reset code and
replaces the transport with a software I2C master on GPIO 55/56 (qup17, south TLMM at
0x03d00000); the command/event layer follows Xiaomi's cepheus `fts_521` driver (`0x86`
event read, `0xa0/00/01` sense-on, `0xa6/00/00` system info, event `0x03`
controller-ready), not mainline `stmfts`. Details:

- `cepheustouch=1` starts after `cepheustouchdelay` seconds; `cepheustouchlog=1` logs every event.
- 19.2 MHz `mach_absolute_time` ticks, 4 us setup, 2 us low / 3 us high (~150-200 kHz),
  START/repeated START/STOP, ACK/NACK, SCL clock stretching (10 ms) and 9-clock recovery.
- Probes SDA=55/SCL=56 first, then the swapped pair; a wrong order only NACKs and a reset
  between attempts leaves the chip in a known state.
- Reset GPIO 12 is **active low** (Xiaomi's cepheus driver drives it low for 10 ms, then
  high; the live DT's flag 0 is not a polarity). Reset and IRQ 122 use the DT active state:
  function gpio, pull-up, 8 mA. SDA/SCL use the qup17 sleep state: pull-up, 2 mA.
- VDDIO is GPIO 84 (`touch_vddio_vreg`, active high, 4 ms), AVDD is `ldoc1` at 1.8 V.
- IRQ is polled: read the event stack when GPIO 122 is low, plus once every 10 ms; after
  10 consecutive read errors the bus is recovered and a release is sent for held fingers.

**Patch stack.** `0085` was regenerated with the 774-line `bsd/dev/sweet_fts.c` (blob
`8954729db921f60ff4476884435887bc2a5dad9f`); the obsolete `sweet_fts.c` GCC-halt hunk was
removed from `0086`. Host `clang -fsyntax-only` (kernel stubs, `-Wall -Wextra`) and
`tools/validate-repo.py` pass. The stack has since been built and booted on the phone
(`touch-on2`..`touch-on4` above).

**Boot setup used.** `touch-on4` (the only cold-boot run so far) used the standard debug
arguments plus:

```
cepheuswdt=0 cepheustouch=1 cepheustouchdelay=55 cepheustouchlog=1
```

The transport is confirmed in the log: `SWEET FTS: ready try 1 sda=55 scl=56`, then
`sysinfo fw=0060 chip0=3639`, then `sense on err=0`; events arrive and decode into the
expected `enter`/`motion`/`leave` shapes. If the chip never ACKs on a future change, check
the idle levels printed before START (`idle=1/1`), the SDA/SCL order, and whether GPIO 84
really powers `touch_vddio_vreg`.

**Open.** The transport and the `fts_521` command/event layer work; the remaining fault is
that mutual sensing reports complete rows/columns and the chip then enters water mode (see
the section above for the ruled-out list and next test). After touch works, restore
`wdt_arm(30)` in `shim/shim.c` and drop `cepheuswdt=0`.

### Superseded: GENI I2C driver (2026-11-09)

The earlier 666-line GENI driver (SE17 at 0xc80000, wrapper 0xcc0000) is kept in patch
history only. Its SE register reads were valid, but `touch-on1` shows the FIFO interface
is hardware-disabled; the wrapper/FIFO-depth work is moot for the current transport.

### Hardware read from live phone (DONE 2026-11-05)

Read from the Xiaomi Mi 9 (cepheus) via adb (root). All key hardware facts are now in NOTES.md:

- **Display**: Samsung EA8076 FHD CMD, 1080x2340 @60 Hz, command mode
- **Splash FB**: cont_splash_region 0x9c000000/0x2400000; the VIG0 source register says the panel scans out 0x9c000000 itself (flame's FB is at 0x9c400000 — copying that number shifted the UI down 4 MB)
- **Touch**: ST FTS on I2C 0xc80000, addr 0x49, IRQ 122, x=1080 y=2340
- **Memory**: banks 0x80000000/0x3cc00000, 0xc0000000/0x80000000, 0x1_4000_0000/0xc0000000 (~6 GB)
- **Most SoC addresses identical to flame** (GIC, PMIC, USB, UFS, WDT, MDP, GPU)
- **Keys**: GPIO keys (vol_up code 115, ai_key code 689) — needs GPIO keys driver

### 1. Update remaining TBD values in code

Files still needing cepheus-specific addresses:
- `xnu/patches/0083-cepheus-display-keys-watchdog.patch` — WDT address confirmed (0x17c10000), but
  keys and touch drivers need rework (GPIO keys vs PMIC keys, I2C touch vs SPI touch)
- `xnu/patches/0085-cepheus-fts-touch.patch` — complete rewrite needed for I2C ST FTS at 0xc80000
- `xnu/patches/0088-cepheus-usb.patch` — USB PHY addresses confirmed same as flame
- `shim/sde.c` — panel comment updated (Samsung EA8076), register offsets same as flame

### 2. Build first test kernel and shim

```sh
python3 tools/mkadt.py work/boot/cepheus-hi.adt
tools/build-xnu.sh work/build-0083.log
cp work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet work/boot/kernel-0083
# build shim + payload + boot image
# fastboot boot work/boot/shim-boot-0083.img
```

### 3. Boot and read logs from TWRP

```sh
fastboot boot downloads/twrp/twrp-...-cepheus.img
tools/cepheus-log-pull.sh shim logs/boot-0083.txt
tools/cepheus-log-pull.sh xnu logs/boot-0083-xnu.bin
```

## Where things stand

**Initial skeleton created.** Project structure copied from ios-flame with all flame→cepheus renames.
Hardware facts confirmed from live FDT. Most SoC addresses are identical to flame (same SM8150).
Key remaining work: touch driver (I2C not SPI), keys driver (GPIO not PMIC), and verifying
Xiaomi ABL boot image rules.

## Milestones (to be filled in as work progresses)

Each milestone entry should include:
- Date
- Kernel number and boot args
- Exact results (what worked, what broke)
- What is still open

Format (copy from ios-flame/HANDOFF.md):

```
### Milestone title (YYYY-MM-DD, HH:MM) - kernel NNNN
- **What worked:** ...
- **What broke:** ...
- **Open:** ...
```

# iOS 16 on cepheus (Xiaomi Mi 9, SM8150) - working notes

Port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) (XNU xnu-8796 + iPhone X iOS 16.7.16 userland on
a Xiaomi Mi 9, SM8150), adapted from the ios-flame port (Pixel 4, also SM8150). Read `docs/ios-sweet/NOTES.md`
for the history of ios-sweet's kernel patches (0001-0081); this file records what differs on cepheus.

## Compatibility (from https://hackiosx.dev/compatibility)

Snapdragon 855 (SM8150, the chip in both flame/Pixel 4 and cepheus/Mi 9) is in the "Snapdragon from 2018 on"
category — rated "Good" for porting. Same building blocks as the flame port: same interrupt controller type,
same RPMh power controller, same serial engines for I²C. A port means updating config values (memory map,
storage, screen) and pointing the touch and button drivers at cepheus hardware.

What works now is in [README.md](README.md). [HANDOFF.md](HANDOFF.md) is the engineering log, newest
first: every boot, what broke and why. Paths under `work/`, `logs/`, `backup/`, `dtb/`, `stock/`,
`downloads/`, `images/` and `research/` in either file are local and not in git (see the README).

## Platform tools

adb/fastboot live at `/Users/mattia/Documents/Projects/ios-cepheus/platform-tools/`.
Use the full path: `.../platform-tools/adb`, `.../platform-tools/fastboot`.


## Test loop (no buttons, no screen)

1. Phone in fastboot (`adb reboot bootloader` from TWRP).
2. `fastboot boot shim/out/shim-boot.img` (shim alone = probe; with a payload = XNU).
3. The shim sets the PMIC reboot reason to bootloader, so a reset lands in fastboot.
4. `fastboot boot downloads/twrp/twrp-...-cepheus.img`, then
   `tools/cepheus-log-pull.sh shim logs/X.txt` (and `xnu` for the XNU ring).

## Boot image rules (confirmed from cepheus stock boot.img, 2026-11-05)

Extracted from `/dev/block/sde49` (boot partition) via adb and unpacked with
`tools/unpack_bootv2.py`. Stored in `downloads/cepheus/boot.img`.

| Field | Value | Notes |
|-------|-------|-------|
| Header version | 0 | Legacy Android boot image format |
| Kernel offset | 4096 (0x1000) | File offset where kernel data starts |
| Kernel load addr | 0x8000 | Physical address ABL loads kernel to |
| Compression | UNCOMPRESSED | Kernel starts with "UNCOMPRESSED_IMG" magic (NOT LZ4 like flame) |
| Kernel size | ~46 MB | 48572798 bytes |
| Ramdisk | none | ramdisk_size = 0 |
| DTB | not in boot.img | Separate dtbo partition at /dev/block/sde45 |
| AVB footer | Required | Magic "AVBf" found 64 bytes from end of partition |
| Partition size | 128 MB | 0x8000000 |
| Page size | 4096 | |
| Tags addr | 0x100 | Boot parameters location |
| Cmdline | `console=ttyMSM0,115200n8 earlycon=msm_geni_serial,0xa90000 ...` | From stock boot.img |

### Key differences from flame (Pixel 4)

| Item | Flame (Google ABL) | Cepheus (Xiaomi ABL) |
|------|-------------------|---------------------|
| Header version | v2 | v0 |
| Compression | LZ4 | UNCOMPRESSED |
| Kernel offset | 0x80000 | 0x1000 |
| Kernel load addr | 0x80080000 | 0x8000 |
| AVB footer | Required (64 MiB padded) | Present (64 bytes from partition end) |
| Ramdisk | In boot image | None in stock |
| DTB | In boot image | Separate dtbo partition |

### Build implications

- `mkbootimg` with header version 0, kernel offset 4096
- Kernel must be uncompressed (no LZ4)
- AVB footer must be appended (likely `--algorithm NONE` like flame, or Xiaomi may require signing)
- Boot image padded to partition size or appropriate size
- DTB loaded separately (dtbo partition) or embedded in kernel

## Hardware facts (confirmed from cepheus live FDT, 2026-11-05)

Read from the Xiaomi Mi 9 (cepheus) running Android 11 (RKQ1.200826.002):

```sh
PLAT=/Users/mattia/Documents/Projects/ios-cepheus/platform-tools
$PLAT/adb root
$PLAT/adb shell cat /proc/device-tree/memory/reg        # RAM banks
$PLAT/adb shell cat /proc/device-tree/reserved-memory/cont_splash_region/reg  # splash FB
$PLAT/adb shell dmesg | grep -i -E "panel|touch|watchdog|mdss|ufshc|dwc3|wdt"
```

| | cepheus (confirmed) | (flame reference) |
|---|---|---|
| SoC | Qualcomm SM8150 / Snapdragon 855 | same |
| CPU | 4x Cortex-A55 + 4x Cortex-A76 (8 cores) | 4x A55 + 3x A76 + 1x A76 prime |
| RAM | ~6 GB: banks 0x80000000(0x3cc00000) + 0xc0000000(0x80000000) + 0x1_4000_0000(0xc0000000) | <0x80000000 0x3cc00000>, <0xc0000000 0x140000000> |
| GIC | GICv3, GICD 0x17a00000, GICR 0x17a60000 | same |
| Timer | 19.2 MHz, vtimer INTID 19 | same |
| PSCI | 1.1 smc | same |
| UFS | ufshc@0x1d84000 | same |
| Display | Samsung EA8076 FHD CMD, 1080x2340 @60 Hz | NT37280 CMD 1080x2280 |
| Splash FB | cont_splash_region: 0x9c000000, size 0x2400000; FB at 0x9c000000 | 0x9c400000 (4 MB into the block) |
| MDP | 0x0ae00000 | same |
| Touch | ST FTS, I2C bus 0xc80000, I2C addr 0x49, x=1080 y=2340, IRQ=122 | ST FTS, SPI 0x890000, addr 0x5d |
| Keys | GPIO keys: vol_up (code 115), ai_key (code 689) | PMIC PON keys + GPIO6 |
| PMIC arb | spmi@0xc440000 | same |
| Watchdog | wdt@0x17c10000 | same |
| USB | DWC3 @0xa600000, PHY @0x88e2000 | same |
| GPU | Adreno 640, kgsl@0x2C00000, gmu@0x2C6A000, smmu@0x2CA0000 | same |

Boot: Android 11 RKQ1.200826.002, verified boot state orange (unlocked).

**Touch bus quirk (2026-11-09).** SE17's FIFO interface is disabled in hardware:
`GENI_IF_FIFO_DISABLE_RO` (0x64) reads 0x64 and TWRP's i2c-geni log says
`i2c in GSI ONLY mode`. Android drives the bus through GPI DMA, so XNU bit-bangs
GPIO 55/56 (qup17) as a software I2C master instead.

## Porting methodology (from https://hackiosx.dev/how-it-works and /porting)

The four problems every port solves: starting Apple's kernel, driving the hardware,
getting a picture on the screen, keeping iOS happy without the parts it expects.

### What changes vs. what carries over

Of the ~81 original ios-sweet kernel patches:
- **~66** don't care what phone they run on (generic XNU build, filesystems,
  security stand-ins, workarounds) — copy unchanged
- **~10** only need new addresses (memory map, display, UFS, USB, PMIC) — adapt
- **~5** are Qualcomm hardware drivers (keys, touch, GPU) — may need rework

### Porting steps, in order (complete each before moving on)

1. **Get your own code running** — bootloader accepts the image, watchdog stopped,
   compression/signature right. Verify: shim output visible (color on screen, reboot).
2. **A picture and a log** — draw into the bootloader's framebuffer, get kernel log off
   the phone (reserved RAM ring, storage tail, USB network, or QR codes).
3. **Describe hardware to XNU** — Apple Device Tree: memory range (avoid reserved),
   GICv3 addresses, timer frequency + interrupt, CPU core regs, UFS controller base.
   Verify: kernel logs scroll, stops at "waiting for root device".
4. **CPU cores** — start with one core, 16 KB pages (some small cores can't use them),
   start others via PSCI.
5. **Storage and iOS system** — UFS driver reads the HFS+ root in userdata,
   launchd starts, Apple logo appears.
6. **Lock screen** — device identity (iPhone10,3/D22AP), screen size fitted to panel,
   disable crashing daemons (modem, Bluetooth).
7. **Touch and buttons** — touch chip power rail (RPMh on Qualcomm), bus, irq/reset pins,
   edge touches for swipe-up-home; buttons become Apple-style key events.
8. **Everything else** — GPU, Wi-Fi, audio, battery, camera, etc.

### Common failure modes

| Symptom | Likely cause |
|---------|-------------|
| Bootloader refuses image | Wrong boot image version, compression or signature |
| Nothing changes on screen | Wrong framebuffer address, or command-mode panel needs refresh |
| Phone reboots within seconds | Watchdog still running, or memory overlaps reserved region |
| Kernel text appears, then stops | GIC or timer values wrong |
| Hangs when more cores start | 16 KB pages unsupported, or GIC not prepared for cores |
| Stuck waiting for root device | Storage driver can't reach controller |
| Lock screen, touch does nothing | Touch chip power rail, pins, or protocol wrong |

## Layout

- `shim/` cepheus shim (`build.sh`, Homebrew llvm + lld), `tools/` host tools, `phone/` code that runs in
  iOS on the phone (each directory has a `build.sh`; they use `work/userland/bin/ios-cc`)
- `xnu/patches/` ios-sweet 0001-0081 + cepheus patches from 0082
- `work/dxb/` blacktop darwin-xnu-build checkout; `work/dxb/xnu` is a git repo whose HEAD is
  "baseline + applied patches", new patches come from `git diff`
- Not for git: `backup/` (partition dumps, serials), `dtb/`, `stock/`, `downloads/` (TWRP,
  IPSW parts, AOSP tools), `work/`, `logs/`

## Cepheus patches

- 0082 IORPC.kernelContent: Xcode 16+ iig emits it (portable, no change)
- 0083 cepheus basics: early console on splash FB (0x9c000000, 1080x2340), watchdog (0x17c10000),
  keys (GPIO 6=vol_up, GPIO 97=ai_key - disabled by default via sweetkeys=0),
  touch (I2C 0xc80000 addr 0x49 - disabled by default via cepheustouch=0), display keys
- 0084 early FB device memory: same as flame (confirmed FB block 0x9c000000)
- 0085 FTS touch: bit-banged I2C on GPIO 55/56 (SE17 FIFO is hardware-disabled; GSI-only
  mode), ST FTS addr 0x49, cepheus `fts_521` protocol (`0x86` events, `0xa0/00/01`
  sense-on, `0xa6/00/00` system info), reset 12 active low, IRQ 122, AVDD ldoc1 at 1.8 V,
  panel 1080x2340; disabled by default via cepheustouch=0
- 0086 cpufreq: same GCC offsets as flame (SM8150)
- 0087 vsync/fps: same as flame
- 0088 USB: same DWC3 @0xa600000, PHY @0x88e2000 as flame
- 0089-0124: same as flame (same SoC subsystems)
- 0125 FTS touch: reset the chip again after the `0xa6` sysinfo read (Xiaomi's probe
  resets twice before `a0/00/01`), log AFE/config/TX/RX, and add
  `cepheustouchreset2=N` (default 1), `cepheustouchsysinfo=0`,
  `cepheustouchpoll=0` plus a 10 s alive heartbeat. Verified on hardware
  (`touch-on6`): AFE 03/03/03, one-finger tracking, no `43 05 30`/water
- 0126 FTS touch: enable by default (`cepheustouch` defaults to 1, delay stays 20 s);
  pass `cepheustouch=0` to disable
- 0127 backlight: publish a fake `AppleARMBacklight` (`IOClass=AppleARMBacklight`,
  `backlight-control=true`, iPhone X DBV/AAP properties) as a child of `AppleCLCD` and log
  `setProperties` writes (`IOMFBBrightnessLevel` / `brightness-nits`); phase 1 only logs,
  panel brightness is unchanged
- 0128 backlight: also override `IOMobileFramebuffer::setProperties` (the real
  `IOMFBBrightnessLevel`/`brightness-nits` write target, i.e. `AppleCLCD`) and log the raw
  inputs of IOMFB selectors 50/51/84; still logs only, panel brightness unchanged
- ... (one line per patch, filled in as they are created)

## Step 1 resolved (2026-11-05)

Keys and touch TBD items from the initial port plan are now resolved:

**Keys** (patch 0083):
- Confirmed from live FDT: GPIO keys node with vol_up (GPIO 6, linux code 115) and ai_key (GPIO 97, linux code 689)
- Power/vol-down may still be on PMIC PON (not confirmed yet)
- Keys disabled by default via `sweetkeys=0` boot arg (added to sweet_keys_init)
- Needs a GPIO keys driver before enabling

**Touch** (patch 0083 + 0085):
- Confirmed from live FDT: ST FTS on I2C bus 0xc80000, I2C addr 0x49, IRQ 122, reset fts_rst
- Panel resolution 1080x2340 matches fts,x-max=1080, fts,y-max=2340
- Touch disabled by default via `cepheustouch=0` (patch 0085)
- Patch 0085 now bit-bangs SDA/SCL as GPIO (2026-11-09): SE17's FIFO is hardware-disabled,
  so the GENI driver was replaced; enable with `cepheustouch=1`. The command/event layer
  follows Xiaomi's `fts_521` driver, not mainline `stmfts`.

**Remaining TBD items:**
- GPIO register addresses for reading keys (need TLMM/GPIO controller layout)
- Xiaomi ABL boot image rules (compression, text_offset, AVB footer)

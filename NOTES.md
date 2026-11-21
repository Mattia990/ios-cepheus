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
- 0129 backlight: gate the fake `AppleARMBacklight` with `sweetbl` (`0` never, `1` at
  `AppleCLCD::start`, `2` on first display-on) and add `sweetfb_sel_log=1` to log every IOMFB
  `externalMethod` call with its first scalar inputs; logging only, panel unchanged
- 0130 backlight: `sweetbl=3` publishes a non-conforming `SweetBacklight` service
  (`backlight-control=true`, but not `AppleARMBacklight`) so CoreBrightness takes the non-ARM
  container path and keeps the registry brightness write; logging only, panel unchanged
- 0131 backlight: log `IOMobileFramebufferUserClient::registerNotificationPort` (type, port,
  refCon) and the first dropped `notify` calls per type, plus the client count in the 5 s
  stats; instrumentation only, panel unchanged
- 0132 backlight: complete MIG async calls from `IOMobileFramebufferUserClient::externalMethod`
  (`sendAsyncResult64` on `args->asyncReference`); `IOUserClient::callExternalMethod` calls the
  override directly, so XNU never sent the completion and CoreBrightness/backboardd waited on
  its own 5 s timeout. Logs the first async call/reply per selector, `sweetfb_async_log=1` logs
  all of them; no reply is sent when `asyncWakePort` is null
- 0133 backlight: log selector 55 (`_IOMobileFramebufferSetMatrix`, scalar + 72-byte 3x3
  matrix) as nine u64 words with `sweetfb_sel_log=1`; logging only, panel unchanged. Not the
  brightness call: selector 50 is `SetBrightnessCorrection`, and the real level reaches the
  kernel through `IOMobileFramebufferSwapSetBrightness` → `BCBrtControlSetNits` and the
  `SwapEnd` record (flag +0x2e7, nits double +0x2ec)
- 0134 backlight: with `sweetfb_sel_log=1`, dump the brightness fields of every `SwapEnd`
  record in `AppleCLCD::swap`: presence bytes +0x2e2 and +0x2e7..+0x2eb, doubles +0x2ec
  (nits, also printed as milli-nits), +0x2f4, +0x2fc, +0x304, +0x363, and floats +0x37f /
  +0x387; logging only, panel unchanged
- 0135 backlight: publish the four `IODisplayParameters` sub-dictionaries (`brightness`
  0..0x10000 value 0x8000, `rawBrightness` 0..0x7ff, `BrightnessMicroAmps` 0..25000,
  `BrightnessMilliNits` 0..600000 with `uncalMilliNits`) and
  `force-brightness-control=true` on the fake `AppleARMBacklight`, matching the value set
  the iPhone X AppleARMBacklight driver builds; `BCAppleBacklightBrtControl
  initWithService:` returns nil without `IODisplayParameters`, so the IOMFB match never
  sets the brightness flag (`+0xb39`) and `SwapSetBrightness` stays inert. ARM mode only
  (`sweetbl=1`); `sweetbl=3` is unchanged, panel brightness still untouched
- 0136 backlight: the `brightness` / `backlight-level` writes that reach the fake
  `AppleARMBacklight` now drive the panel. Map the 16.16 value (0..0x10000) to the EA8076
  DCS level (0, or 2..0x7ff from DTB `bl-min`/`bl-max`) and send DCS `0x51` as a MIPI long
  write (`03 00 39 c0 51 msb lsb ff`, the panel's `qcom,mdss-dsi-bl-dcs-type-ss` byte
  order) through the DSI0 command FIFO at `0xae94000`; no IOVA/SMMU setup, no other DSI
  state changed. Reads `DSI_HW_VERSION`/`DSI_CTRL`/`DSI_STATUS`/`DSI_FIFO_STATUS` on first
  use, logs the first eight sends and defers/reuses the FIFO until `DSI_CMD_MODE_DMA_DONE`.
  `sweetbl_dcs=1` by default, `sweetbl_dcs=0` disables the DCS writes; ARM mode only
  (`sweetbl=3` is unchanged)
- 0137 touch: power-cycle both dedicated touch rails before the first reset (VDDIO
  `touch_vddio_vreg` on GPIO 84, AVDD PM8150L L1/ldoc1 at 1.8 V) so a TWRP ->
  fastboot warm boot starts from the same analog state as a cold boot. Also recover
  from an early `43 05 30/31` force-cal failure or water-mode enter within 5 s of
  sense-on by repeating the power cycle and sense-on (at most twice).
  `cepheustouchpwr=0` and `cepheustouchrecover=0` disable the two parts
- 0138 touch: the 0137 warm test still failed, so the rail-off is now diagnosable:
  `cepheustouchpwrms=N` (default 200) busy-waits with reset asserted, then releases reset
  at 2 mA and reads the 0xa6 sysinfo frame (`controller unpowered` proves a real power
  loss; `alive` means the rail did not drop). Power-up order restored to VDDIO then AVDD.
- 0139 touch: replay TWRP fts_521's remaining probe steps after the sysinfo read: read the
  lockdown record (`0xa6/00/00` header, `0xa4/06/70` request, echo poll, 28-byte read with
  sequence-counter check) and read the CRC status register (`0xfb` + `0x20000078`, mask
  `0x03`), then system-reset and scan 500 ms for config/Cx checksum-error events. No dummy
  byte (fts_521's I2C branch sets the `DUMMY_*` flags to 0). `cepheustouchprobe=0` skips both.
- 0140 touch: at init and after a recovery power cycle, send sense-on (`a0 00 01`),
  sense-off (`a0 00 00`), wait 50 ms (`WAIT_AFTER_SENSEOFF`) and sense-on again, matching
  TWRP's `fts_init_sensing` FOD branch; the first calibration is aborted before it
  completes. `cepheustouchsensecycle=0` keeps the single sense-on.
- 0141 touch: `SweetFramebuffer.cpp` increments `sweet_fb_display_on_count` in
  `kSweetFBRequestPowerChange` when the display turns on; the FTS thread polls it and,
  on a change, resets the controller, waits for ready and sends sense-on (`a0 00 01`, no
  rail cycle), matching TWRP's `fts_drm_state_chg_callback` after `DSI_CMD_SET_ON`.
  `cepheustouchdisplay=0` disables it.
- 0142 touch: `fts_sense_cycle()` runs the first scan for `cepheustouchsensems` ms
  (default 350), senses off, waits 50 ms, then resets the controller, waits for ready and
  senses on, matching the point where TWRP cuts its first scan and system-resets FTS
  before the second calibration. `cepheustouchcyclereset=0` skips the reset (control);
  the sense-on line reports `ms=N cr=1`.
- 0143 touch: replay the live DTB's 34-command EA8076 SET_ON sequence through the DSI0
  command FIFO at display-on (and at FTS start when the display is already up), before
  the 0141 reset/sense-on. This is the panel re-init the warm path lacks: the shim cannot
  replay it because the DSI link is off at shim entry, while XNU's FIFO is live. Skips
  the sequence's `51 00 00`, waits 200 ms after display-on so CoreBrightness's first
  `0x51` is not overlapped, and 50 ms after `0x29` like TWRP. `cepheustouchpanel=0`
  disables the replay; `cepheustouchpanelhs=1` sends it in HS instead of LP.
- 0144 touch: the boot rail cycle also sends TWRP's `DSI_CMD_SET_OFF` (`28 00`, then
  `10 00` with the DTB's 120 ms sleep), drops panel VCI (east TLMM GPIO 21) and reset
  (south TLMM GPIO 6) with the DTB's pre-off sleep, and re-applies the rail order
  VDDIO -> AVDD -> 20 ms -> VCI -> 10 ms -> reset -> 10 ms, so the DDIC gets a real
  POR. The SET_ON replay then runs at boot, first display-on and recovery, sets
  `panel_ready`, and is skipped on power-key wakes; `SweetFramebuffer.cpp` exports
  `sweet_bl_reassert()` to resend the last DCS `0x51` level and clear the stale
  FIFO-pending flag after the replay. `cepheustouchpanelpor=0` keeps VCI/reset powered.
- 0145 touch: `cepheustouchvddio=0` leaves north TLMM GPIO 84 (shared VDDIO) untouched
  and cycles only the touch AVDD (PM8150L L1) plus reset. The earlier "ghost follows
  VDDIO" reading was wrong: `touch-0145-noreplay` used `vddio=0`, so GPIO 84 stayed low
  from boot and the chip was never powered (no ghost, but zero touches). With `vddio=1`
  the warm ghost/`43 05 30` returns with or without the rail cycle (0146 runs).
- 0146 touch: `cepheustouchvideo=1` (default 0) stops the DSI video engine around the
  SET_ON replay (`DSI_CTRL` 0xae94004 saved, `ctrl & ~7u`, 20 ms, replay, 50 ms, restore,
  20 ms, then the brightness reassert). Warm test showed `ctrl & ~7u` also disables the
  command DMA: all 33 replay commands fail and the engine never resumes (frozen logo, 0
  fps), so the option is diagnostic only. With `cepheustouchpanel=0` (no replay) the
  display, brightness and wake survive a full FTS rail cycle, but the FTS still fails its
  auto-calibration ~3 s after sense-on and reports `43 05 30`. A minimal init (single
  reset, sysinfo, one `a0 00 01`, every optional step off) reproduces the same ghost, so
  the warm failure is in the chip's first calibration, not in the probe or rail cycle.
- 2026-11-17 hardware re-check (TWRP source + live DT): panel supplies are `vddio`
  (PM8150 L14) and `vcie` (`disp_vci_vreg`, east TLMM GPIO 21); the DT's `vci-supply`
  (PM8150L L13) is not used by the ea8076 panel entries. `touch_vddio_vreg` (north
  GPIO 84) is the FTS VDDIO load switch. TWRP power-on is vddio -> 10 ms -> vcie ->
  reset low 1 ms/high 10 ms; power-off is the exact reverse. PM8150L L1 SPMI readback
  proves the RPMh vote enables AVDD (`46=0 41=7 45=2` -> `46=80 41=7 45=2`).
- 2026-11-17 TWRP FTS probe/blank sequence (source + dmesg): probe reset, sense-on,
  sense-off, 50 ms `WAIT_AFTER_SENSEOFF`, sense-on; DRM blank senses off; panel SET_ON
  system-resets and senses on again. Only `43 05 01`, never `43 05 30`. TWRP's AFE
  readback (CFG/CX/PANEL `03/03/03`, config `0037`, fw `0060`) matches the shim exactly,
  so config/firmware mismatch is ruled out.
- 2026-11-17 shim panel-POR controls (all warm, logs under `logs/runs/`): a true
  L14+VCIE POR with full 34-command SET_ON before a quiet drain still ghosts
  (`fts-panelpor`); continuous polling is stochastic, the clean `fts-panelpor-poll` run
  did not repeat (`fts-panelpor-poll2` ghosted twice); 25 s panel-on age + fast polling
  ghosts (`fts-age25`); 6 ms per logged event ghosts (`fts-evdelay6`); and a **second
  full POR immediately before FTS power-up** still ghosts (`fts-age25-repor`). Every POR
  reports 34 cmds/0 errors/pos 458/458, backlight `0x7ff`, TE 6 edges. So a panel POR
  does not clear the warm ghost, panel freshness at sense-on is not the differentiator,
  and the failure is stochastic.
- 2026-11-17 shim flags: `SHIM_FTS_PREDELAY_MS` (panel-on age), `SHIM_FTS_EVDELAY_MS`
  (per-event delay), `SHIM_FTS_REPOR` (second POR after the age delay),
  `SHIM_FTS_SOFT_RECOVER` (after the first `43 05 30`, wait 600 ms, sense-off/50 ms/
  sense-on), `SHIM_FTS_BLDIM_MS` + `SHIM_FTS_BLLEVEL` (DCS `0x51` level around the scan),
  `SHIM_FTS_PREPROBE` (I2C address probe before the rails; expect a NACK if ABL really
  left the chip unpowered). `ft_power_off()` now logs PM8150L L1 after the off vote.
- 2026-11-17 DSI-off and APID map: `SHIM_FTS_DSIOFF` runs `dsi_phy_hw_v4_0_disable()` after
  the panel POR and leaves the PHY off during the scan; the ghost is unchanged
  (`fts-dsioff`, `3db96611...`), so DSI lane activity is not the trigger.
  `spmi_dump_map()` prints every mapped APID with owner EE; warm has 345 entries
  (owners 0:99, 1:14, 2:12, 4:220) and PM8150B SID 3 exposes only APIDs 304-308, so SID 3
  LDO reads say `no apid` even though the charger blocks exist. `log_ldo_bank()` dumps the
  full SID 3/5 LDO bank at entry; `ft_l1_log()` now prints the full PM8150L L1 register
  set at entry/powered/off. A warm reset with the cable unplugged (no VBUS at SBL1) still
  ghosts (`fts-pmic2-warm-novbus`, 3965/4122 ms), so USB/charger detection is not it.
- 2026-11-17 soft re-arm works: with `SHIM_FTS_SOFT_RECOVER` the chip re-armed after the
  force-cal (`43 05 30` -> water mode -> sense off/on -> `43 05 01`) and stayed sensing
  for 7.6 s with no second burst (`fts-softrec`, `fts-softrec12`). A panel/rail cycle is
  not needed. `FT_POLL_MS` is now overridable at compile time for longer windows.
- 2026-11-18 shim warm-order flags: `SHIM_FTS_RESET2=N` (a second hardware reset straight
  after the `0xa6` sysinfo read, XNU 0125's known-good order; `fts-reset2` still ghosts),
  `SHIM_FTS_BLANKCMD` (TWRP's DRM-blank commands `A2 03 20 00 00 01`, `A0 01` low-power
  scan mode, `C0 02 00 00` before the final reset, then `SHIM_FTS_BLANKCMD_MS` = 250),
  `SHIM_FTS_RETRY_MS` (second full power-cycle bring-up + quiet scan in the same boot),
  `SHIM_FTS_DUMMYSCAN_MS/_OFF_MS` (one throwaway `a0 00 01` for N ms, then rails off for
  `_OFF_MS` = 300 and a fresh power-up before the real init). N>=2000 removes the ghost
  (2000/2500/3000 quiet; 1500 leaves one `43 05 30`; 500 ghosts), but the chip then
  **reports no real touch at all** (`fts-dummy3-touch`, `fts-latetouch`, `fts-latetouch2`:
  a 60 s human swipe gives 0 frames and 1 IRQ). Same image on a cold boot reports touch
  fine, so this is a silent warm chip, not a fix. `SHIM_FTS_LATE_TOUCH_MS=N` is the
  visible post-display touch window used for those tests. `tools/cepheus-shim-run.sh NAME
  IMAGE` runs the whole warm loop and prints the event summary. `fts-quiet-dwell90` keeps
  the rails off for 90 s before power-up and still ghosts, so warm residue on the touch
  rails is not a time-decay effect.
- 0147 touch: re-arm the scan instead of power-cycling when the controller reports
  `43 05 30/31` or water-mode enter within 10 s of sense-on (at most 4 times), waiting
  `cepheustouchrearmms` (default 600) first. `cepheustouchrearm=0` disables it; the 0137
  power-cycle recovery remains the fallback. Each re-arm is plain sense-off (`A0 00 00`)
  immediately followed by sense-on. Warm XNU test (`touch-0147-noreplay`):
  the re-arm returns the chip to `43 05 01` and suppresses the second force cal, but the
  AFE reports no contacts at all, so `43 05 01` is a normal post-sense-on status, not
  proof that the chip is scanning for fingers.
- 0148 wifi: give the status bar a real signal level. The Apple80211 link event now
  fills `snr`/`noise` from the joined BSS RSSI (`rssi + 95` and a `-95` dBm noise floor,
  the same assumption as `phone/wifi/radio.c`), and `GET NOISE` (type 17) answers with
  the same 52-byte `apple80211_noise_data` layout as `GET RSSI` instead of
  `EOPNOTSUPP`. `wifid` asks for both on link-up; with `noise=0` its SNR was negative
  and SpringBoard drew one arc. `sta_state()` also refreshes the joined BSS RSSI from
  `cepheus_wlan_link_bss()` now, so `GET RSSI` and later link events report the signal
  as heard rather than the join-time copy. Patch written; not yet built or tested.
- 0149 battery: cepheus PM8150B FG-gen4 backend. `SweetBattery` probes SPMI USID 2
  (FG subtypes 0x10/0x11) and, when found, publishes the fuel gauge's own monotonic
  SOC (0x4009/copy 0x400a, 0/255 scaled to 0/100), VBATT (0x41a0, LE, x122070/1000 uV)
  and IBATT (0x41a2, LE signed, x488281/1000 uA, sign flipped for IOPMPowerSource),
  plus SMB5 charger state (0x1006), power path (0x110b), USB plugin (0x1310) and the
  error/JEITA gates (0x1007/0x100d). All reads stay on the existing observer; the SPMI
  filter now accepts USID 2 peripherals 0x10-0x13 and 0x40/0x41. Read-only, no PMIC
  writes; `cepheusbattery=0` disables it and keeps the static fallback. `sweetbattery=1`
  still selects the PM6150 path on sweet. The cepheus design capacity is 3300 mAh
  (`batterydata-F1-*-3300mah`), not sweet's 5020. Temperature is read from `BATT_INFO`
  0x4150/0x4151 and decoded as either the gen3 0.25 K/LSB or the gen4 0.25 C/LSB
  encoding, whichever lands in -20..80 C; otherwise 25.0 C stays marked estimated.
  Raw values are published as `SweetBatteryRaw{MSOC,VBATT,IBATT,Temp}`,
  `SweetBatterySocReady` and `SweetBatteryChargerState`. Patch written; not yet
  built or tested.
- 0151 cpumask: SWEET's `ml_parse_cpu_topology` clamp no longer forces one CPU when
  only `cpumask=` is present (it still does with no boot arg, and `cpus=` and
  `cpumask=` cannot be combined). Makes `cpumask=0xF1` usable: boot A55 plus all four
  A76/prime, no `cpus=8`. Diagnostic only; patch written, not yet built or tested.
- 0152 sampler: every `sweetsample` `SMP` line gains `cpu`, `pset` and `ptype` from
  `thread->last_processor` and its processor set, so a placement run can show where
  each sampled thread runs. `sweetsamplestart`, `sweetsampleperiod` and
  `sweetsamplecount` (default 90/120/40) tune the schedule for a short test.
  Patch written; not yet built or tested.
- 0153 AMP: `__ARM_AMP__=1` and `MAX_CPU_CLUSTERS=2` for SWEET, plus an early panic
  when the ADT has no E or no P cluster. Needs `tools/mkadt.py --amp` (cluster-type
  `'E'` on CPUs 0-3, `'P'` on 4-7). Moved into `xnu/patches/` after the cpumask run
  (2026-11-21); `work/boot/cepheus-amp.adt` is generated. Patch not yet built or
  booted.
- 2026-11-17 status decode: `43 05 01` = `EVT_TYPE_STATUS_FORCE_CAL` subtype `0x01`
  ("Mutual negative detect Force cal"), emitted right after every sense-on; TWRP reports
  the same and works. The old note about a distinct TWRP `43 05 02` was a misread: the
  last byte is `event[7]`, the FIFO counter. `43 05 30` remains "MS invalid Mutual
  Strength soft Force cal".
- 2026-11-17 panel emission is not the trigger: `fts-bldim60` (DCS `0x51 = 0` before the
  FTS rails and through sense-on + 15 s) still produced the same right/top-edge burst at
  3.26-3.47 s and no touch after brightness restore. `fts-quiet-paneloff30` (panel VCI +
  reset low for 30 s, scan with the panel off) also ghosts, so DDIC drive/emission is out.
- 2026-11-17 LineageOS check: `drivers/input/touchscreen/xiaomi/fts_521` is the same ST
  5.2.4 as TWRP (whitespace + Android plumbing only), but cepheus' vendor config selects
  `CONFIG_TOUCHSCREEN_ST_FTS_V521=y`, i.e. `drivers/input/touchscreen/st/fts.c` 4.2.14
  with DT `fts@49 compatible = "st,fts"`. Its probe path has no warm-boot-specific init.
  4.2.14 does define `USE_NOISE_PARAM` and has a chip-info/full-init (`0xA5`) path, but
  firmware 0060 implements none of the commands they need: `C8 01`, `C7 01`, `B8 FF FF`
  all time out (2 s, scan on/off) and `D0 8000` reads zeros (2026-11-18). Nothing to port.
  The old claim "5.2.4 has no noise-parameter write" was correct but incomplete.
- TWRP `/proc/fts/driver_test` (fts_proc.c): text mode is space-separated hex bytes.
  `01 A0 00 01` = raw write (sense on), `12` = system reset, `14` = `fts_chip_powercycle`,
  `15 00` = read sysinfo, `17 01` = enable the AP IRQ (`14` masks it and never restores it;
  TWRP's ESD path enables it after sense-on), `23 <type>` = get MS frame (0 raw, 1 filter,
  2 strength, 3 baseline; `GETMSFRAME` senses on, waits, senses *off*, flushes the FIFO and
  reads the frame, so send `01 A0 00 01` afterwards).
  `tools/cepheus-fts-driver-test.sh baseline|cycle OUTDIR` runs these from TWRP over adb
  and saves dmesg, frames and an 8 s `/dev/input/event4` capture. Result: the driver's own
  power cycle (20 ms), system reset and sense-on produced `43 05 01` with no `43 05 30`
  and real touch afterwards, and the frames read normally. `adb wait-for-device` hangs on
  TWRP's `recovery` transport; use `adb get-state`.
- 2026-11-17 shim flag: `SHIM_FTS_TWRPSEQ_PANEL` (with `SHIM_FTS_TWRPSEQ` and
  `SHIM_PANEL_INIT/SHIM_PANEL_CYCLE`) inserts TWRP's DRM blank/unblank between the
  twrpseq off2 and the controller reset: 250 ms, then `panel_reinit()` (VCI/L14 drop,
  SET_ON), then reset and sense-on. Every earlier shim test did the panel power change
  before the FTS bring-up instead. Result (`fts-midpanel`): still ghosts, 20 contacts at
  3.70 s, `43 05 30` at 4.07 s, water mode 4.18/4.44 s.
- 2026-11-17 pre-probe (`SHIM_FTS_PREPROBE`, `fts-preprobe`): before VDDIO/AVDD are
  enabled the I2C bus cannot even be raised (`recover=0 ack=-1 sda=0 scl=0`), and entry
  GPIO 84 is `io=0` with PM8150L L1 `46=0`; after `ft_power_off()` L1 reads `46=0` again,
  so the RPMh off vote lands. The chip is unpowered at a warm entry and the same run still
  ghosts (44 events, `43 05 30` at 4.90 s), so nothing is retained in the controller.

- 2026-11-18 PMIC GPIOs (new dump, `log_pmic_gpios()`): the blocks at SID 0/2/4
  `0xc000+0x100*(n-1)` are the last retained state never compared cold vs warm. The live
  DT's only always-on GPIO output is `vdd_boost_vreg` on PM8150B GPIO5 (`0xc400`,
  `regulator-always-on`, no consumer); `pm8150b_gpios 12` (`ext_5v_boost`) is disabled,
  the rest are inputs. `SHIM_FTS_GPIO5OFF` releases GPIO5 (mode `0x40=0`, enable `0x46`
  bit 7 cleared, i.e. digital input and master enable off) before the FTS rails;
  `fts-gpio5.img` (`2300496f...`) also dumps the GPIO blocks at entry. Cold run of
  `a856c75a...`: the entry diff vs warm is only four FTS-SMPS set points (`1:2f00`,
  `1:3000`, `5:1d00`, `5:1e00`; raw 0x40), all DVFS leftovers. GPIO register map:
  `0x04` type (0x10), `0x05` subtype, `0x10` RT input, `0x40` mode, `0x41` vin,
  `0x42` pull, `0x43` dig-in, `0x45` out ctl, `0x46` enable (bit 7 master).
- 2026-11-18 result: PM8150B GPIO5 entry `5=11 10=0 40=0 41=0 42=4 43=0 45=1 46=80`
  (mode already input, master enable set); clearing enable still ghosts 27 events with
  `43 05 31` + two `43 05 30` at 5213/5451 ms (`fts-gpio5-warm`). Not the trigger.
- 2026-11-18 panel-drain control: VCI off + DDIC reset low + RPMh L14 off for 30 s before
  FTS power-up still ghosts (events at 877-897 and 3755-4002 ms, two `43 05 30` at
  4272/4423 ms; `fts-drain-warm`). DDIC/POR residue is not the trigger.
- 2026-11-18 `SHIM_FTS_LATE_RESET_MS=N` resets while the chip is scanning (TWRP's
  fw-update `fts_system_reset` 1 s after sense-on); at N=1000 the warm chip emits only
  `43 05 01` + the sense-on echo and 2 IRQs in 60 s, i.e. silent exactly like the dummy
  scan (`fts-rescan-warm`, first session). A reset before the burst is not a fix.
- 2026-11-18 TWRP control re-read: the warm TWRP boot that produced
  `twrp-now-touch2` also has `43 05 30` at 5.108 s (after a 4.95/5.02 s press/release
  pair) and still reports real touch at 148-184 s. `43 05 30` therefore does not by itself
  mean the chip is dead; the shim's chip goes silent after it. The old "TWRP never emits
  `43 05 30`" note was a grep artifact (`30 00` in dmesg vs `43 05 30` in shim logs).
- 2026-11-18 TLMM tile map (from `work/fts/pinctrl-sm8150.c` `PINGROUP` entries; bases
  west 0x03100000, east 0x03500000, north 0x03900000, south 0x03d00000): touch pins
  55/56/122 south, 12/84 north, 21 east; fingerprint `goodix` reset 37 north, IRQ 120
  south, `fp-gpio-pwr` 125 west; IDT wireless-charger interrupt 90 / enable 104 north;
  NFC 41/42 north, 47 east, 113 west. A full 176-pad entry dump is the next warm-vs-cold
  comparison.
- 2026-11-18 TLMM pads **0-3 and 126-129 have no usable GPIO register**: reading pad 0 at
  `0x03d00000` stalls the shim (save at 7706 ms, phone resets ~10 s later, no exception).
  TWRP's `/sys/kernel/debug/gpio` on the same hardware lists only 4..125 and 130..174.
  The full dump skips those eight. TWRP's live pin snapshot: `logs/runs/twrp-pinctrl-warm/`
  (`gpio` has direction/drive/pull/level; `pinconf-pins` is empty because the downstream
  pinctrl driver does not implement pinconf_get; `pinmux-pins` names the owning devices).
- 2026-11-18 **TLMM warm vs cold: 0 of 167 pads differ** (`fts-tlmm5-warm` vs
  `fts-tlmm5-cold`, `tools/cepheus-tlmm-diff.py`). Touch 12/55/56/84/122, fingerprint
  37/120/125, IDT 90/104 and NFC 41/42/47/113 are all `cfg 1` / pull-down / input / `io 0`
  at both entries; only VCI GPIO 21 is driven high. Pad state is not retained warm.
- 2026-11-18 **the early scan is not a clean control**: the `fts-latetouch2` early window is
  **quiet** (no FIFO reads), so its `43 05 01` + `43 01 a0` and 1 IRQ mean the chip was
  silent. The early **polling** scan (`fts-early60`, `166095b2…`) produces the edge burst +
  `43 05 30` at ~4.3 s and then silence, before any display init. The failure is not specific
  to the post-display bring-up.
- 2026-11-18 **`43 05 30` is not fatal**: TWRP warm emits `01` at 1.28-1.87 s, `21` at 2.50 s
  and `30` at 5.11 s after real taps, then reports coherent touch for 1340 samples
  (148.5-184.0 s). The shim's failure is the silence after the force-cal, not the status.
- 2026-11-18 TWRP's full probe → DRM blank → unblank order replayed in the late window
  (`fts-twrpfull`, `8ef4cadc…`) still gives 0 touch events in 60 s; the blank-command order
  is not the difference. A 6 s quiet late calibration (`fts-calquiet`, `da6f9d61…`) also
  gives 0 touch events.
- 2026-11-18 TWRP's event handler reads the FIFO **only when the IRQ asserts**; the shim
  reads an empty FIFO every 5 ms. `SHIM_FTS_IRQDRIVEN=1` tests that difference.
- 2026-11-18 Nov 9 recheck: the saved `touch-on6` kernel (`logs/runs/touch-on6-recheck`)
  boots SpringBoard warm with zero touch despite swipes and display off/on, so `touch-on6`/
  `on7` were almost certainly cold-boot successes.
- 2026-11-18 (uncommitted when this was written) `SHIM_FTS_LATE_RESET_MS` in
  `shim/ftstest.c`; committed with these notes.

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

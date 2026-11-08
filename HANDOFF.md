# Handoff: iOS on Xiaomi Mi 9 (cepheus)

Last updated **2026-11-09** (newest work first under "Start here"). Read this first, then [NOTES.md](NOTES.md)
(hardware facts, test loop, bootloader rules).

## Start here (next session)

### Touch driver finalized in the patch stack; rebuild next (2026-11-09)

**Where the port is.** iOS 16.7.16 (iPhone10,3, build 20H392) boots to a full-screen
SpringBoard at 1080x2340, presented as 1125x2436 (centre crop: 22 px sides, 48 px
top/bottom), with working hardware buttons. The last verified run is the touch-off
image `work/boot/runs/touch-off2.img` (log `logs/runs/touch-off2/xnu.txt`):
`SWEET FB: swap` at 49.939 s, 58.2 fps / 291 swaps by 191 s.

**This session changed patches only; no phone run yet.** `0085` now carries the real
cepheus GENI I2C ST FTS driver (666 lines, `bsd/dev/sweet_fts.c`): SE17 at `0xc80000`,
wrapper at `0xcc0000`, SDA 55 / SCL 56 (qup17), IRQ 122, reset 12, VDDIO 84, AVDD
`ldoc1` at 1.8 V, panel 1080x2340. Changes on top of the earlier I2C rewrite:

- Read `QUPV3_HW_VER_REG` from a separate wrapper mapping (`0xcc0000`, +0x4); the SE
  mapping is only 0x4000, so the old `se + 0x40000` read was outside it.
- Accept the QUP >= 3.10 8-bit FIFO-depth field (256-byte FIFO); a zero depth falls
  back to the other layout, and an unusable depth is clamped to 2 instead of aborting.
- Drain RX before and after `DONE` (the final word can land with `DONE`).
- DFS level 0 mismatch logs and continues instead of aborting bring-up.

**Stack consistency.** The 2340 panel height is now propagated through
`0095`/`0110`/`0111`/`0112`/`0119` (including the GPU `PANEL_H`). `0086`'s
`sweet_fts.c` hunk was rebased onto the new driver (wrap2 branches `0x1e004`,
`0x1e008`, `0x1e274`) with updated blob hashes. A targeted replay of patches
0001-0123 over the affected files (`bsd/conf/files`, `SweetHID.cpp`,
`SweetFramebuffer.cpp`, `sweet_gpu.c`, `sweet_fts.c`) applies cleanly; the driver
also passes a host `clang -fsyntax-only` check with kernel stubs. No kernel has been
built from this stack yet.

**Next boot (user builds the kernel).** Use the previous working command line with
touch enabled and the watchdog still off:

```
debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 \
ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 \
launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=60 ufslogsecs=2 \
cepheuswdt=0 cepheusreboot=300 cepheustouch=1 cepheustouchdelay=30 cepheustouchlog=1
```

Check the XNU log for `SWEET FTS: se17 fw=... proto=3 ... qup=... depth=...`,
`info err=0 chip=3639`, `system reset err=0`, `sleep out err=0`, `sense on err=0`,
then touch events. Coordinates should cover the full 1080x2340 panel; the CPU path
inverts the centre crop as `nx=(x+22)/1125`, `ny=(y+48)/2436`.

**Open.** No touch data yet with the new driver. GPU present is still unavailable
(`research/gpu/fw` is empty, so `cepheus-run.sh -G` cannot be used). After touch is
verified, restore `wdt_arm(30)` in `shim/shim.c` and drop `cepheuswdt=0`.

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

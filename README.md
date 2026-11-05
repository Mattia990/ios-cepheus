# ios-cepheus

**Apple's iOS 16 running on a Xiaomi Mi 9 (cepheus).**

A port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) (via [ios-flame](../ios-flame)) to the Xiaomi Mi 9
(`cepheus`, Qualcomm SM8150 / Snapdragon 855, Adreno 640). Apple's open-source XNU kernel (xnu-8796, the iOS 16
generation), with ios-sweet's patches and the cepheus ones on top, runs the **unmodified iOS 16.7.16 (20H392) userland
of the iPhone X**. The phone is booted with `fastboot boot` from Xiaomi's bootloader. Nothing is flashed to the boot
chain; the iOS root lives in the `userdata` partition.

Because cepheus and flame (Pixel 4) share the same SoC (SM8150) and GPU (Adreno 640), most low-level bring-up code
(GIC, timer, PSCI, PMIC, USB, UFS, GPU registers, SDE display engine) copies directly from ios-flame. Board-level
differences (display panel, touch controller, button GPIO mapping, memory layout, partition offsets) are adapted in
`shim/`, `xnu/patches/`, and `tools/mkadt.py`.

> Experimental bring-up, not a usable phone OS. It **erases Android's user data**, Apple's security model is mostly
> switched off (permissive AMFI and sandbox, stub keystore), and many parts are missing. Do not put anything personal
> on it.

## What works (planned, tracking ios-flame)

| Area | State |
| --- | --- |
| Boot | Xiaomi bootloader → `fastboot boot` of the shim → XNU |
| CPU | All 8 cores (4x Cortex-A55 + 4x Cortex-A76) at full clock, PSCI |
| Storage | UFS; the iOS root is an HFS+ image in `userdata`, read/write |
| Display | The AMOLED command-mode panel ABL leaves running. iOS sees an iPhone X (1125x2436); the Adreno 640 scales each frame to fit |
| GPU / Metal | Metal on Mesa's Turnip Vulkan driver through metal2vulkan (`sweetmtl=2`) |
| Touch, buttons | Touch controller TBD; power and volume keys |
| USB | Ethernet to the Mac: SSH, file server, live kernel log |
| Wi-Fi | TBD (same WCN3990-class radio as flame, verify) |
| Apps | Settings, Safari, Clock, Calendar, Notes, Photos, stock apps |
| Time | PMIC RTC |

**Not working yet:** everything not listed above — see HANDOFF.md.

## How it boots

```
Xiaomi bootloader (ABL)
  -> fastboot boot shim-boot.img          temporary; nothing is flashed to boot
     -> shim (shim/): acts as iBoot. Logs to UFS, sets the reboot reason,
        builds the boot args and jumps to XNU with an Apple device tree (tools/mkadt.py)
   -> XNU (xnu/patches): every driver built in: UFS, display, GPU, touch, keys, USB, RPMh,
      /dev/pmem for userland drivers
   -> launchd and the iPhone X userland from userdata
      + phone/: SweetMetal2.bundle (Metal), debug tools
```

## Repository

| Path | What |
| --- | --- |
| `xnu/patches/` | 0001-0081 from ios-sweet, 0082+ for cepheus (one line per patch in NOTES.md) |
| `shim/` | The bare-metal shim ABL boots (`shim/build.sh`) |
| `tools/` | Host tools: ADT and payload, boot-and-collect-logs loop, SSH/file/command access, GPU/Metal debugging |
| `phone/` | Code that runs in iOS: `wifi/`, `metal2/` + `turnip/`, `metal/`, `gpu/`, `tools/` |
| [NOTES.md](NOTES.md) | Hardware facts, boot image rules, test loop, one line per cepheus patch |
| [HANDOFF.md](HANDOFF.md) | The engineering log, newest first |
| [METAL-NEXT.md](METAL-NEXT.md) | The plan for Metal at user quality (copied from ios-flame) |
| [RECOVERY.md](RECOVERY.md), [DEVICE.md](DEVICE.md) | Back to stock Android; the test device and host setup |
| [AGENTS.md](AGENTS.md) | Project rules for agents |
| `docs/ios-sweet/` | ios-sweet's README and notes (history of patches 0001-0081) |

## Not in git: what you need to get yourself

- **The phone**: a Xiaomi Mi 9 (`cepheus`) with the bootloader unlocked. Keep Google's/Xiaomi's factory image in
  `images/` (RECOVERY.md). Back up per-device partitions first (persist, modemst1/2, fsg, fsc, ...) into `backup/`.
- **Boot image tools**: AOSP `mkbootimg` in `downloads/mkbootimg/`, `avbtool.py` in `downloads/avb/`, and the stock
  DTB (`python3 tools/unpack_bootv2.py boot.img stock/` on the factory image's boot.img) as `stock/dtb`.
- **TWRP** for cepheus in `downloads/twrp/`. It writes the root image and reads the logs back.
- **The iOS root**: the iPhone X iOS 16.7.16 (20H392) IPSW. Build the HFS+ image with `tools/mkiosroot.sh` in a
  Linux VM, then `tools/sweet-root-tweaks.py`. Write it from TWRP with `adb exec-in`, not `fastboot flash userdata`.
- **Firmware** from the factory image's `vendor.img`: GPU (`a640_gmu.bin`, `a640_zap.mdt`/`.b02`, `a630_sqe.fw`),
  Wi-Fi (`wlanmdsp.mbn`, `bdwlan-cepheus.bin`, modem files).
- **Toolchains**: Xcode with the iOS SDK (`work/userland/bin/ios-cc`), Homebrew `llvm`, `lld`, `lz4` for the shim,
  [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) for the first kernel build, Mesa and
  metal2vulkan checkouts, dropbear, toybox and dash built for iOS.

## Run it

```sh
python3 tools/mkadt.py work/boot/cepheus-hi.adt    # Apple device tree for cepheus
tools/build-xnu.sh $PWD/work/build.log              # kernel
cp work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet work/boot/kernel-0083
tools/cepheus-run.sh first -k work/boot/kernel-0083 -s 600   # one boot + logs
# once, with the phone up:
tools/cepheus-install-ssh.sh
tools/cepheus-fresh-run.sh second 1800              # from then on: boot and wait for SSH
tools/cepheus-ssh                                    # root shell in iOS
```

`tools/cepheus-run.sh` is the loop underneath. It builds the payload and boot image, runs `fastboot boot`, waits for
the run to end back in fastboot, boots TWRP, then pulls and decodes the kernel log. NOTES.md and HANDOFF.md cover
each step and the boot args.

## Credits

Built on ios-sweet by dipa-studio, via ios-flame, with Metal from UniXNU, metal2vulkan and Mesa. Licences and
details in CREDITS.md.

# Device

Test device for the ios-on-cepheus project. Values below read from the phone with `adb shell getprop`
and from the live FDT (`/proc/device-tree`) on 2026-11-05.

## Hardware

| Field | Value |
|---|---|
| Model | Xiaomi Mi 9 |
| Codename | `cepheus` |
| SoC | Qualcomm Snapdragon 855 (SM8150) |
| GPU | Adreno 640 |
| RAM | ~6 GB LPDDR4X |
| Display | Samsung EA8076 FHD CMD, 1080x2340 @60 Hz, command mode |
| Touch | ST FTS, I2C 0xc80000, addr 0x49 |
| Shipped with | Android 9 (Pie), currently Android 11 |

## Software (stock, as currently installed)

| Field | Value |
|---|---|
| Android version | 11 |
| Build ID | RKQ1.200826.002 / V12.5.4.0.RFACNXM |
| Fingerprint | `Xiaomi/cepheus/cepheus:11/RKQ1.200826.002/V12.5.4.0.RFACNXM:user/release-keys` |
| Bootloader | Unlocked (verified boot state: orange) |
| Active slot | TBD |

## Lock state

| Field | Value | Meaning |
|---|---|---|
| Bootloader | Unlocked | `fastboot flashing unlock` was run; wiped the phone |

Before this project the phone was locked. It was unlocked with `fastboot flashing unlock`, which wiped it.

## Host setup (macOS)

- Tools: `/Users/mattia/Documents/Projects/ios-cepheus/platform-tools/adb` and `fastboot`.
- On the phone: Settings → About phone → tap **Build number** 7 times, then Settings → System → Developer options → **USB debugging** on.
- Accept the "Allow USB debugging?" prompt on the phone after every wipe.

Quick check:

```sh
/Users/mattia/Documents/Projects/ios-cepheus/platform-tools/adb devices -l
/Users/mattia/Documents/Projects/ios-cepheus/platform-tools/adb shell getprop ro.build.fingerprint
```

## Button combos

| Action | How |
|---|---|
| Bootloader (fastboot mode) | Power off, then hold **Power + Volume Down**, or run `adb reboot bootloader` |
| Recovery | From the bootloader, use the volume keys to pick "Recovery mode" and press power |
| Force restart | Hold **Power** for about 30 s |

See [RECOVERY.md](RECOVERY.md) for restoring the phone to stock Android.

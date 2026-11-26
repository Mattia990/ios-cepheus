# Recovery: restoring stock Android on cepheus

To restore the Xiaomi Mi 9 (cepheus) to stock Android:

1. Obtain the full factory image (factory zip) for your exact cepheus variant and put it in
   `images/cepheus-factory/`.
2. Extract `flash-all.sh` (or `flash-all.bat`) and the image files (boot, system, vendor, etc.).
3. Boot the phone into fastboot (Power + Volume Down).
4. Run `images/cepheus-factory/flash-all.sh` from a terminal (on macOS/Linux with adb/fastboot in PATH).

**Important:** this erases everything, including the iOS root in userdata and any per-device
partition modifications. Back up `persist`, `modemst1`, `modemst2`, `fsg`, `fsc` and `boot`/`dtbo`/`vbmeta`
into `backup/` before starting iOS bring-up; they contain your IMEI and calibration data.

Never flash ABL, XBL, TZ, modem or other firmware partitions with modified software. Only use
`fastboot boot` for temporary bring-up images; never `fastboot flash boot` with a custom image.

> **Current device exception (2026-11-26):** the `boot` partition on this phone now contains
> `safari4-cold.img` so a normal power-on boots iOS. Restore the stock boot image with
> `fastboot flash boot downloads/cepheus/boot.img` before following the factory-image steps.
> The general rule above still applies to every other image and partition.

#!/bin/sh
# Pull the WCN3990 firmware and the modem's rmtfs images off a phone booted in
# TWRP into work/wifi/fw/ (gitignored: the rmtfs images carry the IMEI).
#   tools/cepheus-wifi-fw-extract.sh
# The wland installer uploads them to /var/local/fw on the running iOS root.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
if [ -d "$ROOT/../platform-tools" ]; then
	PATH="$ROOT/../platform-tools:$PATH"
	export PATH
fi
adb devices | grep -q recovery || { echo "phone not in TWRP/recovery" >&2; exit 1; }

M=$ROOT/work/wifi/fw/modem
R=$ROOT/work/wifi/fw/rmtfs
mkdir -p "$M" "$R"

# The stock factory image keeps the firmware on the modem partition (vfat):
# /firmware/image on Android, a manually mounted copy here.
adb shell 'mkdir -p /tmp/fw; mountpoint -q /tmp/fw || mount -t vfat -o ro /dev/block/by-name/modem /tmp/fw' >/dev/null 2>&1 || true
adb shell 'ls /tmp/fw/image' | grep -E '^(modem\.(mdt|b[0-9a-f]+)|modemr\.jsn|modemuw\.jsn|wlanmdsp\.mbn|bdwlan\.bin)$' > "$M/.list"
[ -s "$M/.list" ] || { echo "no firmware in /tmp/fw/image (mount the modem partition)" >&2; exit 1; }
while IFS= read -r f; do
	adb pull "/tmp/fw/image/$f" "$M/$f" >/dev/null
done < "$M/.list"
rm -f "$M/.list"
# wlfw.c looks for one BDF under this name; the stock default is bdwlan.bin.
cp "$M/bdwlan.bin" "$M/bdwlan-cepheus.bin"

for p in modemst1 modemst2 fsg fsc; do
	adb exec-out "dd if=/dev/block/by-name/$p bs=1048576 2>/dev/null" > "$R/$p.img"
done

echo "modem:  $(ls "$M" | wc -l | tr -d ' ') files, $(du -sh "$M" | cut -f1)"
echo "rmtfs:  $(ls "$R" | tr -d '\n') $(du -sh "$R" | cut -f1)"
echo "phone-only data: do not commit work/wifi/fw"

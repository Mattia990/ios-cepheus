#!/bin/sh
# Pull the ADSP firmware and the cepheus audio configuration off a phone
# booted in TWRP into work/audio/fw/ (gitignored: Qualcomm firmware and the
# phone's own calibration data).
#   tools/cepheus-audio-fw-extract.sh
#
# The ADSP image (adsp.mdt + adsp.bNN) lives in the modem partition's vfat
# /image directory on this platform, next to modem.mdt.  The CS35L41 speaker
# amplifier's DSP firmware and the mixer XMLs are in the vendor partition.
# Nothing is written to the phone; this only mounts both partitions read-only
# and pulls files.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
if [ -d "$ROOT/../platform-tools" ]; then
	PATH="$ROOT/../platform-tools:$PATH"
	export PATH
fi
adb devices | grep -q recovery || { echo "phone not in TWRP/recovery" >&2; exit 1; }

A=$ROOT/work/audio/fw/adsp
C=$ROOT/work/audio/fw/cs35l41
E=$ROOT/work/audio/fw/etc
mkdir -p "$A" "$C" "$E"

# The modem/firmware partition is vfat with the images under /image.
adb shell 'mkdir -p /tmp/fw; mountpoint -q /tmp/fw || mount -t vfat -o ro /dev/block/by-name/modem /tmp/fw' >/dev/null 2>&1 || true
if adb shell 'test -d /tmp/fw/image' >/dev/null 2>&1; then
	adb shell 'ls /tmp/fw/image' | grep -E '^adsp\.(mdt|b[0-9a-f]+)$' > "$A/.list" || true
fi
if [ ! -s "$A/.list" ]; then
	# Some releases keep adsp on its own partition instead.
	adb shell 'mkdir -p /tmp/adsp; mountpoint -q /tmp/adsp || mount -t vfat -o ro /dev/block/by-name/adsp /tmp/adsp' >/dev/null 2>&1 || true
	if adb shell 'test -d /tmp/adsp/image' >/dev/null 2>&1; then
		adb shell 'ls /tmp/adsp/image' | grep -E '^adsp\.(mdt|b[0-9a-f]+)$' > "$A/.list" || true
		SRC=/tmp/adsp/image
	elif adb shell 'test -f /tmp/adsp/adsp.mdt' >/dev/null 2>&1; then
		adb shell 'ls /tmp/adsp' | grep -E '^adsp\.(mdt|b[0-9a-f]+)$' > "$A/.list" || true
		SRC=/tmp/adsp
	else
		adb shell 'mkdir -p /tmp/adsp; mountpoint -q /tmp/adsp || mount -t ext4 -o ro /dev/block/by-name/adsp /tmp/adsp' >/dev/null 2>&1 || true
		adb shell 'ls /tmp/adsp' | grep -E '^adsp\.(mdt|b[0-9a-f]+)$' > "$A/.list" || true
		SRC=/tmp/adsp
	fi
else
	SRC=/tmp/fw/image
fi
[ -s "$A/.list" ] || { echo "no adsp.mdt in the modem or adsp partition" >&2; exit 1; }
while IFS= read -r f; do
	adb pull "$SRC/$f" "$A/$f" >/dev/null
done < "$A/.list"
rm -f "$A/.list"

# The codec firmware and the mixer XMLs are on the vendor partition.
adb shell 'mkdir -p /tmp/vendor; mountpoint -q /tmp/vendor || mount -t ext4 -o ro /dev/block/by-name/vendor /tmp/vendor' >/dev/null 2>&1 || true
V=/tmp/vendor
for f in \
	firmware/cs35l41-dsp1-spk-prot.wmfw \
	firmware/cs35l41-dsp1-spk-prot.bin \
	firmware/cs35l41-dsp1-diag.wmfw \
	firmware/cs35l41-dsp1-diag-z.wmfw \
	firmware/cs35l41-dsp1-diag-z.bin
do
	adb pull "$V/$f" "$C/$(basename "$f")" >/dev/null 2>&1 || true
done
for f in \
	etc/mixer_paths_tavil.xml \
	etc/mixer_paths_overlay_static.xml \
	etc/mixer_paths_overlay_dynamic.xml \
	etc/audio_platform_info.xml \
	etc/audio_policy_configuration.xml \
	etc/acdbdata/adsp_avs_config.acdb
do
	adb pull "$V/$f" "$E/$(basename "$f")" >/dev/null 2>&1 || true
done

echo "adsp:    $(ls "$A" | wc -l | tr -d ' ') files, $(du -sh "$A" | cut -f1)"
echo "cs35l41: $(ls "$C" | wc -l | tr -d ' ') files, $(du -sh "$C" | cut -f1)"
echo "config:  $(ls "$E" | wc -l | tr -d ' ') files, $(du -sh "$E" | cut -f1)"
echo "phone-only data: do not commit work/audio/fw"

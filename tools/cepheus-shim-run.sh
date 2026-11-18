#!/bin/bash
# Run one shim probe image warm-end-to-end and pull its log.
#
#   tools/cepheus-shim-run.sh NAME work/boot/runs/foo.img
#
# The phone must be in TWRP.  The loop is the one in NOTES.md: reboot to the
# bootloader, boot the probe image, wait for it to reset, boot TWRP again and
# read the probe log out of the tail of userdata into logs/runs/NAME.txt.
set -euo pipefail

name=${1:?usage: $0 NAME IMAGE}
img=${2:?usage: $0 NAME IMAGE}

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export PATH="$root/../platform-tools:$PATH"
out="logs/runs/$name.txt"
log() { echo "[$(date +%H:%M:%S)] $*"; }

wait_adb() {
	local i
	for i in $(seq 1 60); do
		if adb devices | grep -q 2cad7038; then return 0; fi
		sleep 2
	done
	echo "adb device never appeared" >&2
	return 1
}

wait_fb() {
	local i
	for i in $(seq 1 60); do
		if fastboot devices | grep -q 2cad7038; then return 0; fi
		sleep 2
	done
	echo "fastboot device never appeared" >&2
	return 1
}

wait_adb
log "rebooting to bootloader"
adb reboot bootloader
wait_fb
log "booting probe $img"
fastboot boot "$img"
sleep 12
log "booting TWRP for the log"
fastboot boot downloads/twrp/twrp-3.5.2_9-0-cepheus-nofooter.img
wait_adb
tools/cepheus-log-pull.sh shim "$out"
if ! grep -q "resetting" "$out"; then
	echo "probe did not finish (no reset marker in $out)" >&2
	exit 1
fi
log "log: $out"
python3 tools/cepheus-fts-events.py "$out" | awk 'NF>2 {print $3}' | sort | uniq -c |
	awk '{printf "%s x%s, ", $2, $1} END {print ""}'
grep -o "43 5 3[01]" "$out" | sort | uniq -c || true

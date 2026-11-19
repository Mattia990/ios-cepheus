#!/bin/bash
# Cold-boot counterpart of cepheus-shim-run.sh: the phone is already in
# fastboot because the user powered it off and held Vol- + Power.
#
#   tools/cepheus-shim-cold-run.sh NAME work/boot/runs/foo.img
#
# Boots the probe image into the cold boot, waits for the shim's own reset to
# bring fastboot back (cold UFS init can take minutes), then boots TWRP and
# reads the probe log out of the tail of userdata.
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
	for i in $(seq 1 90); do
		if adb devices | grep -q 2cad7038; then return 0; fi
		sleep 2
	done
	echo "adb device never appeared" >&2
	return 1
}

wait_fb() {	# $1: timeout in seconds
	local i
	for i in $(seq 1 "$1"); do
		if fastboot devices | grep -q 2cad7038; then return 0; fi
		sleep 1
	done
	echo "fastboot device never came back" >&2
	return 1
}

wait_fb_gone() {
	local i
	for i in $(seq 1 60); do
		if ! fastboot devices | grep -q 2cad7038; then return 0; fi
		sleep 1
	done
	echo "fastboot device did not drop off after boot" >&2
	return 1
}

fastboot devices | grep -q 2cad7038 || {
	echo "phone is not in fastboot; cold boot it first (power off, Vol- + Power)" >&2
	exit 1
}
log "booting probe $img (cold)"
fastboot boot "$img"
# The shim resets the phone itself; cold UFS init before the dump can take
# several minutes (517 s seen once), then the per-pad dump runs under a 120 s
# watchdog.  `fastboot boot` returns while the device is still enumerated, so
# wait for it to drop off first, then allow 15 min for the reset.
wait_fb_gone
log "waiting for the probe to return to fastboot"
wait_fb 900
log "booting TWRP for the log"
fastboot boot downloads/twrp/twrp-3.5.2_9-0-cepheus-nofooter.img
wait_adb
tools/cepheus-log-pull.sh shim "$out"
if ! grep -q "resetting" "$out"; then
	echo "warning: no reset marker in $out" >&2
fi
log "log: $out"

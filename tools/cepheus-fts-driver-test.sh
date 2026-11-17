#!/bin/bash
# Drive the TWRP FTS 5.2.4 driver from its /proc/fts/driver_test node while
# Android is *not* running, so a warm-boot chip experiment needs no rebuild.
#
#   tools/cepheus-fts-driver-test.sh baseline OUTDIR   frames + sysinfo, no state change
#   tools/cepheus-fts-driver-test.sh cycle    OUTDIR   power-cycle, reset, sense-on, watch 8 s
#
# Phone in TWRP with adb.  `baseline` only reads (GETMSFRAME leaves the scan
# off, so it sends sense-on again afterwards).  `cycle` runs the driver's own
# fts_chip_powercycle (vdd_reg off, avdd_reg off, reset low, vdd on, 1 ms,
# avdd on, 5/10 ms, reset high), then a system reset and a sense-on; that is
# exactly the rail + reset path XNU takes at 30 s.  Watch dmesg during the
# 8 s window: a warm ghost shows as 0x13/0x23/0x33 contacts on the right/top
# edge followed by 43 05 30 and silence.
#
# Opcodes (drivers/input/touchscreen/fts_521/fts_proc.c):
#   0x01 WRITE        echo "01 A0 00 01"  -> sense on (0xA0, mode active, MS)
#   0x12 SYSTEMRESET  echo "12"
#   0x14 POWERCYCLE   echo "14"
#   0x15 READSYSINFO  echo "15 00"
#   0x23 GETMSFRAME   echo "23 00"        -> MS raw (01 filter, 02 strength, 03 baseline)
set -euo pipefail
mode=${1:-} out=${2:-}
case $mode in baseline|cycle) ;; *) echo "usage: $0 baseline|cycle OUTDIR" >&2; exit 2 ;; esac
[ -n "$out" ] || { echo "usage: $0 baseline|cycle OUTDIR" >&2; exit 2; }
mkdir -p "$out"
dt() { adb shell "echo \"$1\" > /proc/fts/driver_test"; }
frame() { # frame NAME TYPE
	dt "23 $2"
	adb shell cat /proc/fts/driver_test > "$out/frame-$1.txt" 2>&1 || true
	dt "01 A0 00 01" # GETMSFRAME leaves the scan off
}
says() { adb shell cat /proc/fts/driver_test > "$out/$1.txt" 2>&1 || true; }

adb wait-for-device
adb shell 'test -e /proc/fts/driver_test' || {
	echo "/proc/fts/driver_test missing: is this the TWRP FTS 5.2.4 kernel?" >&2
	exit 1
}

if [ "$mode" = baseline ]; then
	adb shell dmesg -c > /dev/null 2>&1 || true
	dt "15 00"
	says sysinfo
	frame raw 00
	frame filter 01
	frame strength 02
	frame baseline 03
	adb shell dmesg > "$out/dmesg-baseline.txt" 2>&1 || true
	echo "baseline frames in $out/frame-*.txt (scan on again)"
	exit 0
fi

echo "power cycle + reset + sense on; touch the screen for the next 8 s"
adb shell dmesg -c > /dev/null 2>&1 || true
dt "14"
sleep 1
dt "12"
dt "01 A0 00 01"
sleep 8
adb shell dmesg > "$out/dmesg-cycle.txt" 2>&1 || true
frame raw 00
frame strength 02
frame baseline 03
adb shell dmesg > "$out/dmesg-after.txt" 2>&1 || true
echo "logs in $out/"
grep -E 'FTS|fts' "$out/dmesg-cycle.txt" | tail -40 || true

#!/bin/sh
# Install wland (the WCN3990 driver from phone/wifi) and its firmware into the
# running iOS root over the kernel file server, and add the launchd boot job.
#   tools/cepheus-wifi-install.sh [--conf WLAN.CONF] [--no-job]
#
# WLAN.CONF is the wland auto-join file ("ssid=NAME" / "psk=PASS" lines). It is
# copied to /var/local/wlan.conf on the phone and must never enter the repo.
# Firmware comes from tools/cepheus-wifi-fw-extract.sh (TWRP, gitignored).
#
# wland runs from launchd as `wland auto`: once per phone boot, ~23 s after
# launchd starts (modem PAS boot + 4 MB of firmware over TFTP). Log:
# /var/local/wland.log (previous boot: .prev). Off switch: /var/local/wland.off
# or boot-arg sweetwifi=0. The job takes effect on the next iOS boot.
set -eu
cd "$(dirname "$0")/.."

W=phone/wifi/out/wland
M=work/wifi/fw/modem
R=work/wifi/fw/rmtfs
conf=
job=1
while [ $# -gt 0 ]; do
	case $1 in
	--conf) conf=${2:?--conf needs a file}; shift 2 ;;
	--no-job) job=0; shift ;;
	-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
	*) echo "usage: $0 [--conf WLAN.CONF] [--no-job]" >&2; exit 2 ;;
	esac
done

[ -s "$W" ] || { echo "missing $W (dispatch the wlan workflow and copy the artifact)" >&2; exit 1; }
[ -s "$M/modem.mdt" ] && [ -s "$M/wlanmdsp.mbn" ] && [ -s "$M/bdwlan-cepheus.bin" ] ||
	{ echo "missing firmware in $M (run tools/cepheus-wifi-fw-extract.sh in TWRP)" >&2; exit 1; }
for p in modemst1 modemst2 fsg fsc; do
	[ -s "$R/$p.img" ] || { echo "missing $R/$p.img (run tools/cepheus-wifi-fw-extract.sh)" >&2; exit 1; }
done
[ -z "$conf" ] || [ -s "$conf" ] || { echo "no such file: $conf" >&2; exit 1; }

# Direct 192.168.7.2:2223 works whenever the USB Ethernet link is up; the
# FlameBridge relay is only needed by tools that insist on 127.0.0.1.
: "${CEPH_FILES_HOST:=192.168.7.2}"
export CEPH_FILES_HOST
F="python3 tools/cepheus-files.py"

echo "probing the file server ($CEPH_FILES_HOST:2223)..."
printf x > /tmp/wifi-probe
ready=0
for _ in $(seq 1 90); do
	if $F mkdir /var/local 755 >/dev/null 2>&1 &&
	    $F put /tmp/wifi-probe /var/local/wifi-probe 644 >/dev/null 2>&1; then ready=1; break; fi
	sleep 2
done
[ "$ready" = 1 ] || { echo "file server not reachable (is iOS up?)" >&2; exit 1; }
$F rm /var/local/wifi-probe >/dev/null 2>&1 || true

echo "installing wland and firmware..."
for d in /var/local/bin /var/local/fw /var/local/fw/modem /var/local/fw/rmtfs; do
	$F mkdir "$d" 755 >/dev/null 2>&1 || true
done
$F put "$W" /var/local/bin/wland 755

for f in "$M"/*; do
	b=$(basename "$f")
	case $b in bdwlan.bin) continue ;; esac
	$F put "$f" "/var/local/fw/modem/$b" 644
done
for p in modemst1 modemst2 fsg fsc; do
	$F put "$R/$p.img" "/var/local/fw/rmtfs/$p.img" 600
done
# wland renames the log on every boot; a 666 file it cannot create itself if
# /var/local is not writable by the job.
: > /tmp/wland-empty
$F put /tmp/wland-empty /var/local/wland.log 666 >/dev/null 2>&1 || true

if [ -n "$conf" ]; then
	$F put "$conf" /var/local/wlan.conf 600
	echo "wlan.conf installed (not printed)"
fi

if [ "$job" = 1 ]; then
	echo "adding the launchd job..."
	$F get /System/Library/xpc/launchd.plist /tmp/launchd.plist.cur >/dev/null
	python3 - /tmp/launchd.plist.cur /tmp/launchd.plist.new <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['LaunchDaemons']['/System/Library/LaunchDaemons/org.iosonpixel.wland.plist'] = {
    'Label': 'org.iosonpixel.wland',
    'ProgramArguments': ['/var/local/bin/wland', 'auto'],
    'RunAtLoad': True, 'UserName': 'root',
    'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin'},
    'StandardErrorPath': '/var/local/wland.err',
}
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(p['LaunchDaemons']))
PY
	if ! $F get /System/Library/xpc/launchd.plist.orig /tmp/launchd.plist.orig >/dev/null 2>&1; then
		cp /tmp/launchd.plist.cur /tmp/launchd.plist.orig
		$F put /tmp/launchd.plist.orig /System/Library/xpc/launchd.plist.orig 644
	fi
	$F put /tmp/launchd.plist.new /System/Library/xpc/launchd.plist 644
fi

echo "INSTALL DONE"
echo "next: reboot (tools/cepheus-cmd.py reboot) and boot iOS; watch /var/local/wland.log"

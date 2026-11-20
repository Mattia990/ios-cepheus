#!/bin/bash
# Install SSH (dropbear) + dash + toybox into the running iOS root over the file server
# (tools/cepheus-files.py, FlameBridge). Then reboot: launchd reads its job cache at boot.
#
#   tools/cepheus-install-ssh.sh
#
# Binaries: work/userland/stage (from .github/workflows/ssh.yml, signed ad hoc). Key:
# work/userland/ssh/id_ed25519 (ssh -i that root@192.168.1.100 over Wi-Fi, or through
# FlameBridge at 127.0.0.1:2222).
set -euo pipefail
cd "$(dirname "$0")/.."
F="python3 tools/cepheus-files.py"
U=work/userland
$F mkdir /var/local 755
$F mkdir /var/local/bin 755
$F mkdir /private/etc/dropbear 700
$F mkdir /var/root/.ssh 700
for b in dropbearmulti dash; do
	[ -f "$U/stage/$b" ] || { echo "missing $U/stage/$b (run the ssh workflow)" >&2; exit 1; }
	$F put "$U/stage/$b" "/var/local/bin/$b" 755
done
if [ -f "$U/stage/toybox" ]; then
	$F put "$U/stage/toybox" /var/local/bin/toybox 755
else
	echo "warning: no toybox in $U/stage; installing dash and dropbear only" >&2
fi
for a in dropbear scp dbclient dropbearkey dropbearconvert; do
	$F rm "/var/local/bin/$a" 2>/dev/null || true
	$F link /var/local/bin/dropbearmulti "/var/local/bin/$a" || true
done
if [ -f "$U/stage/toybox" ]; then
	# Only applets in toybox's macos_miniconfig: a symlink to an applet that
	# was not built would shadow the iOS tool of the same name (PATH order).
	for a in ls cat cp mv rm mkdir rmdir ln chmod chown kill grep sed head tail wc find xargs \
		sort uniq cut tr df du uname date env id whoami od sleep touch stat \
		which file tee printf echo true false test basename dirname readlink realpath \
		hostname nc md5sum sha1sum sha256sum tar zcat; do
		$F link /var/local/bin/toybox "/var/local/bin/$a" >/dev/null 2>&1 || true
	done
fi
$F link /var/local/bin/dash /bin/sh || echo "(/bin/sh exists)"
$F put $U/ssh/id_ed25519.pub /var/root/.ssh/authorized_keys 600
for t in psx prefs oslog nettest netsvc openapp lsreg imgtest membw pstack; do
	if [ -f "phone/tools/out/$t" ]; then
		$F put "phone/tools/out/$t" "/var/local/bin/$t" 755
	fi
done

# launchd job (cache: /System/Library/xpc/launchd.plist; original kept as .orig on the phone)
$F get /System/Library/xpc/launchd.plist $U/launchd.plist.cur
# Keep the first backup; do not replace it with a plist that already has our job.
if ! $F get /System/Library/xpc/launchd.plist.orig $U/launchd.plist.orig 2>/dev/null; then
	$F put $U/launchd.plist.cur /System/Library/xpc/launchd.plist.orig 644
fi
python3 - "$U/launchd.plist.cur" "$U/launchd.plist.new" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['LaunchDaemons']['/System/Library/LaunchDaemons/org.iosonpixel.dropbear.plist'] = {
    'Label': 'org.iosonpixel.dropbear',
    'ProgramArguments': ['/var/local/bin/dropbear', '-F', '-E', '-R', '-p', '22'],
    'RunAtLoad': True, 'KeepAlive': True, 'UserName': 'root',
    # System paths first: toybox is a fallback, so a missing applet cannot
    # shadow a working iOS tool with the same name.
    'EnvironmentVariables': {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin:/var/local/bin'},
    'StandardErrorPath': '/var/local/dropbear.log',
}
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(p['LaunchDaemons']))
PY
$F put $U/launchd.plist.new /System/Library/xpc/launchd.plist 644
echo "installed; reboot the phone (tools/cepheus-cmd.py reboot) and boot again for launchd to start dropbear"

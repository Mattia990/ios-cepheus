#!/usr/bin/env python3
"""Remove one stale MachService from mediaserverd's launchd job.

  cepheus-capturesource-plist.py IN.plist OUT.plist

IN is either the boot cache /System/Library/xpc/launchd.plist (the phone boots
with launchd_unsecure_cache=1, so this is the job launchd actually uses) or the
per-job /System/Library/LaunchDaemons/com.apple.mediaserverd.plist. The job is
found by its Label, so the cache's file-path keys do not matter.

launchd advertises `com.apple.coremedia.capturesource` because the job lists it
under MachServices, but mediaserverd never bootstrap_check_in()s it. A client
(SpringBoard's AVControlCenterCaptureDeviceWatcher through CMCapture) gets
launchd's placeholder port, sends its first XPC message and waits forever in
xpc_connection_activate. Removing the key makes bootstrap_look_up fail at once;
AVCaptureProprietaryDefaultsSingleton logs "Tried to copy
proprietaryDefaultsSource but got NULL" and returns instead of wedging.

This is a persistent rootfs edit. Keep IN.plist as the backup; the restore path
is to put it back and reboot (launchd reads the job at boot).
"""
import plistlib
import sys

KEY = 'com.apple.coremedia.capturesource'
LABEL = 'com.apple.mediaserverd'


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        plist = plistlib.load(f)

    jobs = plist['LaunchDaemons'].values() if 'LaunchDaemons' in plist else [plist]
    matches = [job for job in jobs if job.get('Label') == LABEL]
    if len(matches) != 1:
        raise SystemExit(f'{src}: {len(matches)} jobs with Label {LABEL}, expected 1')
    services = matches[0].get('MachServices')
    if not isinstance(services, dict) or KEY not in services:
        raise SystemExit(f'{src}: {KEY} is not in {LABEL} MachServices')
    del services[KEY]

    with open(dst, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    print(f'{dst}: removed {KEY} from {LABEL}; {len(services)} MachServices remain')


if __name__ == '__main__':
    main()

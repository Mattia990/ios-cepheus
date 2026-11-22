#!/bin/sh
# Install phone/metal2/out/SweetMetal2.bundle on the running phone without
# overwriting a mapped binary. The executable and libvulkan_freedreno.dylib are
# staged under /var/local/bin and the bundle path becomes a symlink to the
# staged executable; the bridge resolves its own directory to /var/local/bin,
# so /var/local/bin/libvulkan_freedreno.dylib must point at the staged dylib.
#
#   CEPH_FILES_HOST=192.168.7.2 tools/cepheus-metal2-install.sh [--no-restart]
#
# The phone shell on this image has no rm/mv/tar/toybox; all file operations go
# through tools/cepheus-files.py (the kernel file server). CEPH_SSH_HOST
# overrides the direct USB SSH host (default 192.168.7.2).
set -eu
cd "$(dirname "$0")/.."
B=phone/metal2/out/SweetMetal2.bundle
for f in SweetMetal2 libvulkan_freedreno.dylib Info.plist _CodeSignature/CodeResources; do
	[ -s "$B/$f" ] || { echo "incomplete bundle: no $B/$f (build failed?)" >&2; exit 1; }
done

F="python3 tools/cepheus-files.py"
SSH_HOST=${CEPH_SSH_HOST:-192.168.7.2}
SSH="ssh -i work/userland/ssh/id_ed25519 -o ConnectTimeout=5 -o StrictHostKeyChecking=no root@$SSH_HOST"
export CEPH_FILES_HOST=${CEPH_FILES_HOST:-$SSH_HOST}

# Content-addressed staging names: never overwrite a file a running backboardd
# may have mapped. Removing a mapped file is safe; PUT into it is not.
exe_hash=$(shasum -a 256 "$B/SweetMetal2" | awk '{print substr($1,1,12)}')
dylib_hash=$(shasum -a 256 "$B/libvulkan_freedreno.dylib" | awk '{print substr($1,1,12)}')
exe_stage=/var/local/bin/SweetMetal2.$exe_hash
dylib_stage=/var/local/bin/libvulkan_freedreno.$dylib_hash.dylib

# Keep the original bundle executable once, for rollback. If the bundle path is
# already a symlink and no original was saved, do not save the patched binary
# as the original.
if ! $SSH 'test -f /var/local/bin/SweetMetal2.orig'; then
	if $SSH 'test -L /System/Library/Extensions/SweetMetal2.bundle/SweetMetal2'; then
		echo "warning: bundle executable is already a symlink and /var/local/bin/SweetMetal2.orig is missing" >&2
		echo "warning: not saving a new original; continuing" >&2
	else
		tmp=$(mktemp)
		$F get /System/Library/Extensions/SweetMetal2.bundle/SweetMetal2 "$tmp"
		$F put "$tmp" /var/local/bin/SweetMetal2.orig 755
		rm -f "$tmp"
	fi
fi

# Stage the new files. rm first: PUT does not truncate an existing file.
$F rm "$exe_stage" 2>/dev/null || true
$F rm "$dylib_stage" 2>/dev/null || true
$F put "$B/SweetMetal2" "$exe_stage" 755
$F put "$B/libvulkan_freedreno.dylib" "$dylib_stage" 755

# Update bundle metadata. These are not the mapped executable; the
# _CodeSignature directory must exist before CodeResources can be written.
$F mkdir /System/Library/Extensions/SweetMetal2.bundle/_CodeSignature 755 >/dev/null 2>&1 || true
$F rm /System/Library/Extensions/SweetMetal2.bundle/Info.plist 2>/dev/null || true
$F put "$B/Info.plist" /System/Library/Extensions/SweetMetal2.bundle/Info.plist 644
$F rm /System/Library/Extensions/SweetMetal2.bundle/_CodeSignature/CodeResources 2>/dev/null || true
$F put "$B/_CodeSignature/CodeResources" /System/Library/Extensions/SweetMetal2.bundle/_CodeSignature/CodeResources 644

# Swap the executable and the dylib symlink. The old inodes stay mapped until
# backboardd exits.
$F rm /System/Library/Extensions/SweetMetal2.bundle/SweetMetal2
$F link "$exe_stage" /System/Library/Extensions/SweetMetal2.bundle/SweetMetal2
$F rm /var/local/bin/libvulkan_freedreno.dylib 2>/dev/null || true
$F link "$dylib_stage" /var/local/bin/libvulkan_freedreno.dylib

[ "${1:-}" = --no-restart ] || $SSH '/var/local/bin/psx -k backboardd 9' || true

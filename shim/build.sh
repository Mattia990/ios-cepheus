#!/bin/bash
# Build the cepheus shim and wrap it in a boot image v0 with Xiaomi's ABL layout
# (confirmed from cepheus stock boot.img):
#   header_version 0, kernel offset 4096, kernel load addr 0x8000,
#   UNCOMPRESSED kernel, no ramdisk, AVB footer not required.
#
#   shim/build.sh [PAYLOAD] [OUT]   PAYLOAD: tools/mkpayload.py output (ramdisk);
#                                   none -> probe-only image
#   SHIM_CFLAGS=-DSHIM_SHOWLOG shim/build.sh "" OUT
#                                   show the logs on UFS on screen (showlog.c)
# Needs Homebrew llvm + lld.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
LLVM=/opt/homebrew/opt/llvm/bin
LLD=/opt/homebrew/opt/lld/bin
CC="$LLVM/clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra ${SHIM_CFLAGS:-}"
SRCS="start.S shim.c console.c log.c util.c mmu.c fdt.c loader.c ufs.c spmi.c sde.c showlog.c diag.c disptest.c gpuprobe.c gpufw.S smmuprobe.c ramlog.c panel.c"
mkdir -p out
objs=
for f in $SRCS; do
	$CC $CFLAGS -c "$f" -o "out/${f%.*}.o"
	objs="$objs out/${f%.*}.o"
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($LLVM/llvm-readelf -r $(echo "$objs" | sed 's| out/start.o||') | grep -c R_AARCH64_ABS64 || true)
[ "$abs" = 0 ] || {
	$LLVM/llvm-readelf -r $(echo "$objs" | sed 's| out/start.o||') | grep -B5 R_AARCH64_ABS64 >&2
	echo "error: $abs absolute relocations in C objects" >&2
	exit 1
}
$LLD/ld.lld -static -nostdlib -T shim.ld --build-id=none -N $objs -o out/shim.elf
$LLVM/llvm-objcopy -O binary out/shim.elf out/Image
# Pad to image_size so .bss/stacks exist as zeroed bytes.
size=$($LLVM/llvm-nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
# Cepheus ABL accepts UNCOMPRESSED kernels (stock boot.img is uncompressed).
cp out/Image out/Image.uncompressed
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	# ABL dislikes a zero-size ramdisk: an empty newc cpio.
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
OUT=${2:-out/shim-boot.img}
# Cepheus uses header version 0, kernel offset 4096, base 0.
# DTB included from cepheus-zip-recovery-creator/sm8150-v2.dtb
PYTHONPATH="$ROOT/../cepheus-zip-recovery-creator/mkbootimg" python3 "$ROOT/../cepheus-zip-recovery-creator/mkbootimg/mkbootimg.py" \
	--kernel out/Image.uncompressed --ramdisk out/ramdisk.bin \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
	--dtb "$ROOT/../cepheus-zip-recovery-creator/sm8150-v2.dtb" --dtb_offset 0x0000000001f00000 \
	--pagesize 4096 --os_version 11.0.0 --os_patch_level 2021-10 --header_version 2 \
	--cmdline "debug=0xc serial=3 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 cepheuswdt=0 cepheusreboot=600" -o "$OUT"
# Xiaomi's ABL requires an AVB footer (like stock boot.img).
#python3 "$ROOT/downloads/avb/avbtool.py" add_hash_footer --image "$OUT" \
#	--partition_size 134217728 --partition_name boot
ls -l out/Image "$OUT"

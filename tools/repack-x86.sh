#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
set -e
cd "$ROOT_DIR"
echo "=== re-pack initramfs-full-x86 ==="
cd rootfs-x86
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$ROOT_DIR"/initramfs-full-x86.cpio.gz
cd "$ROOT_DIR"
gzip -t initramfs-full-x86.cpio.gz && echo full-x86-ok
echo "=== re-populate disk-x86 ==="
SRC=rootfs-x86 IMG=disk-x86.img bash tools/populate-disk.sh 2>&1 | grep -E 'creating|writing|OK' | head -5
echo "=== rebuild UEFI x86 ==="
bash build-uefi-x86.sh 2>&1 | tail -2
#!/bin/bash
set -e
cd /home/melton/myos
echo "=== repack initramfs-full-x86 (with autotest) ==="
cd rootfs-x86
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > /home/melton/myos/initramfs-full-x86.cpio.gz
cd /home/melton/myos
gzip -t initramfs-full-x86.cpio.gz && echo full-ok
echo "=== repopulate disk-x86 ==="
SRC=rootfs-x86 IMG=disk-x86.img bash tools/populate-disk.sh 2>&1 | grep -E 'creating' | head -2
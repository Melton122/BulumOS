#!/bin/bash
set -e
cd /home/melton/myos
RFS=rootfs

echo "=== rebuild arm64 init (uname banner) ==="
aarch64-linux-gnu-gcc -static -O2 -Wall -o "$RFS/sbin/init" src/init.c 2>&1 | grep -E 'error' | head -5 || true
chmod 755 "$RFS/sbin/init"
echo "arm64 init rebuilt"

echo "=== incorp into arm64 initramfs + disk ==="
cd "$RFS"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > /home/melton/myos/initramfs-full.cpio.gz
cd /home/melton/myos
gzip -t initramfs-full.cpio.gz && echo "initramfs-full-ok"
SRC=rootfs IMG=disk.img bash tools/populate-disk.sh 2>&1 | grep -E 'creating' | head -3
echo "=== rebuild arm64 UEFI (memdisk embeds new initramfs) ==="
bash build-all-uefi.sh 2>&1 | tail -3
echo DONE
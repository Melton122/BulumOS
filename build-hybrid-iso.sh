#!/bin/bash
set -e
cd /home/melton/myos

echo "=== building hybrid ISO (El Torito + GPT ESP append; reuses /tmp/efiboot.img) ==="
[ -f /tmp/efiboot.img ] || { echo "ESP image missing; run build-all-uefi.sh first"; exit 1; }
xorriso -as mkisofs -o bulumos.iso \
  -V BULUMOS -J -R \
  -e --interval:appended_partition_2:all:: \
  -no-emul-boot \
  -append_partition 2 0xef /tmp/efiboot.img \
  -isohybrid-gpt-basdat \
  iso 2>&1 | tail -3
ls -la bulumos.iso
echo "=== verify GPT/ESP structure ==="
fdisk -l bulumos.iso 2>/dev/null | tail -8 || sudo fdisk -l bulumos.iso | tail -8

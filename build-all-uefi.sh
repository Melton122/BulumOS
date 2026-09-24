#!/bin/bash
set -euo pipefail
cd /home/melton/myos

echo "=== 1. build EFI app ==="
bash build-uefi.sh 2>&1 | grep -E '===|BOOTAA64|error'

echo "=== 2. build FAT32 ESP (BOOTAA64.EFI only; GRUB reads everything from its memdisk) ==="
rm -f /tmp/efiboot.img
dd if=/dev/zero of=/tmp/efiboot.img bs=1M count=72 status=none
mkfs.vfat -n BULUMOS /tmp/efiboot.img >/dev/null
mmd -i /tmp/efiboot.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/efiboot.img iso/EFI/BOOT/BOOTAA64.EFI ::/EFI/BOOT/
mdir -i /tmp/efiboot.img ::/EFI/BOOT
cp /tmp/efiboot.img /tmp/efiboot.img.noiso

echo "=== 3. hybrid ISO (El Torito + GPT ESP append) ==="
bash build-hybrid-iso.sh 2>&1 | grep -E '===|BOOTAA64|Writing|Written|sectors|MiB'

echo "=== 4. UEFI disk image ==="
cp /tmp/efiboot.img bulumos-uefi.img
ls -la bulumos.iso bulumos-uefi.img
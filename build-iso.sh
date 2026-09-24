#!/bin/bash
set -e
cd /home/melton/myos
bash build-uefi.sh
echo "=== building ISO ==="
rm -rf iso/boot
mkdir -p iso/boot/grub
cp kernel/arch/arm64/boot/Image iso/boot/Image
cp initramfs.cpio.gz iso/boot/initramfs.cpio.gz
cp iso/EFI/BOOT/BOOTAA64.EFI /tmp/BOOTAA64.EFI
rm -rf iso/EFI
mkdir -p iso/EFI/BOOT
mv /tmp/BOOTAA64.EFI iso/EFI/BOOT/BOOTAA64.EFI
ls -la iso/EFI/BOOT/ iso/boot/
xorriso -as mkisofs -o bulumos.iso \
  -V BULUMOS \
  -J -R \
  -e EFI/BOOT/BOOTAA64.EFI -no-emul-boot \
  -iso-level 3 \
  iso 2>&1 | tail -3
ls -la bulumos.iso

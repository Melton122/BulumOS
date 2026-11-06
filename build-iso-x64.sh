#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
set -e
cd "$ROOT_DIR"

echo "=== assembling x86_64 ISO ==="
rm -rf iso/boot
mkdir -p iso/boot/grub

if [ -f kernel/arch/x86/boot/bzImage ]; then
    echo "Found kernel at kernel/arch/x86/boot/bzImage"
    cp kernel/arch/x86/boot/bzImage iso/boot/bzImage
else
    echo "Error: kernel/arch/x86/boot/bzImage not found"
    exit 1
fi

if [ -f initramfs.cpio.gz ]; then
    cp initramfs.cpio.gz iso/boot/initramfs.cpio.gz
else
    echo "Error: initramfs.cpio.gz not found"
    exit 1
fi

if [ -f iso/EFI/BOOT/BOOTX64.EFI ]; then
    echo "Found BOOTX64.EFI"
else
    echo "Warning: BOOTX64.EFI missing from iso/EFI/BOOT/"
fi

xorriso -as mkisofs -o bulumos-x64.iso \
  -V BULUMOS \
  -J -R \
  -e EFI/BOOT/BOOTX64.EFI -no-emul-boot \
  -iso-level 3 \
  iso 2>&1 | tail -3

ls -la bulumos-x64.iso

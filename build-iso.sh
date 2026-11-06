#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
set -e
cd "$ROOT_DIR"
bash build-uefi.sh
echo "=== building ISO ==="
rm -rf iso/boot
mkdir -p iso/boot/grub
# Keep the ISO filesystem consistent with the x86_64 grub memdisk: the arm64
# initramfs.cpio.gz cannot run under this kernel (exec format error).
cp kernel/arch/x86/boot/bzImage iso/boot/bzImage
cp initramfs-switch-x86.cpio.gz iso/boot/initramfs-switch.cpio.gz
cp initramfs-full-x86.cpio.gz iso/boot/initramfs.cpio.gz
cp iso/EFI/BOOT/BOOTX64.EFI /tmp/BOOTX64.EFI
rm -rf iso/EFI
mkdir -p iso/EFI/BOOT
mv /tmp/BOOTX64.EFI iso/EFI/BOOT/BOOTX64.EFI
ls -la iso/EFI/BOOT/ iso/boot/

echo "=== FAT ESP image (El Torito boot image must be FAT, not the raw PE) ==="
rm -f /tmp/efiboot-iso.img
# Must hold BOOTX64.EFI, which embeds the whole grub memdisk (~68M).
dd if=/dev/zero of=/tmp/efiboot-iso.img bs=1M count=96 status=none
mkfs.vfat -n BULUMOS /tmp/efiboot-iso.img >/dev/null
mmd -i /tmp/efiboot-iso.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/efiboot-iso.img iso/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
mdir -i /tmp/efiboot-iso.img ::/EFI/BOOT

echo "=== hybrid ISO (El Torito via appended GPT ESP, same as build-hybrid-iso.sh) ==="
xorriso -as mkisofs -o bulumos.iso \
  -V BULUMOS \
  -J -R \
  -e --interval:appended_partition_2:all:: \
  -no-emul-boot \
  -append_partition 2 0xef /tmp/efiboot-iso.img \
  -isohybrid-gpt-basdat \
  iso 2>&1 | tail -3
ls -la bulumos.iso

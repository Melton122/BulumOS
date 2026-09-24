#!/bin/bash
# BulumOS x86_64 UEFI build: grub x86_64-efi memdisk + BOOTX64.EFI + ESP + hybrid ISO.
set -euo pipefail
cd /home/melton/myos

MKIMAGE=/usr/bin/grub-mkimage
MODS=/usr/lib/grub/x86_64-efi
KIMG=kernel/arch/x86/boot/bzImage
INITRD_SW=initramfs-switch-x86.cpio.gz
INITRD_FULL=initramfs-full-x86.cpio.gz

[ -f "$MODS/normal.mod" ] || { echo "missing x86_64 grub modules"; exit 1; }
[ -f "$KIMG" ] || { echo "missing $KIMG"; exit 1; }

echo "=== 1. grub.cfg ==="
cat > /tmp/grub-x86.cfg <<'EOF'
set timeout=3
set default=0
menuentry "BulumOS 1.2 (Linux 7.1.5 x86-64, persistent root)" {
    linux /boot/bzImage console=ttyS0,115200n8 root=/dev/vdb rw rootfstype=ext4 init=/sbin/init
    initrd /boot/initramfs-switch.cpio.gz
}
menuentry "BulumOS 1.2 - Recovery mode (ramdisk)" {
    linux /boot/bzImage console=ttyS0,115200n8 rdinit=/sbin/init recovery
    initrd /boot/initramfs-full.cpio.gz
}
EOF

echo "=== 2. memdisk (FAT) ==="
rm -f /tmp/memdisk-x86.img
dd if=/dev/zero of=/tmp/memdisk-x86.img bs=1M count=64 status=none
mkfs.vfat -n MEMDISK /tmp/memdisk-x86.img >/dev/null
mmd -i /tmp/memdisk-x86.img ::/boot ::/boot/grub
mcopy -i /tmp/memdisk-x86.img /tmp/grub-x86.cfg ::/boot/grub/grub.cfg
mcopy -i /tmp/memdisk-x86.img "$KIMG" ::/boot/bzImage
mcopy -i /tmp/memdisk-x86.img "$INITRD_SW" ::/boot/initramfs-switch.cpio.gz
mcopy -i /tmp/memdisk-x86.img "$INITRD_FULL" ::/boot/initramfs-full.cpio.gz
mdir -i /tmp/memdisk-x86.img ::/boot

echo "=== 3. BOOTX64.EFI ==="
rm -rf iso-x86
mkdir -p iso-x86/EFI/BOOT
"$MKIMAGE" -O x86_64-efi -d "$MODS" \
  -m /tmp/memdisk-x86.img \
  -o iso-x86/EFI/BOOT/BOOTX64.EFI \
  fat archelp cpio tar memdisk normal linux iso9660 ext2 search configfile \
  terminal efi_gop echo ls cat test all_video boot minicmd

echo "=== 4. ESP image ==="
rm -f /tmp/efiboot-x86.img
dd if=/dev/zero of=/tmp/efiboot-x86.img bs=1M count=72 status=none
mkfs.vfat -n BULUMOS /tmp/efiboot-x86.img >/dev/null
mmd -i /tmp/efiboot-x86.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/efiboot-x86.img iso-x86/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
cp /tmp/efiboot-x86.img bulumos-uefi-x86.img

echo "=== 5. hybrid ISO ==="
xorriso -as mkisofs -o bulumos-x86.iso \
  -V BULUMOS -J -R \
  -e --interval:appended_partition_2:all:: \
  -no-emul-boot \
  -append_partition 2 0xef /tmp/efiboot-x86.img \
  -isohybrid-gpt-basdat \
  iso-x86 2>&1 | tail -1

echo "=== done ==="
ls -la bulumos-uefi-x86.img bulumos-x86.iso
#!/bin/bash
# BulumOS VirtualBox x86_64 build: UEFI ISO with root=/dev/sda (SATA), console on screen.
set -euo pipefail
cd /home/melton/myos

MKIMAGE=/usr/bin/grub-mkimage
MODS=/usr/lib/grub/x86_64-efi
KIMG=kernel/arch/x86/boot/bzImage
INITRD_SW=initramfs-switch-x86.cpio.gz
INITRD_FULL=initramfs-full-x86.cpio.gz

[ -f "$MODS/normal.mod" ] || { echo "missing x86_64 grub modules"; exit 1; }
[ -f "$KIMG" ] || { echo "missing $KIMG"; exit 1; }

echo "=== 1. vbox grub.cfg ==="
cat > /tmp/grub-vbox.cfg <<'EOF'
set timeout=3
set default=0
menuentry "BulumOS 1.2 (Linux 7.1.5 x86-64, VirtualBox SATA root)" {
    linux /boot/bzImage console=tty0 root=/dev/sda rw rootfstype=ext4 init=/sbin/init
    initrd /boot/initramfs-switch.cpio.gz
}
menuentry "BulumOS 1.2 - Recovery mode (ramdisk)" {
    linux /boot/bzImage console=tty0 rdinit=/sbin/init recovery
    initrd /boot/initramfs-full.cpio.gz
}
EOF

echo "=== 2. memdisk (FAT) ==="
rm -f /tmp/memdisk-vbox.img
dd if=/dev/zero of=/tmp/memdisk-vbox.img bs=1M count=64 status=none
mkfs.vfat -n MEMDISK /tmp/memdisk-vbox.img >/dev/null
mmd -i /tmp/memdisk-vbox.img ::/boot ::/boot/grub
mcopy -i /tmp/memdisk-vbox.img /tmp/grub-vbox.cfg ::/boot/grub/grub.cfg
mcopy -i /tmp/memdisk-vbox.img "$KIMG" ::/boot/bzImage
mcopy -i /tmp/memdisk-vbox.img "$INITRD_SW" ::/boot/initramfs-switch.cpio.gz
mcopy -i /tmp/memdisk-vbox.img "$INITRD_FULL" ::/boot/initramfs-full.cpio.gz
mdir -i /tmp/memdisk-vbox.img ::/boot

echo "=== 3. BOOTX64.EFI ==="
rm -rf iso-vbox
mkdir -p iso-vbox/EFI/BOOT
"$MKIMAGE" -O x86_64-efi -d "$MODS" \
  -m /tmp/memdisk-vbox.img \
  -o iso-vbox/EFI/BOOT/BOOTX64.EFI \
  fat archelp cpio tar memdisk normal linux iso9660 ext2 search configfile \
  terminal efi_gop echo ls cat test all_video boot minicmd

echo "=== 4. ESP partition + hybrid ISO ==="
rm -f /tmp/efiboot-vbox.img bulumos-vbox.iso
dd if=/dev/zero of=/tmp/efiboot-vbox.img bs=1M count=72 status=none
mkfs.vfat -n BULUMOS /tmp/efiboot-vbox.img >/dev/null
mmd -i /tmp/efiboot-vbox.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/efiboot-vbox.img iso-vbox/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
xorriso -as mkisofs -o bulumos-vbox.iso \
  -V BULUMOS -J -R \
  -e --interval:appended_partition_2:all:: \
  -no-emul-boot \
  -append_partition 2 0xef /tmp/efiboot-vbox.img \
  -isohybrid-gpt-basdat \
  iso-vbox 2>&1 | tail -1

echo "=== done ==="
ls -la bulumos-vbox.iso
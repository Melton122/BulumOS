#!/bin/bash
set -e
MKIMAGE=/usr/bin/grub-mkimage
MODS=~/grub-arm64/extracted/usr/lib/grub/arm64-efi
SYSROOT=/home/melton/myos

echo "=== 1. build grub.cfg ==="
cat > /tmp/grub.cfg <<'EOF'
set timeout=5
set default=0
menuentry "BulumOS 1.2 (Linux 7.1.5 aarch64, persistent root)" {
    linux /boot/Image console=tty0 console=ttyAMA0 root=/dev/vdb rw rootfstype=ext4 init=/sbin/init
    initrd /boot/initramfs-switch.cpio.gz
}
menuentry "BulumOS 1.2 - Recovery mode (ramdisk)" {
    linux /boot/Image console=ttyAMA0 rdinit=/sbin/init recovery
    initrd /boot/initramfs-full.cpio.gz
}
EOF

echo "=== 2. assemble memdisk as FAT (kernel + initramfs + grub.cfg) ==="
rm -f /tmp/memdisk.img
dd if=/dev/zero of=/tmp/memdisk.img bs=1M count=64 status=none
mkfs.vfat -n MEMDISK /tmp/memdisk.img >/dev/null
mmd -i /tmp/memdisk.img ::/boot ::/boot/grub
mcopy -i /tmp/memdisk.img /tmp/grub.cfg ::/boot/grub/grub.cfg
mcopy -i /tmp/memdisk.img "$SYSROOT/kernel/arch/arm64/boot/Image" ::/boot/Image
mcopy -i /tmp/memdisk.img "$SYSROOT/initramfs-switch.cpio.gz" ::/boot/initramfs-switch.cpio.gz
mcopy -i /tmp/memdisk.img "$SYSROOT/initramfs.cpio.gz" ::/boot/initramfs-full.cpio.gz
mdir -i /tmp/memdisk.img ::/boot

echo "=== 3. build BOOTAA64.EFI (FAT memdisk, no --config) ==="
rm -rf "$SYSROOT/iso"
mkdir -p "$SYSROOT/iso/EFI/BOOT"
"$MKIMAGE" -O arm64-efi -d "$MODS" \
  -m /tmp/memdisk.img \
  -o "$SYSROOT/iso/EFI/BOOT/BOOTAA64.EFI" \
  fat archelp cpio tar memdisk normal linux iso9660 ext2 search configfile \
  terminal efi_gop echo ls cat test all_video boot minicmd
file "$SYSROOT/iso/EFI/BOOT/BOOTAA64.EFI"
ls -la "$SYSROOT/iso/EFI/BOOT/"
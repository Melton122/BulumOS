#!/bin/bash
# BulumOS UEFI build: GRUB bootloader + ESP image + hybrid ISO
# Requires: grub-mkimage (host), arm64-efi GRUB modules in ~/grub-arm64/extracted,
#           dosfstools+mtools, xorriso, kernel Image + initramfs.cpio.gz in repo.
set -e
cd /home/melton/myos

MKIMAGE=/usr/bin/grub-mkimage
MODS=~/grub-arm64/extracted/usr/lib/grub/arm64-efi
[ -f "$MODS/normal.mod" ] || { echo "missing arm64 GRUB modules in $MODS"; exit 1; }
[ -f kernel/arch/arm64/boot/Image ] || { echo "kernel Image not built"; exit 1; }
[ -f initramfs.cpio.gz ] || { echo "initramfs.cpio.gz missing"; exit 1; }

echo "=== 1. grub.cfg ==="
cat > /tmp/grub.cfg <<'EOF'
set timeout=3
set default=0
menuentry "BulumOS 1.0 (Linux 7.1.5 aarch64)" {
    linux /boot/Image console=ttyAMA0 rdinit=/sbin/init
    initrd /boot/initramfs.cpio.gz
}
menuentry "BulumOS 1.0 - Recovery mode" {
    linux /boot/Image console=ttyAMA0 rdinit=/sbin/init recovery
    initrd /boot/initramfs.cpio.gz
}
EOF

echo "=== 2. memdisk as FAT (kernel + initramfs + grub.cfg) ==="
rm -f /tmp/memdisk.img
dd if=/dev/zero of=/tmp/memdisk.img bs=1M count=58 status=none
mkfs.vfat -n MEMDISK /tmp/memdisk.img >/dev/null
mmd -i /tmp/memdisk.img ::/boot ::/boot/grub
mcopy -i /tmp/memdisk.img /tmp/grub.cfg ::/boot/grub/grub.cfg
mcopy -i /tmp/memdisk.img kernel/arch/arm64/boot/Image ::/boot/Image
mcopy -i /tmp/memdisk.img initramfs.cpio.gz ::/boot/initramfs.cpio.gz

echo "=== 3. BOOTAA64.EFI (GRUB core with FAT memdisk) ==="
rm -rf iso
mkdir -p iso/EFI/BOOT
"$MKIMAGE" -O arm64-efi -d "$MODS" \
  -m /tmp/memdisk.img \
  -o iso/EFI/BOOT/BOOTAA64.EFI \
  fat archelp cpio tar memdisk normal linux iso9660 ext2 search configfile \
  terminal efi_gop echo ls cat test all_video boot minicmd
file iso/EFI/BOOT/BOOTAA64.EFI

echo "=== 4. ESP image (whole-disk FAT: documented AAVMF boot path) ==="
rm -f /tmp/efiboot.img
dd if=/dev/zero of=/tmp/efiboot.img bs=1M count=72 status=none
mkfs.vfat -n BULUMOS /tmp/efiboot.img >/dev/null
mmd -i /tmp/efiboot.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/efiboot.img iso/EFI/BOOT/BOOTAA64.EFI ::/EFI/BOOT/
cp /tmp/efiboot.img bulumos-uefi.img

echo "=== 5. hybrid ISO (El Torito + GPT ESP) ==="
xorriso -as mkisofs -o bulumos.iso \
  -V BULUMOS -J -R \
  -e --interval:appended_partition_2:all:: \
  -no-emul-boot \
  -append_partition 2 0xef /tmp/efiboot.img \
  -isohybrid-gpt-basdat \
  iso 2>&1 | tail -2

echo "=== done ==="
ls -la bulumos-uefi.img bulumos.iso
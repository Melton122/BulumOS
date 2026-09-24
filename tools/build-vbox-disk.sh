#!/bin/bash
set -euo pipefail
cd /home/melton/myos
command -v parted >/dev/null || command -v sgdisk >/dev/null || { echo "NO parted/sgdisk"; exit 1; }
command -v parted >/dev/null && P=parted || P=sgdisk

echo "=== 0. vbox grub.cfg (root=/dev/sda2) ==="
if [ "${VBOX_SERIAL:-0}" = "1" ]; then
    C1="console=tty0 console=ttyS0,115200n8"
else
    C1="console=tty0"
fi
cat > /tmp/grub-vbox.cfg <<EOF
set timeout=3
set default=0
menuentry "BulumOS 1.2 (Linux 7.1.5 x86-64, VirtualBox root /dev/sda2)" {
    linux /boot/bzImage $C1 root=/dev/sda2 rw rootfstype=ext4 init=/sbin/init
    initrd /boot/initramfs-switch.cpio.gz
}
menuentry "BulumOS 1.2 - Recovery mode (ramdisk)" {
    linux /boot/bzImage $C1 rdinit=/sbin/init recovery
    initrd /boot/initramfs-full.cpio.gz
}
EOF

echo "=== 1. memdisk ==="
rm -f /tmp/memdisk-vbox.img
dd if=/dev/zero of=/tmp/memdisk-vbox.img bs=1M count=64 status=none
mkfs.vfat -n MEMDISK /tmp/memdisk-vbox.img >/dev/null
mmd -i /tmp/memdisk-vbox.img ::/boot ::/boot/grub
mcopy -i /tmp/memdisk-vbox.img /tmp/grub-vbox.cfg ::/boot/grub/grub.cfg
mcopy -i /tmp/memdisk-vbox.img kernel/arch/x86/boot/bzImage ::/boot/bzImage
mcopy -i /tmp/memdisk-vbox.img initramfs-switch-x86.cpio.gz ::/boot/initramfs-switch.cpio.gz
mcopy -i /tmp/memdisk-vbox.img initramfs-full-x86.cpio.gz ::/boot/initramfs-full.cpio.gz

echo "=== 2. BOOTX64.EFI ==="
rm -rf iso-vbox
mkdir -p iso-vbox/EFI/BOOT
/usr/bin/grub-mkimage -O x86_64-efi -d /usr/lib/grub/x86_64-efi \
  -m /tmp/memdisk-vbox.img -o iso-vbox/EFI/BOOT/BOOTX64.EFI \
  fat archelp cpio tar memdisk normal linux iso9660 ext2 search configfile \
  terminal efi_gop echo ls cat test all_video boot minicmd

echo "=== 3. EFI System Partition (FAT) ==="
rm -f /tmp/efiboot-vbox.img
dd if=/dev/zero of=/tmp/efiboot-vbox.img bs=1M count=72 status=none
mkfs.vfat -n BULUMOS /tmp/efiboot-vbox.img >/dev/null
mmd -i /tmp/efiboot-vbox.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/efiboot-vbox.img iso-vbox/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/

echo "=== 4. hybrid GPT disk: ESP (1-73MiB) + ext4 rootfs (73-384MiB) ==="
rm -f bulumos-vbox.img
truncate -s 384M bulumos-vbox.img
if [ "$P" = parted ]; then
    parted -s bulumos-vbox.img mklabel gpt \
      mkpart ESP fat32 1MiB 73MiB \
      mkpart root ext4 73MiB 100% \
      set 1 esp on
else
    sgdisk -og bulumos-vbox.img
    sgdisk -n 1:2048:149503 -t 1:ef00 bulumos-vbox.img
    sgdisk -n 2:149504:786200 -t 2:8300 bulumos-vbox.img
fi
dd if=/tmp/efiboot-vbox.img of=bulumos-vbox.img bs=1M seek=1 conv=notrunc status=none
dd if=disk-x86.img of=bulumos-vbox.img bs=1M seek=73 conv=notrunc status=none

echo "=== 5. verify ==="
$P -s bulumos-vbox.img print 2>/dev/null | grep -E 'Number|1|2'
ls -la bulumos-vbox.img
echo DONE
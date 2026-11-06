#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
set -e
MKIMAGE=/usr/bin/grub-mkimage
MODS="$HOME/grub-x86_64/extracted/usr/lib/grub/x86_64-efi"
[ -f "$MODS/moddep.lst" ] || MODS=/usr/lib/grub/x86_64-efi
SYSROOT="$ROOT_DIR"

echo "=== 1. build grub.cfg ==="
cat > /tmp/grub.cfg <<'EOF'
set timeout=5
set default=0
menuentry "BulumOS 1.2 (Linux 7.1.5 x86_64, persistent root)" {
    linux /boot/bzImage console=tty0 root=/dev/vdb rw rootfstype=ext4 init=/sbin/init
    initrd /boot/initramfs-switch.cpio.gz
}
menuentry "BulumOS 1.2 - Recovery mode (ramdisk)" {
    linux /boot/bzImage console=tty0 rdinit=/sbin/init recovery
    initrd /boot/initramfs-full.cpio.gz
}
EOF

echo "=== 2. assemble memdisk as FAT (kernel + initramfs + grub.cfg) ==="
rm -f /tmp/memdisk.img
dd if=/dev/zero of=/tmp/memdisk.img bs=1M count=64 status=none
mkfs.vfat -n MEMDISK /tmp/memdisk.img >/dev/null
mmd -i /tmp/memdisk.img ::/boot ::/boot/grub
mcopy -i /tmp/memdisk.img /tmp/grub.cfg ::/boot/grub/grub.cfg
# x86_64 kernel + x86_64 userspace initramfses: initramfs.cpio.gz (rootfs/, arm64
# userspace) would fail with exec format error under this kernel.
mcopy -i /tmp/memdisk.img "$SYSROOT/kernel/arch/x86/boot/bzImage" ::/boot/bzImage
mcopy -i /tmp/memdisk.img "$SYSROOT/initramfs-switch-x86.cpio.gz" ::/boot/initramfs-switch.cpio.gz
mcopy -i /tmp/memdisk.img "$SYSROOT/initramfs-full-x86.cpio.gz" ::/boot/initramfs-full.cpio.gz
mdir -i /tmp/memdisk.img ::/boot

echo "=== 3. build BOOTX64.EFI (FAT memdisk, no --config) ==="
rm -rf "$SYSROOT/iso"
mkdir -p "$SYSROOT/iso/EFI/BOOT"
"$MKIMAGE" -O x86_64-efi -d "$MODS" \
  -m /tmp/memdisk.img \
  -o "$SYSROOT/iso/EFI/BOOT/BOOTX64.EFI" \
  fat archelp cpio tar memdisk normal linux iso9660 ext2 search configfile \
  terminal efi_gop echo ls cat test all_video boot minicmd
file "$SYSROOT/iso/EFI/BOOT/BOOTX64.EFI"
ls -la "$SYSROOT/iso/EFI/BOOT/"

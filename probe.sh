#!/bin/bash
for t in xorriso genisoimage grub-mkrescue grub-mkstandalone grub-install limine qemu-img fbset fbgrab; do
  if command -v "$t" >/dev/null 2>&1; then echo "FOUND: $t"; fi
done
echo "--- OVMF/AAVMF:"
ls /usr/share/OVMF* /usr/share/AAVMF* 2>/dev/null
echo "--- EFI packages:"
dpkg -l 2>/dev/null | grep -iE 'grub-efi|ovmf|qemu-efi|edk2' | head -5
echo "--- init/system pkgs:"
dpkg -l 2>/dev/null | grep -E '^ii' | awk '{print $2}' | grep -iE 'openrc|runit|s6-|sysv|daemon|procps|util-linux|psmisc' | head -12
echo "--- host tools:"
ls /home/melton/myos/rootfs/sbin/init /home/melton/myos/rootfs/bin/syslogd /home/melton/myos/rootfs/bin/klogd /home/melton/myos/rootfs/bin/poweroff 2>/dev/null
echo "--- gcc cross:"
command -v aarch64-linux-gnu-gcc

#!/bin/bash
echo "== x86_64 grub modules =="
ls /usr/lib/grub/x86_64-efi/normal.mod 2>&1
dpkg -l grub-efi-amd64-bin 2>/dev/null | tail -1
echo "== tools =="
for t in mkfs.vfat mmd mcopy xorriso; do command -v "$t" >/dev/null && echo "$t ok" || echo "$t MISSING"; done
echo "== config bits =="
grep -E "CONFIG_LOCALVERSION=|CONFIG_EFI_STUB=|CONFIG_EFI_GENERIC_STUB=" ~/BulumOS/kernel/.config
echo "== busybox config =="
ls -la ~/BulumOS/kernel/busybox/.config 2>&1 | head -1
ls -la ~/BulumOS/rootfs/bin/busybox 2>&1
echo "== populate-disk head =="
head -30 ~/BulumOS/tools/populate-disk.sh 2>&1
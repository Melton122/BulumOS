#!/bin/bash
cd /home/melton/myos
echo "== ESP bytes =="
dd if=bulumos-vbox.img of=/tmp/esp1.fat bs=1M skip=1 count=72 status=none
mcopy -i /tmp/esp1.fat ::/EFI/BOOT/BOOTX64.EFI /tmp/out.efi && ls -la /tmp/out.efi
echo "== rootfs bytes =="
dd if=bulumos-vbox.img of=/tmp/root2.ext4 bs=1M skip=73 count=256 status=none
debugfs -R 'stat /sbin/bctl' /tmp/root2.ext4 2>&1 | head -3
debugfs -R 'stat /usr/sbin/dropbear' /tmp/root2.ext4 2>&1 | head -3
echo "== ext4 integrity =="
e2fsck -f -n /tmp/root2.ext4 2>&1 | tail -3
echo DONE
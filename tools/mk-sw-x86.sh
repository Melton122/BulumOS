#!/bin/bash
sed -e 's|rootfs/|rootfs-x86/|' -e 's|OUT=initramfs-switch.cpio.gz|OUT=initramfs-switch-x86.cpio.gz|' /home/melton/BulumOS/build-switch-initramfs.sh > /tmp/sw-x86.sh
bash /tmp/sw-x86.sh 2>&1 | tail -1
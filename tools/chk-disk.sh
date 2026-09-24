#!/bin/bash
cd /home/melton/BulumOS
echo "== rootfs-x86 dropbear + loader =="
ls -la rootfs-x86/usr/sbin/dropbear rootfs-x86/lib64/ld-linux-x86-64.so.2 2>&1
echo "== in disk-x86.img (debugfs) =="
debugfs -R "stat /usr/sbin/dropbear" disk-x86.img 2>&1 | head -3
debugfs -R "stat /lib64/ld-linux-x86-64.so.2" disk-x86.img 2>&1 | head -3
echo "== ls of lib64 on disk =="
debugfs -R "ls -l /lib64" disk-x86.img 2>&1 | head -5
debugfs -R "ls /usr/sbin" disk-x86.img 2>&1 | head -20
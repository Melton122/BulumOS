#!/bin/bash
cd /home/melton/BulumOS
echo "== rootfs ELF binaries (non-busybox) with arch =="
find rootfs -type f -exec file {} \; 2>/dev/null | grep -E 'ELF' | grep -v 'busybox:' | sort > /tmp/rootfs-elf.txt
cat /tmp/rootfs-elf.txt
echo
echo "== rootfs-x86 ELF binaries with arch =="
find rootfs-x86 -type f -exec file {} \; 2>/dev/null | grep -E 'ELF' | grep -v 'busybox:' | sort
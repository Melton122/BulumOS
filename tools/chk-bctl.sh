#!/bin/bash
cd /home/melton/BulumOS
echo "== where is bctl built? =="
grep -rn "bctl" tools/install-services.sh rebuild-userspace.sh 2>/dev/null | head -5
echo "== rootfs/sbin =="
ls -la rootfs/sbin/
echo "== arch of key rootfs binaries (source of truth) =="
for b in rootfs/sbin/bctl rootfs/sbin/init rootfs/sbin/bulusomething; do
  [ -f "$b" ] && file "$b"
done
file rootfs/sbin/init
echo "== arch of copied rootfs-x86 binaries =="
for b in rootfs-x86/sbin/bctl rootfs-x86/sbin/init rootfs-x86/usr/bin/bdesktop rootfs-x86/usr/bin/bgfx; do
  [ -f "$b" ] && file "$b"
done
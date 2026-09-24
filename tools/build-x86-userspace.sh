#!/bin/bash
# Build x86_64 userspace: rootfs-x86 + x86 initramfses + x86 disk image.
# Leaves the arm64 (default) artifacts untouched. Run from any cwd.
set -e
cd /home/melton/myos

RFS=rootfs-x86
BB=/tmp/bb-x86.busybox
CC=gcc

echo "=== 1. clone rootfs -> rootfs-x86 ==="
rm -rf "$RFS"
mkdir -p "$RFS"
cd rootfs
find . -mindepth 1 ! -path './dev' ! -path './dev/*' -exec cp -a --parents {} "/home/melton/myos/$RFS/" \;
cd /home/melton/myos
mkdir -p "$RFS/dev"
cp "$BB" "$RFS/bin/busybox"
chmod 755 "$RFS/bin/busybox"

echo "=== 2. compile x86_64 binaries ==="
# init
gcc -static -O2 -Wall -s -o "$RFS/sbin/init" src/init.c 2>&1 | grep -E 'error|warning' | head -5 || true
chmod 755 "$RFS/sbin/init"
for item in "sbin/bstatus src/bstatus.c" "sbin/blog src/blog.c" "sbin/bctl src/bctl.c" \
            "sbin/bulum src/bulum.c src/bulum_crypto.c" \
            "usr/bin/bterm src/bterm.c" "bin/bsh src/bsh.c" \
            "usr/bin/bnet src/bnet.c" "usr/bin/bsecure src/bsecure.c" "usr/bin/bfirewall src/bfirewall.c" \
            "usr/bin/busb src/busb.c" "usr/bin/buser src/buser.c" "usr/bin/btime src/btime.c"; do
    out=$(echo "$item" | awk '{print $1}')
    rest=$(echo "$item" | cut -d' ' -f2-)
    gcc -static -O2 -s -o "$RFS/$out" $rest
done
gcc -static -O2 -s -o "$RFS/usr/bin/bgfxinfo" src/bgfxinfo.c src/bgfx.c
gcc -static -O2 -s -o "$RFS/usr/bin/bgfxdemo" src/bgfxdemo.c src/bgfx.c
gcc -static -O2 -s -o "$RFS/usr/bin/binputtest" src/binputtest.c src/binput.c
gcc -static -O2 -s -o "$RFS/usr/bin/bgfx" src/bgfxcli.c src/bgfx.c src/binput.c
gcc -static -O2 -s -o "$RFS/usr/bin/bdesktop" \
    src/bdesktop.c src/bwm.c src/bshell.c \
    src/bgfx.c src/binput.c src/bwindow.c src/btheme.c \
    src/bapp.c src/bnotify.c src/bicon.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c src/bapp_hardware.c src/bhw.c
chmod 755 "$RFS/usr/bin/bgfxinfo" "$RFS/usr/bin/bgfxdemo" "$RFS/usr/bin/binputtest" "$RFS/usr/bin/bgfx" "$RFS/usr/bin/bdesktop"
for b in "$RFS/bin/bsh" "$RFS/usr/bin/bnet" "$RFS/usr/bin/bsecure" "$RFS/usr/bin/bfirewall" \
         "$RFS/usr/bin/busb" "$RFS/usr/bin/buser" "$RFS/usr/bin/btime" "$RFS/sbin/bstatus" \
         "$RFS/sbin/blog" "$RFS/sbin/bctl" "$RFS/sbin/bulum" "$RFS/usr/bin/bterm"; do
    chmod 755 "$b"
done

gcc -static -O2 -s -o "$RFS/usr/bin/bnetwork" src/bnetwork.c src/bhw.c
gcc -static -O2 -s -o "$RFS/usr/bin/bpower" src/bpower.c src/bhw.c
gcc -static -O2 -s -o "$RFS/usr/bin/bhardware" src/bhardware.c src/bhw.c
gcc -static -O2 -s -o "$RFS/usr/bin/bdevices" src/bdevices.c src/bhw.c
gcc -static -O2 -s -o "$RFS/usr/bin/bdriver" src/bdriver.c src/bhw.c
chmod 755 "$RFS/usr/bin/bhardware" "$RFS/usr/bin/bdevices" "$RFS/usr/bin/bdriver" "$RFS/usr/bin/bnetwork" "$RFS/usr/bin/bpower"
ln -sf /sbin/bulum "$RFS/sbin/bpk"

echo "=== 3. x86 nss libs ==="
if ls /lib/x86_64-linux-gnu/libnss_dns.so.2 >/dev/null 2>&1; then
    cp /lib/x86_64-linux-gnu/libnss_dns.so.2 /lib/x86_64-linux-gnu/libnss_files.so.2 "$RFS/lib/"
fi
chmod 755 "$RFS"/lib/libnss_*.so.2 2>/dev/null || true

echo "=== 4. services (arch-independent, into rootfs-x86) ==="
sed 's|myos/rootfs|myos/rootfs-x86|' tools/install-services.sh > /tmp/install-services-x86.sh
bash /tmp/install-services-x86.sh 2>&1 | tail -2

echo "=== 4.5 Phase 12: seeded package db (x86_64) ==="
bash tools/seed-rootfs-db.sh x86_64

echo "=== 5. full initramfs (x86) ==="
cd "$RFS"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > /home/melton/myos/initramfs-full-x86.cpio.gz
cd /home/melton/myos
gzip -t initramfs-full-x86.cpio.gz && echo "full x86 initramfs OK: $(du -h initramfs-full-x86.cpio.gz | cut -f1)"

echo "=== 6. switch initramfs (x86) ==="
sed -e 's|rootfs/|rootfs-x86/|' -e 's|OUT=initramfs-switch.cpio.gz|OUT=initramfs-switch-x86.cpio.gz|' build-switch-initramfs.sh > /tmp/sw-x86.sh
bash /tmp/sw-x86.sh 2>&1 | tail -1

echo "=== 7. disk image (x86) ==="
SRC="$RFS" IMG=disk-x86.img bash tools/populate-disk.sh 2>&1 | grep -E 'creating|OK|error' | head -5

echo "=== x86 userspace done ==="
ls -la initramfs-full-x86.cpio.gz initramfs-switch-x86.cpio.gz disk-x86.img 2>&1
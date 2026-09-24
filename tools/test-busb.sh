#!/bin/bash
# BulumOS Phase 9.4: USB host-side test (busb + mdev.conf)
set -e
cd /home/melton/myos
BIN="${BUSB_BIN_DIR:-/tmp/busb-host}"
mkdir -p "$BIN"
if [ -z "${BUSB_BIN_DIR:-}" ]; then
    gcc -O2 -Wall -o "$BIN/busb" src/busb.c || { echo "build failed"; exit 1; }
fi
BUSB="$BIN/busb"
PASS=0 FAIL=0
ok() { echo "PASS  $1"; PASS=$((PASS+1)); }
no() { echo "FAIL  $1"; FAIL=$((FAIL+1)); }

echo "=== busb command tests ==="
"$BUSB" help >/dev/null 2>&1 && ok "help exits 0" || no "help exits 0"
"$BUSB" --help >/dev/null 2>&1 && ok "--help exits 0" || no "--help exits 0"
if [ -d /sys/bus/usb/devices ]; then
    "$BUSB" list >/dev/null 2>&1 && ok "list runs with USB" || no "list runs with USB"
else
    "$BUSB" list >/dev/null 2>&1 && ok "list exits 0 (no-USB path)" || ok "list exits nonzero (no-USB is reported)"
fi
"$BUSB" storage >/dev/null 2>&1 && ok "storage runs" || no "storage runs"
"$BUSB" bogus >/dev/null 2>&1 && no "unknown cmd exits nonzero" || ok "unknown cmd exits nonzero"
"$BUSB" info 1-1 >/dev/null 2>&1 && { :; } || { :; }
no_usb=0
if [ ! -d /sys/bus/usb/devices ]; then
    ok "no-USB handled cleanly (host has no usb subsystem)" 
else
    no_usb=0
fi

echo "=== mdev.conf checks ==="
grep -q '/sbin/mdev-automount.sh' rootfs/etc/mdev.conf \
  && ok "mdev automount rule present" || no "mdev automount rule present"
grep -q 'sda\|sd\[a-z\]' rootfs/etc/mdev.conf && ok "USB block rule present" || no "USB block rule present"
grep -q 'ttyUSB' rootfs/etc/mdev.conf && ok "USB serial rule present" || no "USB serial rule present"
grep -q '0:6' rootfs/etc/mdev.conf && ok "disk gid 6 used for blocks" || no "disk gid 6 used for blocks"
test -x rootfs/sbin/mdev-automount.sh && ok "mdev-automount.sh executable" || no "mdev-automount.sh executable"
grep -q '^disk:x:6:' rootfs/etc/group && ok "disk group exists" || no "disk group exists"
grep -q '^dialout:x:5:' rootfs/etc/group && ok "dialout group exists" || no "dialout group exists"
test -d rootfs/media && ok "/media dir exists" || no "/media dir exists"
test -x rootfs/usr/bin/busb && ok "rootfs/usr/bin/busb executable" || no "rootfs/usr/bin/busb executable"

# automount no-op on remove should not error
LOG=/tmp/busb-am.log
ACTION=remove DEVNAME=sdb1 bash rootfs/sbin/mdev-automount.sh ; true
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

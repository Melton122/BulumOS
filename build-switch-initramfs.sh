#!/bin/bash
# BulumOS: build the tiny switch_root initramfs used for the default disk boot.
# Content: /init script + static busybox with applet symlinks.
set -e
cd /home/melton/myos

OUT=initramfs-switch.cpio.gz
WORK=/tmp/ir-sw

rm -rf "$WORK"
mkdir -p "$WORK/bin"

cp rootfs/bin/busybox "$WORK/bin/busybox"
chmod 755 "$WORK/bin/busybox"
for a in sh mount mkdir sleep switch_root cat echo mountpoint; do
    ln -s busybox "$WORK/bin/$a"
done

cat > "$WORK/init" <<'EOF'
#!/bin/sh
# BulumOS initramfs: find the root disk, mount it, switch_root.
PATH=/bin
echo "SW: initramfs init start" >/dev/console 2>&1
echo "SW: initramfs init start" >/dev/ttyS0 2>&1
/bin/mkdir -p /proc /sys /dev /newroot
/bin/mount -t proc proc /proc
/bin/mount -t sysfs sysfs /sys
/bin/mount -t devtmpfs devtmpfs /dev
/bin/mkdir -p /dev/pts
echo "SW: mounts done" >/dev/console 2>&1
echo "SW: mounts done" >/dev/ttyS0 2>&1

ROOT=/dev/vdb
ROOTFS=ext4
/bin/sleep 1

attempt=0
while [ $attempt -lt 10 ]; do
    if /bin/mount -t "$ROOTFS" "$ROOT" /newroot 2>/dev/null; then
        break
    fi
    attempt=$((attempt + 1))
    /bin/sleep 1
done
if ! /bin/mountpoint -q /newroot 2>/dev/null; then
    echo "!! BulumOS: cannot mount $ROOT, dropping to emergency shell"
    exec /bin/sh
fi

echo "BulumOS: root at $ROOT, switching root..." >/dev/console 2>&1
echo "BulumOS: root at $ROOT, switching root..." >/dev/ttyS0 2>&1
exec /bin/switch_root /newroot /sbin/init
EOF
chmod 755 "$WORK/init"

cd "$WORK"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "/home/melton/myos/$OUT"
cd /home/melton/myos
gzip -t "$OUT" && echo "initramfs-switch OK: $(du -h $OUT | cut -f1)"
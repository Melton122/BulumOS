#!/bin/bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE="${BULUMOS_BUILD_DIR:-$ROOT/out/plasma-x86}"
OUTPUT="${BULUMOS_IMAGE_DIR:-$ROOT/out/plasma-x86-image}"
WORK=

if [[ "$EUID" -ne 0 ]]; then
    echo "Run the Plasma image build as root." >&2
    exit 1
fi
for tool in cpio gzip mkfs.ext4 truncate; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done
for path in \
    "$STAGE/sbin/init" \
    "$STAGE/bin/busybox" \
    "$STAGE/usr/bin/plasmashell" \
    "$ROOT/kernel/arch/x86/boot/bzImage"
do
    [[ -e "$path" ]] || {
        echo "Required Plasma image input is missing: $path" >&2
        exit 1
    }
done
DBUS_PIDFILE="$STAGE/run/dbus/pid"
if [[ -f "$DBUS_PIDFILE" ]]; then
    dbus_pid="$(< "$DBUS_PIDFILE")"
    if [[ "$dbus_pid" =~ ^[0-9]+$ ]] &&
        [[ -r "/proc/$dbus_pid/cmdline" ]] &&
        tr '\0' ' ' < "/proc/$dbus_pid/cmdline" | grep -q 'dbus-daemon'; then
        echo "A D-Bus daemon is still running for the staged root: PID $dbus_pid" >&2
        exit 1
    fi
    echo "Removing stale staged D-Bus pid file."
    rm -f -- "$DBUS_PIDFILE"
fi
for file in root.img initramfs.cpio.gz bzImage; do
    if [[ -e "$OUTPUT/$file" ]]; then
        echo "Refusing to overwrite existing image output: $OUTPUT/$file" >&2
        exit 1
    fi
done

mkdir -p "$OUTPUT"
WORK="$(mktemp -d "$OUTPUT/.build.XXXXXX")"
cleanup() {
    if [[ -n "$WORK" ]]; then
        rm -rf -- "$WORK"
    fi
}
trap cleanup EXIT

echo "== Building isolated Plasma root image =="
truncate -s 4G "$WORK/root.img"
mkfs.ext4 -F -m 0 -L BULUMROOT -d "$STAGE" "$WORK/root.img"

mkdir -p "$WORK/initrd/bin"
install -m 755 "$STAGE/bin/busybox" "$WORK/initrd/bin/busybox"
for applet in sh mount mkdir sleep switch_root cat echo mountpoint; do
    ln -s busybox "$WORK/initrd/bin/$applet"
done
cat > "$WORK/initrd/init" <<'EOF'
#!/bin/sh
PATH=/bin
echo "BulumOS initramfs starting" >/dev/console
/bin/mkdir -p /proc /sys /dev /newroot
/bin/mount -t proc proc /proc
/bin/mount -t sysfs sysfs /sys
/bin/mount -t devtmpfs devtmpfs /dev
/bin/sleep 1
attempt=0
while [ "$attempt" -lt 10 ]; do
    if /bin/mount -t ext4 /dev/vdb /newroot 2>/dev/null; then
        break
    fi
    attempt=$((attempt + 1))
    /bin/sleep 1
done
if ! /bin/mountpoint -q /newroot 2>/dev/null; then
    echo "BulumOS: cannot mount /dev/vdb; emergency shell" >/dev/console
    exec /bin/sh
fi
exec /bin/switch_root /newroot /sbin/init
EOF
chmod 755 "$WORK/initrd/init"
(
    cd "$WORK/initrd"
    find . -print0 | cpio --null -o --format=newc 2>/dev/null |
        gzip -9 > "$WORK/initramfs.cpio.gz"
)

cp "$ROOT/kernel/arch/x86/boot/bzImage" "$WORK/bzImage"
mv "$WORK/root.img" "$OUTPUT/root.img"
mv "$WORK/initramfs.cpio.gz" "$OUTPUT/initramfs.cpio.gz"
mv "$WORK/bzImage" "$OUTPUT/bzImage"
printf 'Plasma x86-64 image\nRoot disk: root.img (ext4; attach as /dev/vdb)\nBoot: bzImage + initramfs.cpio.gz\n' \
    > "$OUTPUT/README.txt"
echo "== Bootable Plasma image ready: $OUTPUT =="
ls -lh "$OUTPUT/root.img" "$OUTPUT/initramfs.cpio.gz" "$OUTPUT/bzImage"

#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# BulumOS: build a bootable ext4 root disk from a rootfs tree.
#
# Usage:
#   tools/populate-disk.sh [x86|arm64]
#
# Environment overrides:
#   SRC     rootfs tree to package (default depends on arch)
#   IMG     output disk image (default depends on arch)
#   SIZE_M  fixed size in MiB (default: auto-size from SRC plus slack)
#
# Unlike the old debugfs-based writer this uses mke2fs -d, which preserves
# ownership, modes, timestamps, hardlinks and symlinks, and is much faster.
set -euo pipefail
cd "$ROOT_DIR"

ARCH="${1:-}"
case "$ARCH" in
    x86|x86_64|amd64)
        IMG=${IMG:-disk-x86.img}
        SRC=${SRC:-rootfs-x86}
        ;;
    arm64|aarch64)
        IMG=${IMG:-disk.img}
        SRC=${SRC:-rootfs}
        ;;
    "")
        IMG=${IMG:-disk.img}
        SRC=${SRC:-rootfs}
        ;;
    -h|--help)
        sed -n '2,16p' "$0"
        exit 0
        ;;
    *)
        echo "usage: $0 [x86|arm64]" >&2
        exit 2
        ;;
esac

[ -d "$SRC" ] || { echo "rootfs not found: $SRC" >&2; exit 1; }
[ -x "$SRC/bin/busybox" ] || { echo "rootfs not built (missing $SRC/bin/busybox)" >&2; exit 1; }
[ -x "$SRC/sbin/init" ] || { echo "rootfs not built (missing $SRC/sbin/init)" >&2; exit 1; }

if [ -z "${SIZE_M:-}" ]; then
    SRC_M=$(du -sm "$SRC" | cut -f1)
    NEED_M=$((SRC_M + 192))
    SIZE_M=$(( (NEED_M + 63) / 64 * 64 ))
    [ "$SIZE_M" -lt 256 ] && SIZE_M=256
fi

echo "=== packaging $SRC -> $IMG (${SIZE_M}M) ==="
rm -f "$IMG"
truncate -s "${SIZE_M}M" "$IMG"

# -d reads the tree directly; run as root in the build container so uids/gids
# are recorded exactly as in the source rootfs.
mkfs.ext4 -q -F -t ext4 -L BULUMROOT -m 0 -d "$SRC" "$IMG"

echo "=== verify ==="
e2fsck -fn "$IMG" >/dev/null 2>&1 || echo "WARN: e2fsck reported issues"
for path in /sbin/init /bin/busybox /bin/sh /etc/passwd; do
    if ! debugfs -R "stat $path" "$IMG" >/dev/null 2>&1; then
        echo "ERROR: missing $path in $IMG" >&2
        exit 1
    fi
done
if [ -f "$SRC/lib64/ld-linux-x86-64.so.2" ] || [ -f "$SRC/usr/lib64/ld-linux-x86-64.so.2" ]; then
    debugfs -R "stat /lib64/ld-linux-x86-64.so.2" "$IMG" >/dev/null 2>&1 || {
        echo "ERROR: missing x86-64 dynamic loader in $IMG" >&2
        exit 1
    }
fi
debugfs -R "stat /tmp" "$IMG" 2>/dev/null | grep -q 'Mode:.*1777' || \
    echo "WARN: /tmp is not mode 1777"

echo "=== done: $IMG ($(du -h "$IMG" | cut -f1)) ==="

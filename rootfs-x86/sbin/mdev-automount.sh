#!/bin/sh
# BulumOS USB auto-mount handler (called by mdev on sd* hotplug).
#   - on ADD of sd[a-z][0-9]  -> mount to /media/<dev> (if filesystem present)
#   - on REMOVE               -> unmount /media/<dev> and clean up
set -e

ACTION=${ACTION:-}
DEVNAME=${DEVNAME:-}

[ -n "$DEVNAME" ] || exit 0

MEDIA=/media
MP="$MEDIA/$DEVNAME"

mount_one() {
    # skip devices that are not USB/removable-class storage names
    case "$DEVNAME" in
        sd[a-z]|sd[a-z][0-9]*|mmcblk[0-9]*|nvme[0-9]*n[0-9]*) : ;;
        *) exit 0 ;;
    esac
    # skip the whole disk if it already has mounted partitions (avoid double-mount)
    if [ -n "$(mount | grep -E "/media/$DEVNAME([0-9]+)? " 2>/dev/null)" ]; then
        return 0
    fi
    mkdir -p "$MP"
    # try common filesystems read-only first (safe), then rw
    if mount -t vfat /dev/$DEVNAME "$MP" -o rw,noatime 2>/dev/null; then
        echo "mdev-automount: mounted /dev/$DEVNAME -> $MP (vfat)"
        return 0
    fi
    if mount -t ext4 /dev/$DEVNAME "$MP" -o rw,noatime 2>/dev/null; then
        echo "mdev-automount: mounted /dev/$DEVNAME -> $MP (ext4)"
        return 0
    fi
    if mount /dev/$DEVNAME "$MP" -o ro 2>/dev/null; then
        echo "mdev-automount: mounted /dev/$DEVNAME -> $MP (auto, ro)"
        return 0
    fi
    rmdir "$MP" 2>/dev/null || true
    echo "mdev-automount: no filesystem on /dev/$DEVNAME"
    return 0
}

unmount_one() {
    umount "$MP" 2>/dev/null || true
    rmdir "$MP" 2>/dev/null || true
}

case "$ACTION" in
    add|change) mount_one ;;
    remove)     unmount_one ;;
esac
exit 0

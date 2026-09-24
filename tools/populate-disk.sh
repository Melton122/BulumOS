#!/bin/bash
# BulumOS: build disk.img as ext4 root disk containing the full rootfs.
# Uses a debugfs command batch file (fast, no root required).
set -e
cd /home/melton/myos

IMG=${IMG:-disk.img}
SRC=${SRC:-rootfs}
SIZE_M=256

[ -x "$SRC/bin/busybox" ] || { echo "rootfs not built"; exit 1; }

echo "=== creating blank ext4 image (${SIZE_M}M) ==="
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=$SIZE_M status=none
mkfs.ext4 -q -F -L BULUMROOT -m 0 "$IMG"

CMD=$(mktemp /tmp/populate.XXXXXX)
trap 'rm -f $CMD' EXIT

echo "rmdir /lost+found" >> "$CMD"

echo "=== mkdir ==="
find "$SRC" -type d | grep -v '^'"$SRC"'$' | awk '{print length($0), $0}' | sort -n | cut -d' ' -f2- | while read -r d; do
    rel="${d#"$SRC"}"
    echo "mkdir $rel" >> "$CMD"
done

echo "=== device nodes: skipped (devtmpfs creates /dev at boot) ==="

echo "=== symlinks ==="
find "$SRC" -type l | while read -r f; do
    rel="${f#"$SRC"}"
    tgt=$(readlink "$f")
    echo "symlink $rel $tgt" >> "$CMD"
done

echo "=== regular files ==="
find "$SRC" -type f | while read -r f; do
    rel="${f#"$SRC"}"
    mode=$(stat -c %a "$f")
    modehex=$(printf '0x%x' $(( 0100000 | 8#$mode )))
    echo "write $f $rel" >> "$CMD"
    echo "set_inode_field $rel mode $modehex" >> "$CMD"
done

echo "set_inode_field /tmp mode $(printf '0x%x' $((040000 | 8#1777)))" >> "$CMD"

echo "=== ownership fixes ==="
echo "set_inode_field /home uid 0" >> "$CMD"
echo "set_inode_field /home gid 0" >> "$CMD"
echo "set_inode_field /home/melton uid 1000" >> "$CMD"
echo "set_inode_field /home/melton gid 1000" >> "$CMD"
echo "set_inode_field /home/melton mode $(printf '0x%x' $((040000 | 8#755)))" >> "$CMD"
echo "set_inode_field /root mode $(printf '0x%x' $((040000 | 8#700)))" >> "$CMD"
echo "set_inode_field /etc/dropbear uid 0" >> "$CMD"
echo "set_inode_field /etc/dropbear gid 0" >> "$CMD"
echo "set_inode_field /etc/dropbear mode $(printf '0x%x' $((040000 | 8#700)))" >> "$CMD"

echo "=== populating (debugfs) ==="
debugfs -w -f "$CMD" "$IMG" >/dev/null 2>&1

echo "=== verify ==="
debugfs -R "ls /" "$IMG" 2>/dev/null | tr -s ' ' | cut -d' ' -f2 | tr '\n' ' '
echo
debugfs -R "stat /bin/busybox" "$IMG" 2>/dev/null | grep -E 'Type|Mode|User'
debugfs -R "stat /tmp" "$IMG" 2>/dev/null | grep Mode
debugfs -R "stat /bin/sh" "$IMG" 2>/dev/null | grep Type
debugfs -R "stat /sbin/init" "$IMG" 2>/dev/null | grep -E 'Type|Mode'
debugfs -R "stat /tmp" "$IMG" 2>/dev/null | grep Mode
debugfs -R "stat /bin/sh" "$IMG" 2>/dev/null | grep -E 'Type'

echo "=== done: $IMG ($(du -h $IMG | cut -f1)) ==="
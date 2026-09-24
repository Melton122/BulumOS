#!/bin/bash
# regenerate /etc/shadow with fresh hashes for root + melton
set -e
cd /home/melton/myos
RH=$(openssl passwd -6 -salt bulumos1 root)
MH=$(openssl passwd -6 -salt bulumos2 melton)
printf 'root:%s:19786:0:99999:7:::\nmelton:%s:19786:0:99999:7:::\n' "$RH" "$MH" > rootfs/etc/shadow
chmod 600 rootfs/etc/shadow
echo "=== /etc/shadow ==="
cat rootfs/etc/shadow
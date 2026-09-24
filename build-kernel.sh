#!/bin/bash
# BulumOS kernel build.
#
# The kernel Makefile refuses source trees whose path contains spaces
# ("Bulum OS"), so we build from a space-free mirror at /home/melton/ksrc
# (created once by tools/sync-kernel-src.sh) and copy the resulting
# Image and .config back into the repository tree.
set -e
cd /home/melton/myos

[ -d /home/melton/ksrc/arch/arm64 ] || {
    echo "kernel build mirror missing; run: bash tools/sync-kernel-src.sh"
    exit 1
}

echo "=== syncing config ==="
cp kernel/.config /home/melton/ksrc/.config
cd /home/melton/ksrc
echo "=== cleaning ==="
make clean
echo "=== configuring ==="
make -j4 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig 2>&1 | tail -1
echo "=== building Image ==="
make -j4 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image 2>&1 | tail -2

echo "=== copying artifacts back ==="
cp /home/melton/ksrc/.config /home/melton/myos/kernel/.config
cp /home/melton/ksrc/arch/arm64/boot/Image /home/melton/myos/kernel/arch/arm64/boot/Image
echo "kernel Image: $(ls -la /home/melton/myos/kernel/arch/arm64/boot/Image | awk '{print $5}') bytes"
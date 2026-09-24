#!/bin/bash
# Create the space-free kernel build mirror at /home/melton/ksrc.
# The kernel Makefile cannot handle the space in "Bulum OS", so the
# build happens in a rsync mirror (source only, no build artifacts).
# After the first sync, only .config changes are copied in/out.
set -e
cd /home/melton/myos

echo "=== syncing kernel sources to /home/melton/ksrc (source only) ==="
rsync -a \
    --exclude '*.o' --exclude '*.cmd' --exclude '*.a' \
    --exclude '*.mod' --exclude '*.mod.c' --exclude '*.ko' \
    --exclude '*.symvers' --exclude '*.order' \
    --exclude '.config.old' --exclude 'System.map*' \
    --exclude 'arch/arm64/boot' --exclude 'include/config' \
    --exclude '.tmp*' --exclude 'vmlinux' --exclude 'vmlinux.o' \
    kernel/ /home/melton/ksrc/

echo "=== restoring vmlinux.lds.S (build input, not a kernel binary) ==="
cd kernel
find . -name 'vmlinux.lds.S' | while read -r f; do
    mkdir -p "/home/melton/ksrc/$(dirname "$f")"
    cp "$f" "/home/melton/ksrc/$f"
done
cd ..

echo "=== boot dir (Makefiles etc, no Image/dtbs) ==="
rsync -a --exclude '*.o' --exclude '*.cmd' --exclude 'Image' --exclude '*.dtb' \
    kernel/arch/arm64/boot/ /home/melton/ksrc/arch/arm64/boot/

echo "=== config ==="
cp kernel/.config /home/melton/ksrc/.config
echo "mirror ready: $(du -sh /home/melton/ksrc | cut -f1)"
#!/bin/bash
# BulumOS self-test: serial console variant (no window needed).
# Run from /home/melton/myos:  bash boot-serial.sh
# Type directly in this terminal; quit with:  Ctrl+A then X
# After boot:  login root / root, then try the same commands as boot-gfx.sh
# (bgfxinfo, bgfx status, bgfxdemo, binputtest --list, ...)
cd /home/melton/myos || exit 1
pkill -f qemu-system 2>/dev/null
sleep 1
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file="$PWD/bulumos-uefi.img",format=raw,if=virtio \
  -drive file="$PWD/disk.img",format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -nographic
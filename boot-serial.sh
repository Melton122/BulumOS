#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# BulumOS self-test: serial console variant (no window needed).
# Run from "$ROOT_DIR":  bash boot-serial.sh
# Type directly in this terminal; quit with:  Ctrl+A then X
# After boot: log in as root with the password configured during the build.
# Then try the same commands as boot-gfx.sh
# (bgfxinfo, bgfx status, bgfxdemo, binputtest --list, ...)
cd "$ROOT_DIR" || exit 1
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
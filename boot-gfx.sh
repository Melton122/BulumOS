#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# BulumOS self-test: interactive QEMU boot with graphics window (WSLg).
# Run from "$ROOT_DIR":  bash boot-gfx.sh
# After boot: log in as root with the password configured during the build.
# Then:
#   bgfxinfo          -> framebuffer geometry
#   bgfx status       -> backend + mode
#   bgfxdemo          -> animated demo on the window
#   bgfx test         -> known test pattern
#   bgfxdemo --selfcheck   -> golden hash of the rendered frame
#   binputtest --list      -> keyboard + mouse devices
#   binputtest --timeout 5 -> press keys / move mouse, watch events
#   poweroff          -> clean shutdown
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
  -device virtio-gpu-pci \
  -device virtio-keyboard-pci,vectors=3 \
  -device virtio-mouse-pci,vectors=3
#!/bin/bash
# BulumOS UEFI boot with graphics (Phase 8):
#   virtio-gpu-pci        -> /dev/fb0 + /dev/dri/card0 (+renderD128)
#   virtio-keyboard-pci   -> /dev/input/event0
#   virtio-mouse-pci      -> /dev/input/event1
# Serial console stays primary (console=ttyAMA0); the framebuffer is
# an additional display layer.
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci \
  -device virtio-keyboard-pci \
  -device virtio-mouse-pci \
  -nographic > /home/melton/myos/boot-uefi.log 2>&1
#!/bin/bash
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos.iso,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci \
  -nographic > /home/melton/myos/boot-iso.log 2>&1
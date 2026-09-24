#!/bin/bash
# BulumOS QEMU runner (headless: no GPU/input devices, initramfs boot)
# Usage: ./run-qemu.sh [recovery]
# A VM without graphics hardware exercises the headless path:
# "Graphics: unavailable / Console mode: active" must be a normal result.
cd /home/melton/myos
CMD="console=ttyAMA0 rdinit=/sbin/init"
if [ "$1" = "recovery" ]; then
  CMD="$CMD recovery"
fi
qemu-system-aarch64 \
  -M virt -cpu cortex-a72 -m 512M \
  -kernel kernel/arch/arm64/boot/Image \
  -initrd initramfs.cpio.gz \
  -append "$CMD" \
  -drive file=disk.img,if=virtio,format=raw \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -nographic

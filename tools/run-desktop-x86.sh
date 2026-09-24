#!/bin/bash
# Run BulumOS x86-64 in a QEMU window (OVMF, WSLg). Shows the live desktop.
cd /home/melton/BulumOS
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/bulumos-vars4m.fd
pkill -f qemu-system 2>/dev/null || true
sleep 1
echo "Booting BulumOS x86-64 in a window (serial log -> boot-gui-x86.log)..."
qemu-system-x86_64 -M q35 -cpu max -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars4m.fd \
  -drive file=/home/melton/BulumOS/bulumos-uefi-x86.img,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/disk-x86.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2223-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci,xres=1280,yres=800 -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:/tmp/qemu-mon-x86.sock,server,nowait \
  -serial file:/home/melton/BulumOS/boot-gui-x86.log \
  -display gtk
echo "done"
#!/bin/bash
cd /home/melton/BulumOS
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-dbg.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/BulumOS/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:/tmp/qemu-mon.sock,server,nowait \
  -nographic > /home/melton/BulumOS/boot-dbg.log 2>&1"
echo "booted; waiting for boot to settle (~130s), then shutting qemu down..."
sleep 130
pkill -f qemu-system 2>/dev/null || true
echo "DONE"
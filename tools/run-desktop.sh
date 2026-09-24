#!/bin/bash
# Run BulumOS with a VISIBLE GTK window showing the desktop (WSLg).
# The framebuffer (bdesktop) renders in the QEMU window; use the on-screen
# keyboard/mouse to interact with it. Close the window or Ctrl-C to quit.
cd /home/melton/BulumOS
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
pkill -f qemu-system 2>/dev/null || true
sleep 1
echo "Booting BulumOS in a window (serial log -> boot-gui.log)..."
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/BulumOS/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci,xres=1280,yres=800 -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:/tmp/qemu-mon.sock,server,nowait \
  -serial file:/home/melton/BulumOS/boot-gui.log \
  -display gtk
echo "done"
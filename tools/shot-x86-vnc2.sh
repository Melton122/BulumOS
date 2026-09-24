#!/bin/bash
cd /home/melton/BulumOS
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/bulumos-vars4m.fd 2>/dev/null
qemu-system-x86_64 -M q35 -cpu max -m 512M -no-reboot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars4m.fd \
  -drive file=bulumos-uefi-x86.img,format=raw,if=virtio \
  -drive file=disk-x86.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2223-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci,xres=1280,yres=800 \
  -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:/tmp/qemu-mon-x86.sock,server,nowait \
  -serial file:boot-gui-x86.log \
  -display vnc=127.0.0.1:3 &
QPID=$!
sleep 6
if ! kill -0 $QPID 2>/dev/null; then echo "QEMU DIED EARLY"; wait $QPID; fi
echo "--- display info ---"
printf 'info display\r' | timeout 3 socat - UNIX-CONNECT:/tmp/qemu-mon-x86.sock | cat -v
kill $QPID 2>/dev/null
echo DONE
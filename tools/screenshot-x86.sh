#!/bin/bash
# Boot BulumOS x86-64 headless (egl-headless), wait for desktop, screendump.
cd /home/melton/BulumOS
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/bulumos-vars4m.fd
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-gui-x86.log shot-x86.ppm shot-x86.png mon-x86.log
qemu-system-x86_64 -M q35 -cpu max -m 512M -no-reboot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars4m.fd \
  -drive file=/home/melton/BulumOS/bulumos-uefi-x86.img,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/disk-x86.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2223-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci,xres=1280,yres=800 -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:/tmp/qemu-mon-x86.sock,server,nowait \
  -serial file:/home/melton/BulumOS/boot-gui-x86.log \
  -display gtk &
QPID=$!
echo "qemu pid $QPID - waiting ~90s..."
sleep 90
echo "--- boot log tail ---"
tail -12 boot-gui-x86.log | cat -v | sed 's/\^\[\[[0-9;]*m//g'
python3 tools/send-mon.py /tmp/qemu-mon-x86.sock "info display" >> mon-x86.log 2>&1
python3 tools/send-mon.py /tmp/qemu-mon-x86.sock "screendump /home/melton/BulumOS/shot-x86.ppm" >> mon-x86.log 2>&1
sleep 3
kill $QPID 2>/dev/null || true
sleep 2
echo "DONE"
ls -la shot-x86.ppm 2>&1
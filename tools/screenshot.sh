#!/bin/bash
# Boot the BulumOS VM headless, let the desktop start, grab a framebuffer
# screendump via the QEMU monitor, and convert it to PNG.
cd /home/melton/BulumOS
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-dbg.log shot.ppm shot.png
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/BulumOS/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci,xres=1280,yres=800 -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:/tmp/qemu-mon.sock,server,nowait \
  -nographic > /home/melton/BulumOS/boot-dbg.log 2>&1"
echo "waiting for desktop to come up (~85s)..."
sleep 85
rm -f shot-mon.log
python3 tools/send-mon.py /tmp/qemu-mon.sock "info display" >> shot-mon.log 2>&1
python3 tools/send-mon.py /tmp/qemu-mon.sock "screendump /home/melton/BulumOS/shot.ppm" >> shot-mon.log 2>&1
sleep 3
pkill -f qemu-system 2>/dev/null || true
sleep 2
echo "--- monitor reply ---"
cat shot-mon.log
if [ -f shot.ppm ]; then
    python3 tools/ppm2png.py shot.ppm shot.png
else
    echo "no screendump captured"
fi
echo "DONE"
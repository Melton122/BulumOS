#!/bin/bash
cd /home/melton/BulumOS
rm -f shot-x86.ppm
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
  -display vnc=127.0.0.1:3 2>/dev/null &
QPID=$!
echo "qemu pid $QPID - waiting ~75s..."
for i in $(seq 1 75); do
    if grep -aq 'BulumOS is ready' boot-gui-x86.log 2>/dev/null; then break; fi
    sleep 1
done
sleep 4
echo "--- display info ---"
ser=$( timeout 4 python3 tools/send-mon.py /tmp/qemu-mon-x86.sock 'info display' )
echo "$ser"
if echo "$ser" | grep -q '1280x800'; then
    timeout 4 python3 tools/send-mon.py /tmp/qemu-mon-x86.sock 'screendump /home/melton/BulumOS/shot-x86.ppm' >/dev/null
    sleep 1
fi
echo "--- boot log tail ---"
grep -a 'boot complete' boot-gui-x86.log | tail -1 | cat -v
grep -a 'BulumOS is ready' boot-gui-x86.log | tail -1 | cat -v
kill $QPID 2>/dev/null
ls -la shot-x86.ppm 2>&1
echo DONE
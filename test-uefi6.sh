#!/bin/bash
set -e
cd /home/melton/myos
echo "=== 1. rebuild EFI app with cpio/tar modules ==="
bash build-uefi.sh 2>&1 | grep -E '===|BOOTAA64|error' 
echo "=== 2. rebuild ESP + ISO ==="
bash build-hybrid-iso.sh 2>&1 | grep -E '===|BOOTAA64|Written'
echo "=== 3. make UEFI image ==="
cp /tmp/efiboot.img bulumos-uefi.img
ls -la bulumos.iso bulumos-uefi.img
echo "=== 4. boot via EFI image ==="
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null
pkill -f qemu-system 2>/dev/null
sleep 1
rm -f boot-uefi.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci \
  -nographic > /home/melton/myos/boot-uefi.log 2>&1"
echo "waiting..."
sleep 55
python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-uefi.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
lines = [l.strip('\n') for l in data.split('\r')]
hits = 0
for l in lines:
    if l.startswith('  [') or 'BdsDxe' in l or 'ready' in l or 'Run /sbin/init' in l or 'error' in l.lower() or 'BulumOS 1.0' in l or 'Linux version' in l:
        print(l); hits += 1
if hits == 0:
    print("(no matching lines - raw tail:)", repr(lines[-6:]))
EOF
#!/bin/bash
set -e
cd /home/melton/myos
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null
pkill -f qemu-system 2>/dev/null
sleep 1
rm -f boot-uefi-fs.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/tmp/efiboot.img,format=raw,if=virtio \
  -nographic > /home/melton/myos/boot-uefi-fs.log 2>&1"
echo "started; waiting..."
sleep 30
python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-uefi-fs.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
lines = [l.strip('\n') for l in data.split('\r')]
for l in lines:
    if l.startswith('  [') or 'BdsDxe' in l or 'ready' in l or 'Run /sbin/init' in l or 'Shell' in l or 'error' in l.lower():
        print(l)
EOF

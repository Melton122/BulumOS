#!/bin/bash
# recovery-mode sanity: select GRUB recovery entry, expect shell prompt
set -e
cd /home/melton/myos
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-recovery.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -nographic > /home/melton/myos/boot-recovery.log 2>&1"
echo "trying to select recovery entry (send loop)..."
for i in $(seq 1 18); do
    tmux send-keys -t qemu Down Enter
    sleep 0.5
done
echo "waiting 50s..."
sleep 50
tmux send-keys -t qemu 'echo RECOVERY_OK' Enter
sleep 3
tmux send-keys -t qemu 'poweroff' Enter
sleep 8
pkill -f qemu-system 2>/dev/null || true
python3 - <<'EOF'
import re
txt = open('/home/melton/myos/boot-recovery.log','rb').read().decode('utf-8','replace')
txt = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',txt)
print('PASS recovery banner' if '=== RECOVERY MODE ===' in txt else 'FAIL recovery banner')
print('PASS recovery shell'  if 'RECOVERY_OK' in txt else 'FAIL recovery shell')
s = txt.find('=== RECOVERY MODE ===')
print(txt[max(0,s-900):s+150] if s>=0 else txt[-600:])
EOF
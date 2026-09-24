#!/bin/bash
# BulumOS 1.2 full test: UEFI boot -> switch_root -> login -> services -> framebuffer
set -e
cd /home/melton/myos
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-uefi8.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci \
  -nographic > /home/melton/myos/boot-uefi.log 2>&1"

echo "waiting 70s for boot..."
sleep 70

echo "=== boot output check ==="
python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-uefi.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
lines = [l.strip('\n') for l in data.split('\r')]
checks = {
    'grub':        any('GNU GRUB' in l for l in lines),
    'linux boot':  any('Booting Linux' in l for l in lines),
    'switch_root': any('switching root' in l or 'BusyBox' in l for l in lines),
    'sa banner':   any('BulumOS 1.2' in l for l in lines),
    'mount ok':    any('[ OK ] Mounting filesystems' in l for l in lines),
    '8+ services': sum(1 for l in lines if l.startswith('  [ OK ]')) >= 8,
    'login prompt':any(l.strip().startswith('BulumOS login:') for l in lines),
    'fb console':  any('console [tty0]' in l for l in lines),
}
for k, v in checks.items():
    print(f"{'PASS' if v else 'FAIL'}  {k}")
if not checks['login prompt']:
    print('(login prompt not seen; last 20 lines:)')
    for l in lines[-20:]:
        print(repr(l))
EOF

echo "=== login as root ==="
tmux send-keys -t qemu 'root' Enter
sleep 2
tmux send-keys -t qemu 'root' Enter
sleep 3
tmux send-keys -t qemu 'hostname; uname -r' Enter
tmux send-keys -t qemu 'bstatus' Enter
tmux send-keys -t qemu 'ls /dev/fb0 && echo FB_OK' Enter
tmux send-keys -t qemu 'grep vdb /proc/mounts && echo ROOTDISK_OK' Enter
tmux send-keys -t qemu 'id' Enter
tmux send-keys -t qemu 'cat /etc/hostname' Enter
sleep 4

python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-uefi.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
lines = [l.strip() for l in data.split('\r')]
for key, label in [('BulumOS', 'hostname'), ('7.1.5', 'kernel'),
                   ('Service status', 'bstatus ran'),
                   ('FB_OK', 'framebuffer /dev/fb0'),
                   ('ROOTDISK_OK', 'root on /dev/vdb'),
                   ('uid=0(root)', 'root shell')]:
    hit = any(key in l for l in lines)
    print(f"{'PASS' if hit else 'FAIL'}  {label}")
EOF

echo "=== now test shutdown: send poweroff ==="
tmux send-keys -t qemu 'poweroff' Enter
sleep 10
tail -c 600 boot-uefi.log | tr -d '\033' | tail -8
echo "=== qemu still running? ==="
ps -ef | grep -c '[q]emu-system' || true
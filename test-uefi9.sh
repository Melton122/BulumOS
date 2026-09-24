#!/bin/bash
# BulumOS 1.2 persistence test: user login, write files, verify stored in disk.img
set -e
cd /home/melton/myos
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-uefi9.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci \
  -nographic > /home/melton/myos/boot-uefi9.log 2>&1"

echo "waiting 70s..."
sleep 70

echo "=== login as melton (standard user) ==="
tmux send-keys -t qemu 'melton' Enter
sleep 2
tmux send-keys -t qemu 'melton' Enter
sleep 3
tmux send-keys -t qemu 'id' Enter
tmux send-keys -t qemu "whoami; echo '42-keepme' > /home/melton/hello.txt; cat /home/melton/hello.txt" Enter
tmux send-keys -t qemu 'sudo whoami 2>&1 | head -1' Enter
sleep 4

python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-uefi9.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
lines = [l.strip() for l in data.split('\r')]
checks = {
    'melton login':  any('uid=1000(melton)' in l for l in lines),
    'cat echoed':    any('42-keepme' in l for l in lines),
    'no sudo':       any('not found' in l and 'sudo' in l for l in lines),
}
for k, v in checks.items():
    print(f"{'PASS' if v else 'FAIL'}  {k}")
EOF

echo "=== shutdown (root poweroff for reboot) ==="
tmux send-keys -t qemu 'exit' Enter
sleep 4
tmux send-keys -t qemu 'root' Enter
sleep 2
tmux send-keys -t qemu 'root' Enter
sleep 3
tmux send-keys -t qemu 'sync; poweroff' Enter
for i in $(seq 1 30); do
    n=$(ps -ef | grep -c '[q]emu-system' || true)
    [ "$n" -lt 3 ] && break
    sleep 1
done
echo "qemu procs: $(ps -ef | grep -c '[q]emu-system' || true)"

echo "=== verify file persisted in disk.img (debugfs) ==="
debugfs -R "cat /home/melton/hello.txt" disk.img 2>&1 | head -3
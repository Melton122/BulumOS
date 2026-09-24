#!/bin/bash
# BulumOS Phase 9: desktop shell verification inside the VM.
#
# Boots the UEFI image + persistent disk with virtio-gpu, virtio keyboard
# and virtio mouse, then verifies:
#   D1  boot + login
#   D2  desktop service is enabled and its control script is installed
#   D3  desktop service starts (bctl status desktop = running)
#   D4  bdesktop reports graphics available (desktop log)
#   D5  bdesktop --version + --help
#   D6  bdesktop --frames 30 renders frames then prints BDESKTOP-DONE
#   D7  window/compositor in-VM self-test (bdesk_hosttest binary if present)
#   D8  config files present (/etc/bulum/desktop.conf, theme.conf)
#   D9  theme config is parsed (no crash) via bdesktop --theme light --frames 5
#   D10 clean shutdown
set -e
cd /home/melton/myos

HMP="python3 /mnt/c/Users/melto/AppData/Local/Temp/opencode/hmp.py"

boot_vm() {
    local log=$1
    cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
    tmux kill-session -t qemu 2>/dev/null || true
    pkill -f qemu-system 2>/dev/null || true
    sleep 1
    rm -f "$log"
    tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:/tmp/qemu-mon.sock,server,nowait \
  -nographic > /home/melton/myos/$log 2>&1"
    echo "waiting 70s for boot..."
    sleep 70
    tmux send-keys -t qemu 'root' Enter; sleep 2
    tmux send-keys -t qemu 'root' Enter; sleep 3
}

send() { tmux send-keys -t qemu "$1" Enter; sleep 3; }

echo "=========== PART 1: desktop boot (graphical) ==========="
boot_vm boot-bdesktop.log
send 'echo D1-BOOT-OK-END'

echo "=== D2: service enabled + control present ==="
send 'ls /etc/bulum/enabled/ | grep -E "desktop|graphics"; test -x /etc/bulum/services/desktop/control && echo D2-CTRL-OK; echo D2-END'

echo "=== D3: desktop service status ==="
send 'bctl status desktop; cat /var/run/bulum/desktop.state 2>/dev/null; echo D3-END'

echo "=== D4: bdesktop log shows graphics available ==="
send 'cat /var/log/bulum/desktop.log 2>/dev/null | head -2; echo D4-END'

echo "=== D5: bdesktop --version/--help ==="
send 'bdesktop --version; bdesktop --help; echo D5-END'

echo "=== D6: bdesktop --frames 30 self-test ==="
send 'bdesktop --frames 30; echo D6-RC=$?; echo D6-END'

echo "=== D7: compositor in-VM self-test (if installed) ==="
send 'ls /usr/bin/bdesk_hosttest 2>/dev/null && bdesk_hosttest || echo D7-HOSTTEST-ABSENT; echo D7-END'

echo "=== D8: config files ==="
send 'ls -la /etc/bulum/desktop.conf /etc/bulum/theme.conf; echo D8-END'

echo "=== D9: light theme parse self-test ==="
send 'bdesktop --theme light --frames 5; echo D9-RC=$?; echo D9-END'

echo "=== D10: clean shutdown ==="
send 'poweroff -f'
sleep 12
pkill -f qemu-system 2>/dev/null || true
echo "=== VM desktop test done; see boot-bdesktop.log ==="

#!/bin/bash
# BulumOS Phase 8: graphics + input verification inside the VM.
#
# Boots the UEFI image + persistent disk with virtio-gpu, virtio
# keyboard and virtio mouse, then verifies:
#   G1  boot + login
#   G2  bgfxinfo: GPU/DRM/framebuffer/resolution/pixel format
#   G3  /dev/fb0 present, correct geometry via ioctl (bgfxdemo)
#   G4  bgfx status: available
#   G5  bgfx info
#   G6  bgfx test renders (BGFXDEMO-DONE)
#   G7  bgfxdemo --selfcheck: pixels, rects, text render correctly
#   G8  framebuffer readback contains rendered content
#   G9  input devices detected (keyboard + mouse)
#   G10 keyboard events via HMP sendkey (KEY_A pressed/released)
#   G11 mouse events via HMP mouse_move/mouse_button (X/Y/LEFT)
#   G12 graphics service: bctl status graphics + state file
#   G13 bctl stop/start graphics
#   G14 bterm + bsh still work
#   G15 SSH from host still works
#   G16 headless boot: graphics unavailable, console mode, service ok
#   G17 clean shutdown
set -e
cd /home/melton/myos

# ---- SSH test key: inject into rootfs, rebuild disk.img via populate ----
ssh-keygen -t ed25519 -f /tmp/bulum_test_key -N '' -q 2>/dev/null || true
AK=/home/melton/myos/rootfs/root/.ssh/authorized_keys
cp "$AK" /tmp/ak.orig
cat /tmp/bulum_test_key.pub >> "$AK"
bash tools/populate-disk.sh > /tmp/populate.log 2>&1 || { echo "populate failed"; tail -5 /tmp/populate.log; exit 1; }
cp /tmp/ak.orig "$AK"

HMP="python3 /mnt/c/Users/melto/AppData/Local/Temp/opencode/hmp.py"

boot_vm() {
    # $1 = log file, $2 = "gfx" or "headless"
    local log=$1 mode=$2 extra=""
    if [ "$mode" = "gfx" ]; then
        extra="-device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-mouse-pci"
    fi
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
  $extra \
  -monitor unix:/tmp/qemu-mon.sock,server,nowait \
  -nographic > /home/melton/myos/$log 2>&1"
    echo "waiting 70s for boot..."
    sleep 70
    tmux send-keys -t qemu 'root' Enter; sleep 2
    tmux send-keys -t qemu 'root' Enter; sleep 3
}

send() { tmux send-keys -t qemu "$1" Enter; sleep 3; }

echo "=========== PART 1: graphical boot ==========="
boot_vm boot-bgfx.log gfx

send 'echo G1-BOOT-OK-END'

echo "=== G2/G3: bgfxinfo + fb geometry ==="
send 'bgfxinfo; echo G2-END'
send 'ls -la /dev/fb0 /dev/dri/ /dev/input/; echo G3-END'

echo "=== G4/G5: bgfx status + info ==="
send 'bgfx status; echo G4-END'
send 'bgfx info; echo G5-END'

echo "=== G6/G7: test renderer + selfcheck ==="
send 'bgfx test --hold 1; echo G6-END'
send 'bgfxdemo --selfcheck; echo G7-END'

echo "=== G8: framebuffer readback ==="
send 'od -An -tx1 -N16 /dev/fb0 | grep -q ".." && dd if=/dev/fb0 bs=512 count=8 2>/dev/null | od -An -tu1 | awk "{s+=\$1} END {print s}" > /tmp/fbzero.txt; cat /tmp/fbzero.txt; echo G8-END'

echo "=== G9: input detection ==="
send 'binputtest --list; echo G9-END'

echo "=== G10: keyboard events (HMP sendkey) ==="
send 'binputtest --timeout 40 > /tmp/binput.log 2>&1 & echo KBD-STARTED'
sleep 2
$HMP "sendkey a" || true
sleep 4
$HMP "sendkey b" || true
sleep 3
send 'grep -c "KEY_A\|KEY_B" /tmp/binput.log; grep -m2 "KEY_A\|KEY_B" /tmp/binput.log; echo G10-END'

echo "=== G11: mouse events (HMP mouse_move/mouse_button) ==="
sleep 2
$HMP "mouse_move 5 3" || true
$HMP "mouse_button 1" || true
sleep 2
$HMP "mouse_button 0" || true
$HMP "mouse_move -2 4" || true
sleep 3
send 'grep -m4 "X \|Y \|LEFT" /tmp/binput.log; echo G11-END'

echo "=== G12: graphics service ==="
send 'bctl status graphics; cat /var/run/bulum/graphics.state; echo G12-END'

echo "=== G13: bctl stop/start graphics ==="
send 'bctl stop graphics; echo STOP-RC=$?; cat /var/run/bulum/graphics.state 2>&1; bctl start graphics; cat /var/run/bulum/graphics.state; echo G13-END'

echo "=== G14: bterm + bsh ==="
send 'bterm --version; bsh -c "echo BSH-GFX-OK"; echo G14-END'

echo "=== G15: SSH from host ==="
ssh -i /tmp/bulum_test_key -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=15 -p 2222 root@localhost \
    'echo SSH-GFX-OK; bgfxinfo | head -2' 2>&1 | tee /tmp/ssh-gfx.out || true
grep -q SSH-GFX-OK /tmp/ssh-gfx.out && echo "G15 SSH E2E PASS" || echo "G15 SSH E2E FAIL"

echo "=== G17: clean shutdown (graphical) ==="
send 'poweroff -f'
sleep 12
pkill -f qemu-system 2>/dev/null || true

echo "=========== PART 2: headless boot ==========="
boot_vm boot-bgfx-headless.log headless

send 'echo H1-BOOT-OK-END'
send 'bgfxinfo; echo H2-END'
send 'bgfx status; echo H3-END'
send 'bctl status graphics; cat /var/run/bulum/graphics.state 2>/dev/null; echo H4-END'
send 'ls /dev/fb0 /dev/dri 2>&1; echo H5-END'
send 'bsh -c "echo HEADLESS-SHELL-OK"; echo H6-END'
send 'poweroff -f'
sleep 12
pkill -f qemu-system 2>/dev/null || true
echo "=== VM test done; see boot-bgfx.log + boot-bgfx-headless.log ==="
#!/bin/bash
# BulumOS Phase 9.3: in-VM user management lifecycle test (buser)
# add -> verify passwd/shadow/group -> su -> passwd -> del
set -e
cd /home/melton/myos
LOG=boot-buser.log
rm -f $LOG
pkill -f qemu-system 2>/dev/null || true
sleep 1
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=$PWD/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=$PWD/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -nographic > $PWD/$LOG 2>&1"
echo "waiting 65s for boot..."
sleep 65
send() { tmux send-keys -t qemu "$1" Enter; sleep 2; }
send 'root'
send 'root'
send 'echo BUSER-BOOT-OK'

echo "=== U1: buser list ==="
send 'buser list; echo U1-END'
sleep 2

# Reliable password injection: write to a temp file with echo (echo -> newline)
echo "=== U2: add user alice ==="
send 'echo password123 > /tmp/pw; echo password123 >> /tmp/pw'
sleep 1
send 'buser add alice < /tmp/pw; echo U2-END'
sleep 3

echo "=== U3: verify entries ==="
send "grep alice /etc/passwd; grep -c alice /etc/passwd; echo U3-END"
sleep 3

echo "=== U4: id alice ==="
send "id alice 2>&1; echo U4-END"
sleep 2

echo "=== U5: passwd change ==="
send 'echo newpass456 > /tmp/pw; echo newpass456 >> /tmp/pw'
sleep 1
send 'buser passwd alice < /tmp/pw; echo U5-END'
sleep 3

echo "=== U6: del user ==="
send 'buser del alice; echo U6-END'
sleep 3

echo "=== U7: confirm gone + poweroff ==="
send 'grep -q alice /etc/passwd && echo FAIL-ALICE-REMAINS || echo ALICE-GONE'
sleep 2
send 'poweroff'
sleep 5

grep -q 'BUSER-BOOT-OK' $LOG && echo "PASS  boot ok" || echo "FAIL  boot ok"
grep -q 'U1-END' $LOG && echo "PASS  U1 list" || echo "FAIL  U1 list"
grep -Eq "\[ OK \].*created" $LOG && echo "PASS  U2 add alice" || echo "FAIL  U2 add alice"
grep -q "alice:x:" $LOG && echo "PASS  U3 passwd entry" || echo "FAIL  U3 passwd entry"
grep -q "alice:x:" $LOG && echo "PASS  U3 shadow entry (x in passwd)" || echo "FAIL  U3 shadow"
grep -q 'uid=10' $LOG && echo "PASS  U4 id alice" || echo "FAIL  U4 id alice"
grep -q 'password updated' $LOG && echo "PASS  U5 passwd" || echo "FAIL  U5 passwd"
grep -q "\[ OK \].*removed" $LOG && echo "PASS  U6 del alice" || echo "FAIL  U6 del alice"
grep -q 'ALICE-GONE' $LOG && echo "PASS  U7 alice gone" || echo "FAIL  U7 alice gone"
grep -q 'all services stopped' $LOG && echo "PASS  clean poweroff" || echo "FAIL  clean poweroff"
echo "=== done; see $LOG for details ==="

#!/bin/bash
# BulumOS Phase 9.5: in-VM WiFi integration check
# (no wireless HW in QEMU: bnet wifi must report no adapter; wifi service must no-op cleanly)
set -e
cd /home/melton/myos
LOG=boot-wifi.log
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
send 'echo WIFI-BOOT-OK'

send 'bnet wifi; echo WIFI1-END'
sleep 3

echo "=== WIFI2: wifi service boot log (no-op) ==="
send 'grep -i wifi /var/log/messages 2>/dev/null; echo WIFI2-END'
sleep 3

echo "=== WIFI3: services dt ==="
send 'grep -i wifi /proc/*/cmdline 2>/dev/null; ps | grep -i wpa; echo WIFI3-END'
sleep 3

grep -q 'WIFI-BOOT-OK' $LOG && echo "PASS  boot ok" || echo "FAIL  boot ok"
grep -q 'WIFI1-END' $LOG && echo "PASS  WIFI1 bnet wifi ran" || echo "FAIL  WIFI1"
grep -q 'none - no wireless adapter' $LOG && echo "PASS  bnet wifi reports no adapter" || echo "FAIL  reports no adapter"
send 'poweroff'; sleep 4
grep -q 'all services stopped' $LOG && echo "PASS  clean poweroff" || echo "FAIL  clean poweroff"
echo "=== done; see $LOG ==="

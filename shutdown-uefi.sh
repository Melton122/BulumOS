#!/bin/bash
cd /home/melton/myos
tmux send-keys -t qemu 'poweroff' Enter
sleep 8
echo "=== tail ==="
tail -c 400 boot-uefi.log | tr -d "\033" | tail -6
echo "=== qemu still running? ==="
ps -ef | grep -c '[q]emu-system'
echo "=== menu shown at boot? ==="
grep -ac 'BulumOS 1.0' boot-uefi.log
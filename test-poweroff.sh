#!/bin/bash
: "${BULUMOS_ROOT_PASSWORD:?Set BULUMOS_ROOT_PASSWORD to the password used to build the image}"
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"
tmux kill-session -t qemu 2>/dev/null
pkill -f qemu-system 2>/dev/null
bash rebuild-userspace.sh 2>&1 | grep -E 'initramfs OK|error' 
rm -f boot.log
tmux new-session -d -s qemu "\"$ROOT_DIR\"/run-qemu.sh > \"$ROOT_DIR\"/boot.log 2>&1"
sleep 32
python3 parse-boot.py | tail -2
tmux send-keys -t qemu 'root' Enter
sleep 2
tmux send-keys -t qemu "$BULUMOS_ROOT_PASSWORD" Enter
sleep 3
tmux send-keys -t qemu 'poweroff' Enter
sleep 8
echo "=== tail of log ==="
tail -c 400 boot.log | tr -d '\033' | tail -6
echo "=== qemu processes ==="
ps -ef | grep -c '[q]emu-system'

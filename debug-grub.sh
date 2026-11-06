#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"
tmux kill-session -t qemu 2>/dev/null
pkill -f qemu-system 2>/dev/null
sleep 1
rm -f boot-grub.log
tmux kill-server 2>/dev/null
sleep 1
tmux new-session -d -s qemu ""$ROOT_DIR"/run-uefi-grub.sh" 2>&1
sleep 25
tmux send-keys -t qemu 'set' Enter
sleep 2
tmux send-keys -t qemu 'ls' Enter
sleep 2
tmux send-keys -t qemu 'ls (memdisk)' Enter
sleep 2
tmux send-keys -t qemu 'cat (memdisk)/boot/grub/grub.cfg' Enter
sleep 2
tail -c 3000 "$ROOT_DIR"/boot-grub.log | tr -d '\033' | tail -25
#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
tmux send-keys -t qemu 'set' Enter
sleep 1
tmux send-keys -t qemu 'ls' Enter
sleep 1
tmux send-keys -t qemu 'ls (memdisk)' Enter
sleep 1
tmux send-keys -t qemu 'cat (memdisk)/boot/grub/grub.cfg' Enter
sleep 1
tmux send-keys -t qemu 'insmod memdisk' Enter
sleep 1
tail -c 2000 "$ROOT_DIR"/boot-uefi.log | tr -d '\033' | tail -20

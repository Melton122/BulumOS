#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"
tmux kill-server 2>/dev/null
sleep 1
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux new-session -d -s qemu "bash \"$ROOT_DIR\"/run-uefi-boot.sh > /dev/null 2>&1"
sleep 65
echo "READY_COUNT=$(grep -ac 'BulumOS is ready' "$ROOT_DIR"/boot-uefi.log 2>/dev/null)"
echo "KERNEL_COUNT=$(grep -ac 'Linux version' boot-uefi.log 2>/dev/null)"
tail -c 800 boot-uefi.log 2>/dev/null | tr -d "\033" | tail -14
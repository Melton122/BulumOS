#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"
tmux kill-server 2>/dev/null
sleep 1
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux new-session -d -s qemu "bash \"$ROOT_DIR\"/run-iso-disk.sh > /dev/null 2>&1"
sleep 60
echo "READY=$(grep -ac 'BulumOS is ready' boot-iso.log 2>/dev/null)"
echo "KERNEL=$(grep -ac 'Linux version' boot-iso.log 2>/dev/null)"
tail -c 500 boot-iso.log 2>/dev/null | tr -d "\033" | tail -8
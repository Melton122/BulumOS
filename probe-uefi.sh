#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
tmux send-keys -t qemu 'drivers' Enter
sleep 3
grep -aE 'Fat|Iso|Partition|Ext4|Fv' "$ROOT_DIR"/boot-uefi.log | head -10

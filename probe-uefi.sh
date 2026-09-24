#!/bin/bash
tmux send-keys -t qemu 'drivers' Enter
sleep 3
grep -aE 'Fat|Iso|Partition|Ext4|Fv' /home/melton/myos/boot-uefi.log | head -10

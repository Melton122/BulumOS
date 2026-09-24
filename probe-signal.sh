#!/bin/bash
tmux send-keys -t qemu 'grep -E "SigIgn|SigCgt|SigBlk" /proc/1/status' Enter
sleep 1
tmux send-keys -t qemu 'kill -USR2 1' Enter
sleep 3
tail -c 500 /home/melton/myos/boot.log | tr -d '\033' | tail -8

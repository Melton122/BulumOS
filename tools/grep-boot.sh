#!/bin/bash
LOG=/home/melton/BulumOS/boot-dbg.log
sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b\[K//g' "$LOG" | grep -an 'BulumOS login\|root@BulumOS:\|timed out\|not found\|killed\|BDESKTOP' | tail -40
#!/bin/bash
LOG=/home/melton/BulumOS/boot-dbg.log
echo "=== init + bctl boot service lines ==="
sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b\[K//g' "$LOG" \
  | grep -aE 'ok |starting|Starting|\[\.\.\.\]|\[OK\]|\[ \.\.\. \]|boot complete|desktop|graphics|BulumOS is ready|panic|Kernel panic|Segmentation' \
  | grep -aav 'BulumOS login\|login:' | tail -30
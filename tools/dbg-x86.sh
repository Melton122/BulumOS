#!/bin/bash
cd /home/melton/BulumOS
echo "== full log size =="
wc -l boot-gui-x86.log
echo "== last 25 lines =="
tail -25 boot-gui-x86.log | cat -v | sed 's/\^\[\[[0-9;]*m//g'
echo "== monitor reply =="
cat mon-x86.log 2>&1 | head -5
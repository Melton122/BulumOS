#!/bin/bash
cd /home/melton/BulumOS
echo "== service lines =="
grep -a -E '\[ OK \]|\[\.\.\.\]|boot complete|desktop|graphics|ready' boot-gui-x86.log | cat -v | sed 's/\^\[\[[0-9;]*m//g' | head -30
echo "== total lines =="
wc -l boot-gui-x86.log
echo "== panic/segfault/error scan =="
grep -a -iE 'panic|segfault|error|failed|oops' boot-gui-x86.log | grep -v 'ata[0-9]' | head -10
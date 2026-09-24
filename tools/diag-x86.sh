#!/bin/bash
cd /home/melton/BulumOS
echo "== !! lines =="
grep -a '!!' boot-gui-x86.log | cat -v | sed 's/\^\[\[[0-9;]*m//g'
echo "== graphics/desktop/boot lines =="
grep -a -E 'graphics|desktop|boot|defer|failed' boot-gui-x86.log | cat -v | sed 's/\^\[\[[0-9;]*m//g' | head -20
echo "== ready/login =="
grep -a -E 'ready|login|dropbear' boot-gui-x86.log | cat -v | sed 's/\^\[\[[0-9;]*m//g' | head -5
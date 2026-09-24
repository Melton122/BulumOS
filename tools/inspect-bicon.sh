#!/bin/bash
cd /home/melton/myos
grep -n 'strcmp\|name ==\|"launcher"\|"terminal"\|"help"' src/bicon.c | head -50
echo '--- sizes ---'
wc -l src/bicon.c
#!/bin/bash
echo "== log tail =="
tail -4 /home/melton/BulumOS/tools/kbuild-x86.log 2>&1
echo "== procs =="
pgrep -af make | grep -v pgrep | head -3
echo "== bzImage =="
ls -la /home/melton/ksrc/arch/x86/boot/bzImage 2>&1
echo "== log lines =="
wc -l /home/melton/BulumOS/tools/kbuild-x86.log 2>&1
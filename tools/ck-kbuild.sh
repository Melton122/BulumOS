#!/bin/bash
echo "== procs =="
pgrep -af "make|cc1" | head -5
echo "== artifacts =="
ls -la /home/melton/ksrc/arch/x86/boot/bzImage 2>&1
ls -la /home/melton/ksrc/vmlinux 2>&1 | head -1
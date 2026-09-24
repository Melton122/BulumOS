#!/bin/bash
cd /home/melton/BulumOS
echo "=== /bin/sh ==="
ls -l rootfs/bin/sh
echo "=== /bin/grep ==="
ls -l rootfs/bin/grep
echo "=== /bin (names) ==="
ls rootfs/bin | head -60
echo "=== busybox? ==="
ls -l rootfs/bin/busybox 2>&1
echo "=== does bsh.c implement backtick/command-substitution? ==="
grep -n 'backtick\|`' src/bsh.c | head
echo "=== does bsh.c handle \$ normal var? ==="
grep -n 'B_VAR\|do_var\|expand' src/bsh.c | head
echo "DONE"

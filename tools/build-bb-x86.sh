#!/bin/bash
set -e
rm -rf /tmp/bb-x86
cp -r /home/melton/BulumOS/kernel/busybox /tmp/bb-x86
cd /tmp/bb-x86
make clean >/dev/null 2>&1 || true
make -j8 2>&1 | tail -3
file busybox
cp busybox /tmp/bb-x86.busybox
ls -la /tmp/bb-x86.busybox
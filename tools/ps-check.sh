#!/bin/bash
ps -ef | grep -i qemu | grep -v grep
echo ---
for p in /proc/$(pidof qemu-system-x86_64)/status; do :; done
ss -lntp 2>/dev/null | grep 2223
echo st
ss -lnt 2>/dev/null | grep 2223
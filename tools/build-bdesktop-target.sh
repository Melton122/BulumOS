#!/bin/bash
# Cross-compile the Phase 9 desktop for the BulumOS target (aarch64).
set -e
cd /home/melton/myos
mkdir -p /tmp/bdesk-target
aarch64-linux-gnu-gcc -static -O2 -s -o /tmp/bdesk-target/bdesktop \
    src/bdesktop.c src/bwm.c src/bshell.c \
    src/bgfx.c src/binput.c src/bwindow.c src/btheme.c \
    src/bapp.c src/bnotify.c src/bicon.c \
    2>/tmp/bdesk-target/build.log
echo "TARGET-BUILD-OK"
file /tmp/bdesk-target/bdesktop | head -c 120
echo

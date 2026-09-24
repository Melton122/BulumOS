#!/bin/bash
# host build check for Phase 9 desktop (WSL/Linux host, not the BulumOS target)
set -e
mkdir -p /tmp/bdesk-host
cd /home/melton/myos
gcc -O2 -Wall -o /tmp/bdesk-host/bdesktop \
    src/bdesktop.c src/bwm.c src/bshell.c src/bgfx.c src/binput.c \
    src/bwindow.c src/btheme.c src/bapp.c src/bnotify.c src/bicon.c
echo "HOST-BUILD-OK"

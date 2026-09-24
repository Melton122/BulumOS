#!/bin/bash
cd /home/melton/myos
gcc -O2 -Wall -o /tmp/bdesk-host/bdesktop2 \
    src/bdesktop.c src/bwm.c src/bshell.c src/bgfx.c src/binput.c \
    src/bwindow.c src/btheme.c src/bapp.c src/bnotify.c src/bicon.c \
    >/tmp/bdesk-host/build.log 2>&1
echo "CC_EXIT=$?"

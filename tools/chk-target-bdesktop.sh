#!/bin/bash
# Compile-check the full Phase 10 bdesktop for aarch64 (does NOT install).
cd /home/melton/myos
set -e
aarch64-linux-gnu-gcc -static -O2 -s -o /tmp/bdesktop-phase10 \
    src/bdesktop.c src/bwm.c src/bshell.c \
    src/bgfx.c src/binput.c src/bwindow.c src/btheme.c \
    src/bapp.c src/bnotify.c src/bicon.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c
echo "TARGET-PHASE10-BUILD-OK: $(file -b /tmp/bdesktop-phase10 | cut -c1-60)"
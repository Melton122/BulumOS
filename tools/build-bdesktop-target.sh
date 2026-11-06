#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# Cross-compile the Phase 9 desktop for the BulumOS target (aarch64).
set -e
cd "$ROOT_DIR"
mkdir -p /tmp/bdesk-target
aarch64-linux-gnu-gcc -static -O2 -s -o /tmp/bdesk-target/bdesktop \
    src/bdesktop.c src/bwm.c src/bshell.c \
    src/bgfx.c src/binput.c src/bwindow.c src/btheme.c \
    src/bapp.c src/bnotify.c src/bicon.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_storage.c src/bstorage.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c src/bapp_hardware.c src/bhw.c \
    2>/tmp/bdesk-target/build.log
echo "TARGET-BUILD-OK"
file /tmp/bdesk-target/bdesktop | head -c 120
echo

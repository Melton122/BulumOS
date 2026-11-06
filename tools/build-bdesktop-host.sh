#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# host build check for Phase 9 desktop (WSL/Linux host, not the BulumOS target)
set -e
mkdir -p /tmp/bdesk-host
cd "$ROOT_DIR"
gcc -O2 -Wall -o /tmp/bdesk-host/bdesktop \
    src/bdesktop.c src/bwm.c src/bshell.c src/bgfx.c src/binput.c \
    src/bwindow.c src/btheme.c src/bapp.c src/bnotify.c src/bicon.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_storage.c src/bstorage.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c src/bapp_hardware.c src/bhw.c
echo "HOST-BUILD-OK"

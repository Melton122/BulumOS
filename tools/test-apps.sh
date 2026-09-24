#!/bin/bash
# Host functional test for the Phase 10 application layer (Stage 28).
set -u
cd "$(dirname "$0")/.."
BIN=/tmp/bapp-host
mkdir -p "$BIN"
echo "== building host app test =="
gcc -O2 -Wall -o "$BIN/bapp_hosttest" \
    src/bapp_hosttest.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c src/bapp_hardware.c src/bhw.c \
    src/bwm.c src/bwindow.c src/btheme.c src/bnotify.c src/bicon.c \
    src/bapp.c \
    || { echo "build bapp_hosttest failed"; exit 1; }
"$BIN/bapp_hosttest"
rc=$?
echo "== rc=$rc =="
exit $rc
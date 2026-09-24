#!/bin/bash
# Host functional test for the Phase 9 desktop stack.
# Builds bdesk_hosttest against stubbed bgfx/binput and the real
# compositor/shell/pure modules, runs the scenario checks.
set -u
cd "$(dirname "$0")/.."
BIN=/tmp/bdesk-host
mkdir -p "$BIN"
echo "== building host desktop test =="
gcc -O2 -Wall -o "$BIN/bdesk_hosttest" \
    src/bdesk_hosttest.c \
    src/bwm.c src/bshell.c src/bwindow.c src/btheme.c \
    src/bapp.c src/bnotify.c src/bicon.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c src/bapp_hardware.c src/bhw.c \
    || { echo "build bdesk_hosttest failed"; exit 1; }
"$BIN/bdesk_hosttest"
rc=$?
echo "== rc=$rc =="
exit $rc

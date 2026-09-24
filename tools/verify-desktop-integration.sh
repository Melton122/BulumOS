#!/bin/bash
# Validate Phase 9 desktop integration into rootfs (no heavy initramfs/disk).
set -e
cd /home/melton/myos

echo "== building desktop into rootfs =="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bdesktop \
    src/bdesktop.c src/bwm.c src/bshell.c \
    src/bgfx.c src/binput.c src/bwindow.c src/btheme.c \
    src/bapp.c src/bnotify.c src/bicon.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c src/bapp_hardware.c src/bhw.c
chmod 755 rootfs/usr/bin/bdesktop
chmod 644 rootfs/etc/bulum/desktop.conf rootfs/etc/bulum/theme.conf

echo "== installing services =="
bash tools/install-services.sh

echo "== verify =="
file rootfs/usr/bin/bdesktop | head -c 90; echo
ls -l rootfs/etc/bulum/desktop.conf rootfs/etc/bulum/theme.conf
echo "desktop service meta:"; cat rootfs/etc/bulum/services/desktop/meta
echo "desktop enabled:"; ls rootfs/etc/bulum/enabled/ | grep -E 'desktop|graphics'
echo "desktop control exists:"; test -x rootfs/etc/bulum/services/desktop/control && echo yes
grep -n 'bdesktop' rootfs/etc/bulum/services/desktop/control | head -3
echo "INTEGRATION-OK"

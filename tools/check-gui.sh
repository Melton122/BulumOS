#!/bin/bash
echo "== display backends =="
qemu-system-aarch64 -display help 2>&1
echo "== WSLg =="
echo "DISPLAY=$DISPLAY WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
ls /mnt/wslg 2>/dev/null | head -3
echo "== gui libs =="
ldconfig -p 2>/dev/null | grep -cE "libgtk-3|libSDL2"
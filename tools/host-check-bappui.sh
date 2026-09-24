#!/bin/bash
# Host compile-check for bappui + dependencies (Phase 10).
# Compiles the framework and everything it depends on to objects on the host.
cd /home/melton/myos
set -e
mkdir -p /tmp/bappui-host
gcc -Wall -Wextra -O2 -I src -c src/bappui.c -o /tmp/bappui-host/bappui.o
gcc -Wall -Wextra -O2 -I src -c src/bwm.c -o /tmp/bappui-host/bwm.o
gcc -Wall -Wextra -O2 -I src -c src/bshell.c -o /tmp/bappui-host/bshell.o
gcc -Wall -Wextra -O2 -I src -c src/bwindow.c -o /tmp/bappui-host/bwindow.o
gcc -Wall -Wextra -O2 -I src -c src/btheme.c -o /tmp/bappui-host/btheme.o
gcc -Wall -Wextra -O2 -I src -c src/bicon.c -o /tmp/bappui-host/bicon.o
gcc -Wall -Wextra -O2 -I src -c src/bapp.c -o /tmp/bappui-host/bapp.o
gcc -Wall -Wextra -O2 -I src -c src/bnotify.c -o /tmp/bappui-host/bnotify.o
echo "HOST-BAPPUI-COMPILE-OK"

#!/bin/bash
cd /home/melton/myos
FILES="src/bappui.c src/bsys.c src/bapps.c src/bapp_files.c src/bapp_settings.c \
       src/bapp_about.c src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
       src/bapp_services.c src/bapp_logs.c src/bapp_users.c src/bapp_packages.c \
       src/bapp_terminal.c"
echo "=== system/popen ==="
grep -nE '\b(system|popen|pclose)\s*\(' $FILES || echo "none"
echo "=== unbounded strcpy/strcat/sprintf (raw tokens; most are false-friendly) ==="
grep -nE '\b(strcpy|strcat|sprintf)\s*\(' $FILES || echo "none"
echo "=== gets / alloca ==="
grep -nE '\bgets\s*\(|\balloca\s*\(' $FILES || echo "none"
echo "=== fork/execve usage (must be present in bsys only) ==="
grep -nE '\b(fork|execve|pipe)\s*\(' $FILES
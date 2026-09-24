#!/bin/bash
cd /home/melton/myos
fails=0
for f in bappui.c bsys.c bapps.c \
         bapp_files.c bapp_settings.c bapp_about.c bapp_monitor.c \
         bapp_network.c bapp_security.c bapp_services.c bapp_logs.c \
         bapp_users.c bapp_packages.c bapp_terminal.c; do
    echo "=== $f ==="
    err=$(gcc -Wall -Wextra -O2 -I src -c "src/$f" -o /tmp/chk.o 2>&1 | grep -E 'error:' )
    if [ -n "$err" ]; then
        echo "FAIL:"
        echo "$err"
        fails=1
    else
        w=$(gcc -Wall -Wextra -O2 -I src -c "src/$f" -o /tmp/chk.o 2>&1 | grep -E 'warning:' )
        [ -n "$w" ] && echo "warn: $w" | head -5
        echo "ok"
    fi
done
echo "=== RESULT ==="
[ $fails -eq 0 ] && echo "APPUI-ALL-COMPILE-OK" || echo "COMPILE-FAILURES-PRESENT"
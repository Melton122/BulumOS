#!/bin/bash
R="/home/melton/Bulum OS/rootfs"
echo "== /etc/bulum =="
ls -la "$R/etc/bulum/" 2>&1
echo "== keys =="
ls "$R/etc/bulum/keys" 2>&1
echo "== var/lib =="
ls "$R/var/lib/" 2>&1
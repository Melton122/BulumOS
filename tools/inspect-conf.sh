#!/bin/bash
cd /home/melton/myos
echo "--- rootfs/etc/bulum ---"
ls -la rootfs/etc/bulum/ 2>/dev/null
echo "--- theme.conf (if any) ---"
cat rootfs/etc/bulum/theme.conf 2>/dev/null || echo "(none)"
echo "--- desktop.conf (if any) ---"
cat rootfs/etc/bulum/desktop.conf 2>/dev/null || echo "(none)"
echo "--- btheme.c config key names ---"
grep -oE 'strcmp\(k, "[a-z_]+"\)' src/btheme.c | sort -u
echo "--- install-services.sh desktop/theme refs ---"
grep -nE 'desktop|theme|graphics' tools/install-services.sh | head -30

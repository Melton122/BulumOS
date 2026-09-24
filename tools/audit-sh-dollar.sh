#!/bin/bash
cd /home/melton/myos
echo "=== service control scripts containing command substitution \$() ==="
grep -rn '$(' rootfs/etc/bulum/services/
echo "=== done ==="

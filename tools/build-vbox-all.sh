#!/bin/bash
cd /home/melton/BulumOS
rm -f rootfs-x86/etc/bulum/autotest.sh
echo "autotest removed"
bash tools/repack-x86w.sh 2>&1 | tail -2
echo "=== build vbox iso ==="
bash tools/build-vbox.sh 2>&1 | tail -3
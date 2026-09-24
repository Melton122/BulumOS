#!/bin/bash
# Final Phase 9 check: integration into rootfs + services, then host test.
cd /home/melton/myos
echo "=================== 1. INTEGRATION ==================="
bash tools/verify-desktop-integration.sh > /tmp/intel.log 2>&1
echo "integ rc=$?"
grep -E "INTEGRATION-OK|rootfs/usr/bin/bdesktop:" /tmp/intel.log
echo "desktop meta:"
cat rootfs/etc/bulum/services/desktop/meta 2>/dev/null
echo "enabled:"; ls rootfs/etc/bulum/enabled/ | grep -E 'desktop|graphics' | tr '\n' ' '; echo
echo "=================== 2. HOST TEST ====================="
bash tools/test-bdesktop.sh 2>/dev/null | grep -E "PASS|FAIL|passed|failed"
echo "=================== 3. TARGET BUILD =================="
bash tools/build-bdesktop-target.sh 2>/dev/null | grep -E "TARGET-BUILD-OK|error"
echo "ALL-DONE"

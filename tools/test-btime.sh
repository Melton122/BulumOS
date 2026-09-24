#!/bin/bash
# BulumOS Phase 9.1: time host-side test suite.
#   tools/test-btime.sh  build host btime + run checks against a rootfs sandbox
set -e
cd /home/melton/myos
BIN="${BTIME_BIN_DIR:-/tmp/btime-host}"
mkdir -p "$BIN"
SANDBOX=$(mktemp -d)
mkdir -p "$SANDBOX"
BULUM_TZ_FILE="$SANDBOX/TZ" BULUM_NTP_CONF="$SANDBOX/ntp.conf" > /dev/null 2>&1 || true

if [ -z "${BTIME_BIN_DIR:-}" ]; then
    echo "== building host btime =="
    gcc -O2 -Wall -o "$BIN/btime" src/btime.c || { echo "build failed"; exit 1; }
fi
BTIME="$BIN/btime"

# seed a TZ + ntp.conf in the sandbox
printf 'UTC\n' > "$SANDBOX/TZ"
printf 'server 0.pool.ntp.org\n' > "$SANDBOX/ntp.conf"

PASS=0 FAIL=0
ok() { echo "PASS  $1"; PASS=$((PASS+1)); }
no() { echo "FAIL  $1"; FAIL=$((FAIL+1)); }

echo "=== btime host tests ==="
"$BTIME" show >/dev/null 2>&1 && ok "btime show" || no "btime show"
"$BTIME" help >/dev/null 2>&1 && ok "btime help" || no "btime help"
"$BTIME" status >/dev/null 2>&1 && ok "btime status" || no "btime status"
"$BTIME" set 2025-aa-bb 10:30 >/dev/null 2>&1 && no "bad date rejected" || ok "bad date rejected"
"$BTIME" bogus >/dev/null 2>&1 && no "unknown cmd exits nonzero" || ok "unknown cmd exits nonzero"

BULUM_TZ_FILE="$SANDBOX/TZ" "$BTIME" tz > /tmp/tz.out 2>&1
grep -q 'UTC' /tmp/tz.out && ok "tz reads sandbox TZ" || no "tz reads sandbox TZ"
BULUM_TZ_FILE="$SANDBOX/TZ" "$BTIME" tz EST5EDT >/dev/null 2>&1 && ok "tz set returns 0" || no "tz set returns 0"
grep -q 'EST5EDT' "$SANDBOX/TZ" && ok "tz wrote EST5EDT to sandbox" || no "tz wrote EST5EDT to sandbox"

BULUM_NTP_CONF="$SANDBOX/ntp.conf" "$BTIME" status 2>/dev/null | grep -q 'NTP' && ok "status shows NTP" || no "status shows NTP"

echo ""
echo "rootfs checks:"
test -f rootfs/etc/TZ && ok "rootfs/etc/TZ exists" || no "rootfs/etc/TZ exists"
grep -q '^UTC$\|^$' rootfs/etc/TZ && ok "rootfs/etc/TZ is UTC" || no "rootfs/etc/TZ is UTC"
test -f rootfs/etc/ntp.conf && ok "rootfs/etc/ntp.conf exists" || no "rootfs/etc/ntp.conf exists"
grep -q '^server' rootfs/etc/ntp.conf && ok "rootfs/etc/ntp.conf has server" || no "rootfs/etc/ntp.conf has server"

rm -rf "$SANDBOX" /tmp/tz.out
echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

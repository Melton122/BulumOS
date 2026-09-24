#!/bin/bash
# BulumOS Phase 9.3: user management host-side smoke test (buser)
# Full add/del/passwd/su lifecycle runs in the VM (test-buser-vm.sh) where
# /etc/passwd, /etc/shadow and busybox mkpasswd are the real ones.
set -e
cd /home/melton/myos
BIN="${BUSER_BIN_DIR:-/tmp/buser-host}"
mkdir -p "$BIN"
if [ -z "${BUSER_BIN_DIR:-}" ]; then
    gcc -O2 -Wall -o "$BIN/buser" src/buser.c || { echo "build failed"; exit 1; }
fi
BUSER="$BIN/buser"
PASS=0 FAIL=0
ok() { echo "PASS  $1"; PASS=$((PASS+1)); }
no() { echo "FAIL  $1"; FAIL=$((FAIL+1)); }

echo "=== buser host smoke tests ==="
"$BUSER" help >/dev/null 2>&1 && ok "help exits 0" || no "help exits 0"
"$BUSER" --help >/dev/null 2>&1 && ok "--help exits 0" || no "--help exits 0"
"$BUSER" list >/dev/null 2>&1 && ok "list works" || no "list works"
"$BUSER" list | grep -q 'users:' && ok "list prints 'users:'" || no "list prints 'users:'"
"$BUSER" bogus >/dev/null 2>&1 && no "unknown cmd exits nonzero" || ok "unknown cmd exits nonzero"
"$BUSER" add 'bad:name' </dev/null >/dev/null 2>&1 && no "invalid add rejected (nonroot-safe)" || ok "invalid add rejected"
"$BUSER" del </dev/null >/dev/null 2>&1 && no "del without arg errors" || ok "del without arg errors"
"$BUSER" passwd </dev/null >/dev/null 2>&1 && no "passwd without user requires existing uid" || ok "passwd handles no-arg"

echo "rootfs checks:"
test -x rootfs/usr/bin/buser && ok "rootfs/usr/bin/buser executable" || no "rootfs/usr/bin/buser executable"

echo ""
echo "=== $PASS passed, $FAIL failed (full lifecycle in VM) ==="
[ "$FAIL" -eq 0 ]

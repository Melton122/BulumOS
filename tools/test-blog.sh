#!/bin/bash
# BulumOS Phase 9.2: logging host-side test suite (blog + logrotate)
set -e
cd /home/melton/myos
BIN="${BLOG_BIN_DIR:-/tmp/blog-host}"
mkdir -p "$BIN"
if [ -z "${BLOG_BIN_DIR:-}" ]; then
    gcc -O2 -Wall -o "$BIN/blog" src/blog.c || { echo "build failed"; exit 1; }
fi
BLOG="$BIN/blog"
PASS=0 FAIL=0
ok() { echo "PASS  $1"; PASS=$((PASS+1)); }
no() { echo "FAIL  $1"; FAIL=$((FAIL+1)); }

D=$(mktemp -d)
printf 'Jan  1 00:00:01 info line one\nJan  1 00:00:02 warn line two\nJan  1 00:00:03 error line three\nJan  1 00:00:04 info line four\n' > "$D/test.log"

echo "=== blog tests (sandbox $D) ==="
"$BLOG" -n 2 "$D/test.log" > "$D/out.txt"
[ "$(wc -l < "$D/out.txt")" -eq 2 ] && ok "-n 2 shows last 2 lines" || no "-n 2 shows last 2 lines"
grep -q 'line three' "$D/out.txt" && ok "-n 2 contains line three" || no "-n 2 contains line three"

"$BLOG" -g 'warn' "$D/test.log" > "$D/out2.txt"
[ "$(grep -c 'warn' "$D/out2.txt")" -ge 1 ] && ok "-g warn filters to warn" || no "-g warn filters to warn"
! grep -q 'info line one' "$D/out2.txt" && ok "-g excludes non-matching" || no "-g excludes non-matching"

"$BLOG" -v 'info' "$D/test.log" > "$D/out3.txt"
! grep -q 'info' "$D/out3.txt" && ok "-v info drops info lines" || no "-v info drops info lines"
[ "$(wc -l < "$D/out3.txt")" -eq 2 ] && ok "-v leaves 2 lines" || no "-v leaves 2 lines"

"$BLOG" -r "$D/test.log" >/dev/null 2>&1 && ok "-r rotate returns 0" || no "-r rotate returns 0"

"$BLOG" "$D/nonexistent.log" >/dev/null 2>&1 && no "missing file errors" || ok "missing file errors"

echo "=== logrotate tests ==="
mkdir -p "$D/lg"
printf 'msg A\n' > "$D/lg/messages"
printf 'old 2\n' > "$D/lg/messages.2"
LOG="$D/lg/messages" bash rootfs/sbin/logrotate
[ -f "$D/lg/messages" ] && ok "current log recreated" || no "current log recreated"
[ -f "$D/lg/messages.1" ] && ok "messages.1 created" || no "messages.1 created"
[ -f "$D/lg/messages.3" ] && ok "messages.2 shifted to .3" || no "messages.2 shifted to .3"
[ ! -s "$D/lg/messages" ] && ok "current log empty after rotate" || no "current log empty after rotate"
"$D/lg/messages" 2>/dev/null || true

echo "rootfs checks:"
test -x rootfs/sbin/logrotate && ok "rootfs/sbin/logrotate executable" || no "rootfs/sbin/logrotate executable"
test -f rootfs/etc/syslog.conf && ok "rootfs/etc/syslog.conf exists" || no "rootfs/etc/syslog.conf exists"
grep -q '/var/log/kern.log' rootfs/etc/syslog.conf && ok "syslog.conf routes kern.log" || no "syslog.conf routes kern.log"
grep -q '/var/log/messages' rootfs/etc/syslog.conf && ok "syslog.conf routes messages" || no "syslog.conf routes messages"
test -d rootfs/etc/bulum/services/logrotate && ok "logrotate service defined" || no "logrotate service defined"

rm -rf "$D"
echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

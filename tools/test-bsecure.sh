#!/bin/bash
# BulumOS Phase 7: bsecure host-side test suite.
#
#   tools/test-bsecure.sh                          build host bsecure and run checks
#   BSECURE_BIN=/path/to/bsecure tools/test-bsecure.sh  use an existing host binary
#
# Builds a fixture root tree, audits it with --root + BSECURE_ANY_OWNER
# (rootfs files are owned by the build user), and checks the findings.
set -u
cd "$(dirname "$0")/.."

BIN="${BSECURE_BIN:-/tmp/bsecure-host}"
if [ -n "${BSECURE_BIN:-}" ]; then
    [ -x "$BIN" ] || { echo "bsecure binary not found: $BIN"; exit 1; }
    echo "== using $BIN =="
else
    echo "== building host bsecure =="
    gcc -O2 -Wall -o "$BIN" src/bsecure.c 2>build-bsecure.log || { echo "build failed:"; cat build-bsecure.log; exit 1; }
    rm -f build-bsecure.log
fi

"$BIN" --version > /dev/null 2>&1 || { echo "bsecure does not run"; exit 1; }
echo "== bsecure binary exists and runs =="

pass=0; fail=0; fails=()
ok(){ pass=$((pass+1)); echo "PASS  $1"; }
bad(){ fail=$((fail+1)); fails+=("$1"); echo "FAIL  $1${2:+ :: $2}"; }

T=$(mktemp -d /tmp/bsecure-test.XXXXXX)

echo
echo "--- fixture: hardened tree (all checks should pass) ---"
mkdir -p "$T/good/etc/dropbear" "$T/good/root" "$T/good/tmp" "$T/good/var/www"
chmod 700 "$T/good/etc/dropbear" "$T/good/root"
chmod 1777 "$T/good/tmp"
chmod 755 "$T/good/var/www"
echo '127.0.0.1 localhost' > "$T/good/etc/hosts"
echo '# resolv' > "$T/good/etc/resolv.conf"
echo 'root:x:0:0:root:/root:/bin/bsh' > "$T/good/etc/passwd"
echo 'httpd:x:110:110:httpd:/var/www:/bin/false' >> "$T/good/etc/passwd"
echo 'root:x:0:root' > "$T/good/etc/group"
echo 'httpd:x:110:' >> "$T/good/etc/group"
echo 'root:$6$abc:19000:0:99999:7:::' > "$T/good/etc/shadow"
chmod 600 "$T/good/etc/shadow" "$T/good/etc/passwd" "$T/good/etc/group"
printf '#!/bin/sh\n' > "$T/good/etc/dropbear/dropbear_rsa_host_key"
chmod 600 "$T/good/etc/dropbear/dropbear_rsa_host_key"

out=$(BSECURE_ANY_OWNER=1 "$BIN" audit --root "$T/good" 2>&1); rc=$?
if [ "$rc" -le 1 ]; then
    ok "hardened fixture audits clean (rc $rc: warnings ok, no critical)"
else
    bad "hardened fixture audits clean" "rc=$rc out=$(printf '%s' "$out" | head -c 200)"
fi
printf '%s' "$out" | grep -q "CRITICAL" && bad "no CRITICAL findings in hardened tree" || ok "no CRITICAL findings in hardened tree"

echo
echo "--- fixture: broken tree (findings must be reported) ---"
mkdir -p "$T/bad/etc/dropbear" "$T/bad/root" "$T/bad/var/www"
chmod 755 "$T/bad/root"          # /root must be 700
chmod 755 "$T/bad/etc/dropbear"  # key dir must be 700
chmod 1777 "$T/bad/tmp" 2>/dev/null || mkdir -p "$T/bad/tmp" && chmod 1777 "$T/bad/tmp"
chmod 644 "$T/bad/var/www"       # www dir must be 755 -> warning
echo '127.0.0.1 localhost' > "$T/bad/etc/hosts"
echo '# resolv' > "$T/bad/etc/resolv.conf"
echo 'root:x:0:0:root:/root:/bin/bsh' > "$T/bad/etc/passwd"
echo 'root:x:0:root' > "$T/bad/etc/group"
echo 'root:!:19000:0:99999:7:::' > "$T/bad/etc/shadow"
chmod 600 "$T/bad/etc/shadow" "$T/bad/etc/passwd" "$T/bad/etc/group"

out=$(BSECURE_ANY_OWNER=1 "$BIN" audit --root "$T/bad" 2>&1); rc=$?
printf '%s' "$out" | grep -qi "root.*7[0-9][0-9]\|/root" && ok "finds /root permissions" || bad "finds /root permissions" "$(printf '%s' "$out" | head -c 150)"
printf '%s' "$out" | grep -qi "host keys" && ok "finds SSH host key protection issue" || bad "finds SSH host key protection issue" "$(printf '%s' "$out" | head -c 150)"
printf '%s' "$out" | grep -qi "WARNING" && ok "reports WARNING-level findings" || bad "reports WARNING-level findings" "$(printf '%s' "$out" | head -c 150)"

echo
echo "--- status / help / bad args ---"
BSECURE_ANY_OWNER=1 "$BIN" status --root "$T/good" > /dev/null 2>&1 && ok "status --root works" || bad "status --root works"
out=$("$BIN" status 2>&1); rc=$?
if [ "$rc" -ge 0 ] && [ "$rc" -le 2 ]; then
    ok "status (host) runs (rc $rc, host findings expected)"
else
    bad "status (host) runs" "rc=$rc out=$(printf '%s' "$out" | head -c 100)"
fi
"$BIN" --help | grep -q "usage:" && ok "--help shows usage" || bad "--help shows usage"
"$BIN" bogus > /dev/null 2>&1 && bad "unknown subcommand -> nonzero" || ok "unknown subcommand -> nonzero"
"$BIN" audit > /dev/null 2>&1 && bad "audit without --root on foreign host -> nonzero" || ok "audit without --root on foreign host -> nonzero"

rm -rf "$T"

echo
echo "== $pass passed, $fail failed =="
if [ "$fail" -gt 0 ]; then
    echo "failed: $(IFS=', '; echo "${fails[*]}")"
    exit 1
fi
exit 0
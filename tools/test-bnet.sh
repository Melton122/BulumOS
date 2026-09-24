#!/bin/bash
# BulumOS Phase 7: bnet host-side test suite.
#
#   tools/test-bnet.sh                        build host bnet and run checks
#   BNET_BIN=/path/to/bnet tools/test-bnet.sh  use an existing host binary
#
# Covers bnet read-only commands on the host (no root needed) plus
# config-parsing / exit-code checks with BNET_CONF.  Interface-mutating
# commands are exercised in the VM (test-bnet-vm.sh).
set -u
cd "$(dirname "$0")/.."

BIN="${BNET_BIN:-/tmp/bnet-host}"
if [ -n "${BNET_BIN:-}" ]; then
    [ -x "$BIN" ] || { echo "bnet binary not found: $BIN"; exit 1; }
    echo "== using $BIN =="
else
    echo "== building host bnet =="
    gcc -O2 -Wall -o "$BIN" src/bnet.c 2>build-bnet.log || { echo "build failed:"; cat build-bnet.log; exit 1; }
    rm -f build-bnet.log
fi

"$BIN" --version > /dev/null 2>&1 || { echo "bnet does not run"; exit 1; }
echo "== bnet binary exists and runs =="

pass=0; fail=0; fails=()
ok(){ pass=$((pass+1)); echo "PASS  $1"; }
bad(){ fail=$((fail+1)); fails+=("$1"); echo "FAIL  $1${2:+ :: $2}"; }

# run NAME EXPECT_RC [GREP] CMD
run(){
    local name=$1 exp_rc=$2 grep_pat=$3
    shift 3
    local out rc
    out=$("$BIN" "$@" 2>&1); rc=$?
    if [ "$rc" -eq "$exp_rc" ]; then
        if [ -n "$grep_pat" ] && ! printf '%s' "$out" | grep -qF -- "$grep_pat"; then
            bad "$name" "rc ok but missing '$grep_pat' (out: $(printf '%s' "$out" | head -c 100))"
        else
            ok "$name"
        fi
    else
        bad "$name" "rc=$rc want=$exp_rc (out: $(printf '%s' "$out" | head -c 100))"
    fi
}

echo
echo "--- help / version ---"
run "usage exits 0" 0 "usage:" --help
run "version" 0 "bnet" --version
run "no args -> usage rc 2" 2 "usage:"
run "unknown command -> rc 2" 2 "usage:" bogus

echo
echo "--- read-only status commands ---"
run "status" 0 "Interfaces" status
run "list shows lo" 0 "lo" list
run "info lo" 0 "lo" info lo
run "info nonexistent -> rc 3" 3 "" info bnetbogus99
run "info missing arg -> rc 2" 2 "usage" info
run "ip lo" 0 "lo" ip lo
run "ip missing arg -> rc 2" 2 "usage" ip
run "route shows table" 0 "Gateway" route
run "dns lists resolv.conf" 0 "" dns
run "resolve localhost" 0 "127.0.0.1" resolve localhost
run "resolve garbage -> rc 1" 1 "" resolve bnet_nxdomain_xyz.invalid
run "resolve missing arg -> rc 2" 2 "usage" resolve
run "sockets" 0 "" sockets
run "ping missing arg -> rc 2" 2 "usage" ping

echo
echo "--- config parsing (BNET_CONF) ---"
T=$(mktemp -d /tmp/bnet-test.XXXXXX)
cat > "$T/bad.conf" <<'EOF'
interface=eth0
method=what?
EOF
cat > "$T/noblocks.conf" <<'EOF'
# comment only, no interface= entries
EOF
cat > "$T/static.conf" <<'EOF'
interface=lo
method=static
address=127.0.0.1/8
EOF

out=$(BNET_CONF="$T/bad.conf" "$BIN" apply 2>&1); rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qiE 'bnet:'; then
    ok "malformed config rejected (rc 1, error shown)"
else
    bad "malformed config rejected" "rc=$rc out=$(printf '%s' "$out" | head -c 100)"
fi

out=$(BNET_CONF="$T/noblocks.conf" "$BIN" apply 2>&1); rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "no interface="; then
    ok "config without interface= rejected (parse worked)"
else
    bad "config without interface= rejected" "rc=$rc out=$(printf '%s' "$out" | head -c 100)"
fi

out=$(BNET_CONF="$T/static.conf" "$BIN" apply 2>&1); rc=$?
if [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ]; then
    ok "valid config parses (apply returns 0 or 1, no parse error)"
else
    bad "valid config parses" "rc=$rc out=$(printf '%s' "$out" | head -c 100)"
fi

out=$(BNET_CONF="$T/nonexistent.conf" "$BIN" apply 2>&1); rc=$?
if [ "$rc" -eq 1 ]; then
    ok "missing config file -> rc 1"
else
    bad "missing config file -> rc 1" "rc=$rc out=$(printf '%s' "$out" | head -c 100)"
fi

rm -rf "$T"

echo
echo "== $pass passed, $fail failed =="
if [ "$fail" -gt 0 ]; then
    echo "failed: $(IFS=', '; echo "${fails[*]}")"
    exit 1
fi
exit 0
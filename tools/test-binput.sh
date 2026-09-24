#!/bin/bash
# BulumOS Phase 8: input host-side test suite.
#
#   tools/test-binput.sh                         build host binary and run checks
#   BINPUT_BIN_DIR=/dir tools/test-binput.sh      use an existing host binary
#
# Covers: existence, --help/--version/--list, headless behavior,
# malformed device data (fake event file with garbage must not crash),
# and leak checks (valgrind when available).
set -u
cd "$(dirname "$0")/.."

BIN="${BINPUT_BIN_DIR:-/tmp/bgfx-host}"
mkdir -p "$BIN"
if [ -z "${BINPUT_BIN_DIR:-}" ]; then
    echo "== building host binputtest =="
    gcc -O2 -Wall -o "$BIN/binputtest" src/binputtest.c src/binput.c || { echo "build binputtest failed"; exit 1; }
fi
[ -x "$BIN/binputtest" ] || { echo "binputtest not found in $BIN"; exit 1; }

pass=0; fail=0; fails=()
ok(){ pass=$((pass+1)); echo "PASS  $1"; }
bad(){ fail=$((fail+1)); fails+=("$1"); echo "FAIL  $1${2:+ :: $2}"; }

# run NAME EXPECT_RC [GREP] CMD...
run() {
    local name=$1 want=$2 grepfor=$3; shift 3
    local out rc
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" != "$want" ]; then
        bad "$name" "rc=$rc want=$want: $out"; return
    fi
    if [ -n "$grepfor" ] && ! echo "$out" | grep -q "$grepfor"; then
        bad "$name" "missing '$grepfor' in: $out"; return
    fi
    ok "$name"
}

# 1. existence + options
"$BIN/binputtest" --version >/dev/null 2>&1 && ok "binputtest runs" || bad "binputtest runs"
run "binputtest --version" 0 "binputtest" "$BIN/binputtest" --version
run "binputtest --help" 0 "usage" "$BIN/binputtest" --help
run "binputtest badarg" 2 "usage" "$BIN/binputtest" --bogus

# 2. headless / empty input dir
mkdir -p /tmp/bgfx-empty-input
run "no devices" 1 "none" env BINPUT_DIR=/tmp/bgfx-empty-input "$BIN/binputtest"
run "no devices --list" 1 "none" env BINPUT_DIR=/tmp/bgfx-empty-input "$BIN/binputtest" --list
run "missing dir" 1 "none" env BINPUT_DIR=/nonexistent/input "$BIN/binputtest" --list
run "timeout no devices" 1 "none" env BINPUT_DIR=/tmp/bgfx-empty-input "$BIN/binputtest" --timeout 1

# 3. malformed device data: a garbage file in the input dir must not crash
mkdir -p /tmp/bgfx-malformed
head -c 64 /dev/urandom > /tmp/bgfx-malformed/event0
run "malformed device listed" 0 "Input devices" env BINPUT_DIR=/tmp/bgfx-malformed "$BIN/binputtest" --list
echo "garbage that is not a kernel event frame" > /tmp/bgfx-malformed/event1
run "malformed device poll" 0 "exit" env BINPUT_DIR=/tmp/bgfx-malformed "$BIN/binputtest" --timeout 1

# 3b. classification via a fake sysfs tree (event1 kbd, event2 mouse)
rm -rf /tmp/bgfx-sysfs /tmp/bgfx-sysdev
mkdir -p /tmp/bgfx-sysfs/event1 /tmp/bgfx-sysfs/event2 \
         /tmp/bgfx-sysfs/input1/capabilities /tmp/bgfx-sysfs/input2/capabilities \
         /tmp/bgfx-sysdev
touch /tmp/bgfx-sysdev/event1 /tmp/bgfx-sysdev/event2
ln -sfn ../../input1 /tmp/bgfx-sysfs/event1/device
ln -sfn ../../input2 /tmp/bgfx-sysfs/event2/device
echo -n "QEMU Virtio Keyboard" > /tmp/bgfx-sysfs/input1/name
echo -n "QEMU Virtio Mouse" > /tmp/bgfx-sysfs/input2/name
echo -n "120003" > /tmp/bgfx-sysfs/input1/capabilities/ev
echo -n "7" > /tmp/bgfx-sysfs/input2/capabilities/ev
echo -n "400000007 ff803078f800dfff febeffff7bcfffff fffffffffffffffe" \
    > /tmp/bgfx-sysfs/input1/capabilities/key
echo -n "30400 1f0000 0 0 0 0" > /tmp/bgfx-sysfs/input2/capabilities/key
echo -n "103" > /tmp/bgfx-sysfs/input2/capabilities/rel
run "classify keyboard" 0 "keyboard" env BINPUT_DIR=/tmp/bgfx-sysdev \
    BINPUT_SYSFS=/tmp/bgfx-sysfs "$BIN/binputtest" --list
run "classify mouse" 0 "mouse" env BINPUT_DIR=/tmp/bgfx-sysdev \
    BINPUT_SYSFS=/tmp/bgfx-sysfs "$BIN/binputtest" --list
run "classify names" 0 "QEMU Virtio Keyboard" env BINPUT_DIR=/tmp/bgfx-sysdev \
    BINPUT_SYSFS=/tmp/bgfx-sysfs "$BIN/binputtest" --list

# 4. real /dev/input if the host has one (WSL may expose none)
if [ -d /dev/input ]; then
    n=$("$BIN/binputtest" --list 2>/dev/null | grep -c /dev/input || true)
    echo "INFO  host /dev/input devices: $n"
fi

# 5. leaks (valgrind when available)
if command -v valgrind >/dev/null 2>&1; then
    valgrind --error-exitcode=99 --leak-check=full -q "$BIN/binputtest" --list >/dev/null 2>/tmp/vg4.log \
        && ok "valgrind binputtest" || bad "valgrind binputtest" "$(head -3 /tmp/vg4.log)"
    valgrind --error-exitcode=99 --leak-check=full -q env BINPUT_DIR=/tmp/bgfx-malformed \
        "$BIN/binputtest" --timeout 1 >/dev/null 2>/tmp/vg5.log \
        && ok "valgrind malformed input" || bad "valgrind malformed input" "$(head -3 /tmp/vg5.log)"
else
    echo "SKIP  valgrind not installed"
fi

echo
echo "== $pass passed, $fail failed =="
[ $fail -eq 0 ] || { printf 'failed: %s\n' "${fails[@]}"; exit 1; }
exit 0
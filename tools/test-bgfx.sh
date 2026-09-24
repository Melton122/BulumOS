#!/bin/bash
# BulumOS Phase 8: graphics host-side test suite.
#
#   tools/test-bgfx.sh                        build host binaries and run checks
#   BGFX_BIN_DIR=/dir tools/test-bgfx.sh       use existing host binaries
#
# Covers: existence, --help/--version, headless detection ("Graphics:
# unavailable" must be a normal result), invalid device/backend error
# paths, config parsing, and leak checks (valgrind when available).
set -u
cd "$(dirname "$0")/.."

BIN="${BGFX_BIN_DIR:-/tmp/bgfx-host}"
mkdir -p "$BIN"
if [ -z "${BGFX_BIN_DIR:-}" ]; then
    echo "== building host binaries =="
    gcc -O2 -Wall -o "$BIN/bgfxinfo" src/bgfxinfo.c src/bgfx.c || { echo "build bgfxinfo failed"; exit 1; }
    gcc -O2 -Wall -o "$BIN/bgfxdemo" src/bgfxdemo.c src/bgfx.c || { echo "build bgfxdemo failed"; exit 1; }
    gcc -O2 -Wall -o "$BIN/bgfx" src/bgfxcli.c src/bgfx.c src/binput.c || { echo "build bgfx failed"; exit 1; }
fi
[ -x "$BIN/bgfxinfo" ] && [ -x "$BIN/bgfxdemo" ] && [ -x "$BIN/bgfx" ] || {
    echo "binaries not found in $BIN"; exit 1; }

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

# 1. existence + versions
"$BIN/bgfxinfo" --version >/dev/null 2>&1 && ok "bgfxinfo runs" || bad "bgfxinfo runs"
run "bgfxinfo --version" 0 "bgfxinfo" "$BIN/bgfxinfo" --version
run "bgfxinfo --help" 0 "usage" "$BIN/bgfxinfo" --help
run "bgfxdemo --version" 0 "bgfxdemo" "$BIN/bgfxdemo" --version
run "bgfx --version" 0 "bgfx" "$BIN/bgfx" --version
run "bgfx --help" 0 "status" "$BIN/bgfx" --help
run "bgfx noargs" 2 "usage" "$BIN/bgfx"

# 2. headless detection (host has no /dev/fb0 in most CI environments;
#    both outcomes are accepted, the unavailable path must be clean)
out=$("$BIN/bgfxinfo" 2>&1); rc=$?
if [ "$rc" -eq 0 ]; then
    echo "$out" | grep -q "Graphics" && ok "bgfxinfo report header" || bad "bgfxinfo report header"
else
    run "headless report" 1 "unavailable" "$BIN/bgfxinfo"
    run "headless bgfx status" 1 "Console mode" "$BIN/bgfx" status
    run "headless bgfxdemo" 1 "unavailable" "$BIN/bgfxdemo"
fi

# 3. invalid backend / invalid device handling
run "unknown backend" 1 "unknown backend" "$BIN/bgfxdemo" --backend bogus
run "drm backend honest" 1 "not implemented" "$BIN/bgfxdemo" --backend drm
echo "backend=drm" > /tmp/bgfx-drm.conf
run "drm status honest" 1 "not implemented" env BGFX_CONF=/tmp/bgfx-drm.conf "$BIN/bgfx" status
echo "not a framebuffer" > /tmp/bgfx-fakefb.txt
BGFX_FB_PATH=/tmp/bgfx-fakefb.txt run "invalid fb device" 1 "FSCREENINFO" "$BIN/bgfxdemo"
BGFX_FB_PATH=/nonexistent/fb0 run "missing fb device" 1 "unavailable" "$BIN/bgfxdemo"

# 4. config parsing
cat > /tmp/bgfx-test.conf <<'EOF'
# comment
backend=framebuffer
resolution=auto
input=auto
boguskey=1
EOF
run "config backend=framebuffer" 1 "Framebuffer\|unavailable" env BGFX_CONF=/tmp/bgfx-test.conf "$BIN/bgfx" status
cat > /tmp/bgfx-bad.conf <<'EOF'
backend=wibble
resolution=800x600
input=no
EOF
run "config bad values warn" 1 "warning" env BGFX_CONF=/tmp/bgfx-bad.conf "$BIN/bgfx" status

# 5. leaks (valgrind when available)
if command -v valgrind >/dev/null 2>&1; then
    valgrind --error-exitcode=99 --leak-check=full -q "$BIN/bgfxinfo" >/dev/null 2>/tmp/vg1.log \
        && ok "valgrind bgfxinfo" || bad "valgrind bgfxinfo" "$(head -3 /tmp/vg1.log)"
    valgrind --error-exitcode=99 --leak-check=full -q "$BIN/bgfxdemo" >/dev/null 2>/tmp/vg2.log \
        && ok "valgrind bgfxdemo" || bad "valgrind bgfxdemo" "$(head -3 /tmp/vg2.log)"
    valgrind --error-exitcode=99 --leak-check=full -q "$BIN/bgfx" status >/dev/null 2>/tmp/vg3.log \
        && ok "valgrind bgfx" || bad "valgrind bgfx" "$(head -3 /tmp/vg3.log)"
else
    echo "SKIP  valgrind not installed"
fi

echo
echo "== $pass passed, $fail failed =="
[ $fail -eq 0 ] || { printf 'failed: %s\n' "${fails[@]}"; exit 1; }
exit 0
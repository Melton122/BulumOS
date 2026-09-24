#!/bin/bash
# test-bpk.sh - host-side verification for the Phase 12 package manager.
#
#   1. compiles src/bulum.c with sandboxed paths (sed rewrite) into a host
#      binary, mirroring tools/verify.sh's approach (safe: read-only tests).
#   2. runs bpk_coretest (vercmp / arch matrix / field parsing / JSON escape).
#   3. drives the CLI against a throwaway sandbox: arch, --json list/search/
#      info/verify/owns/version, and repo add/remove/enable/disable/list.
#
# Real install/remove/upgrade flows are exercised in the VM test
# (test-bpk-vm.sh) where / is the guest root.
set -e
cd /home/melton/BulumOS
ROOT=$(pwd)
SB=/tmp/bpksv
BIN=/tmp/bpk-host

echo "=== bpk host harness ==="
rm -rf "$SB"
mkdir -p "$SB/etc/bulum/keys" \
         "$SB/var/lib/dpkg/info" \
         "$SB/var/lib/bulum/lists" \
         "$SB/var/cache/bpk" \
         "$SB/var/lib/bulum/stage" \
         "$SB/var/lib/bulum/backup" \
         "$SB/var/lib/bulum/tx" \
         "$SB/var/log/bulum" \
         "$SB/bin"

# ---- sandboxed copy of bulum.c ----
sed -e 's|/etc/bulum/keys|/tmp/bpksv/etc/bulum/keys|g' \
    -e 's|/etc/bulum/repos.conf|/tmp/bpksv/etc/bulum/repos.conf|g' \
    -e 's|/var/lib/dpkg/status|/tmp/bpksv/var/lib/dpkg/status|g' \
    -e 's|/var/lib/dpkg/info|/tmp/bpksv/var/lib/dpkg/info|g' \
    -e 's|/var/lib/bulum/lists|/tmp/bpksv/var/lib/bulum/lists|g' \
    -e 's|/var/lib/bulum/stage|/tmp/bpksv/var/lib/bulum/stage|g' \
    -e 's|/var/lib/bulum/backup|/tmp/bpksv/var/lib/bulum/backup|g' \
    -e 's|/var/lib/bulum/tx|/tmp/bpksv/var/lib/bulum/tx|g' \
    -e 's|/var/cache/bpk|/tmp/bpksv/var/cache/bpk|g' \
    -e 's|/var/log/bulum|/tmp/bpksv/var/log/bulum|g' \
    src/bulum.c > /tmp/bulum-bpk.c

# ---- part 1: pure logic ----
gcc -O2 -w -I src -I /tmp -o "$BIN-core" tools/bpk_coretest.c src/bulum_crypto.c || {
    echo "coretest build failed"; exit 1; }
"$BIN-core"

# ---- part 2: CLI in the sandbox ----
gcc -O2 -w -I src -o "$BIN" /tmp/bulum-bpk.c src/bulum_crypto.c 2>/dev/null

PASS=0
pass() { PASS=$((PASS+1)); }

# sandbox repo index (two repos; one exercise arch variety)
cat > "$SB/etc/bulum/repos.conf" <<'EOF'
# BulumOS phase-12 test config
main http://test.invalid/main aarch64 /tmp/bpksv/etc/bulum/keys/main.pub
extra http://test.invalid/extra all -
EOF
cat > "$SB/var/lib/bulum/lists/main.Packages" <<'EOF'
Package: hello
Version: 1.1
Architecture: all
Description: test (main repo)
Format: bpk
Filename: hello_1.1_all.bpk
Size: 10
SHA256: 0000000000000000000000000000000000000000000000000000000000000000

Package: htune
Version: 1.0
Architecture: aarch64
Description: arm only (main repo)
Format: bpk
Filename: htune_1.0_aarch64.bpk
Size: 10
SHA256: 0000000000000000000000000000000000000000000000000000000000000000
EOF
cat > "$SB/var/lib/bulum/lists/extra.Packages" <<'EOF'
Package: hello
Version: 1.0
Architecture: all
Description: older from extra repo
Format: bpk
Filename: hello_1.0_all.bpk
Size: 10
SHA256: 0000000000000000000000000000000000000000000000000000000000000000
EOF

# one installed package in the sandbox db, owning a sandbox file
printf '#!/bin/sh\necho base-shell\n' > "$SB/bin/bsh"
cat > "$SB/var/lib/dpkg/status" <<'EOF'
Package: bsh
Status: install ok installed
Description: the shell
Architecture: all
Version: 1.0.0

Package: bxo
Status: install ok installed
Description: x86-only base
Architecture: x86_64
Version: 1.0.0
EOF
echo "/tmp/bpksv/bin/bsh" > "$SB/var/lib/dpkg/info/bsh.list"
md5sum "$SB/bin/bsh" | awk '{print $1"  tmp/bpksv/bin/bsh"}' > "$SB/var/lib/dpkg/info/bsh.md5sums"

# --- arch ---
[ "$(BULUM_ARCH=aarch64 $BIN arch)" = "aarch64" ] && pass || echo "FAIL arch override"
[ "$(BULUM_ARCH=x86_64 $BIN arch)" = "x86_64" ] && pass || echo "FAIL arch x86"
BULUM_ARCH=x86_64 $BIN --json arch | grep -q '"arch":"x86_64"' && pass || echo "FAIL json arch"

# --- repo list / json ---
$BIN repo list >/dev/null && pass || echo "FAIL repo list"
$BIN --json repo list | grep -q '"repos":\[{' && pass || echo "FAIL repo list json"
$BIN --json repo list | grep -q '"enabled":true' && pass || echo "FAIL repo enabled field"

# --- repo add / remove ---
$BIN repo add newrepo http://a.b/c all - 2>/dev/null && pass || echo "FAIL repo add"
$BIN repo add bad url 2>/dev/null && { echo "FAIL repo add bad url accepted"; } || pass
$BIN repo remove newrepo >/dev/null && pass || echo "FAIL repo remove"
$BIN repo remove nonexistent 2>/dev/null && { echo "FAIL repo remove missing"; } || pass

# --- repo disable / enable roundtrip ---
$BIN repo disable main >/dev/null && pass || echo "FAIL repo disable"
grep -qE "^main .*disabled" "$SB/etc/bulum/repos.conf" || echo "FAIL disable wrote flag"
$BIN repo enable main >/dev/null && pass || echo "FAIL repo enable"
grep -E "^main " "$SB/etc/bulum/repos.conf" | grep -qv disabled && pass || echo "FAIL enable cleared flag"
$BIN repo disable main >/dev/null   # leave disabled so update-skip path is covered later
grep -q "disabled" "$SB/etc/bulum/repos.conf" && pass || echo "FAIL re-disable"

# --- search / json ---
BULUM_ARCH=aarch64 $BIN search hello >/dev/null && pass || echo "FAIL search hello"
n=$($BIN --json search hello | tr -dc ',' | wc -c)
[ "$n" -ge 2 ] || echo "FAIL search json shape"

# --- info (installed + available) ---
BULUM_ARCH=aarch64 $BIN --json info bsh | grep -q '"origin"' ; true
BULUM_ARCH=aarch64 $BIN --json info bsh | grep -q '"installed":true' && pass || echo "FAIL info json installed"

# --- list ---
BULUM_ARCH=aarch64 $BIN --json list | grep -q '"name":"bsh"' && pass || echo "FAIL list json"

# --- owns / verify ---
BULUM_ARCH=aarch64 $BIN owns /tmp/bpksv/bin/bsh >/dev/null && pass || echo "FAIL owns"
$BIN --json owns /tmp/bpksv/bin/bsh | grep -q '"owner":"bsh"' && pass || echo "FAIL owns json"
BULUM_ARCH=aarch64 $BIN owns /tmp/nonexistent 2>/dev/null && { echo "FAIL owns missing"; } || pass
BULUM_ARCH=aarch64 $BIN verify bsh >/dev/null && pass || echo "FAIL verify bsh (unmodified)"
$BIN --json verify bsh | grep -q '"modified":0' && pass || echo "FAIL verify json"

# --- version ---
$BIN version | grep -q "1.3" && pass || echo "FAIL version string"

echo
echo "=== bpk host harness: $PASS checks passed ==="
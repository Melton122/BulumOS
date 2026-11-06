#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
set -e
cd "$ROOT_DIR"

T=$(mktemp -d /tmp/bulum-security-defaults.XXXXXX)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/image/etc"

fail() {
    echo "FAIL  $1" >&2
    exit 1
}

BULUMOS_ROOTFS="$T/image" BULUMOS_ROOT_PASSWORD='test-only-password' \
    bash tools/mk-shadow.sh
ROOT_HASH=$(awk -F: '$1 == "root" { print $2 }' "$T/image/etc/shadow")
ROOT_SALT=$(printf '%s\n' "$ROOT_HASH" | cut -d '$' -f 3)
EXPECTED_HASH=$(printf '%s\n' 'test-only-password' |
    openssl passwd -6 -salt "$ROOT_SALT" -stdin)
[ "$ROOT_HASH" = "$EXPECTED_HASH" ] || fail "generated root hash matches the supplied password"
[ "$(awk -F: '$1 == "melton" { print $2 }' "$T/image/etc/shadow")" = "!" ] ||
    fail "default melton account has no password"
[ "$(stat -c '%a' "$T/image/etc/shadow")" = 600 ] || fail "shadow mode is 600"

if env -u BULUMOS_ROOT_PASSWORD BULUMOS_ROOTFS="$T/image" \
    bash tools/mk-shadow.sh </dev/null >/dev/null 2>&1; then
    fail "noninteractive image build rejects a missing root password"
fi

mkdir -p "$T/services/etc/bulum"
BULUMOS_ROOTFS="$T/services" bash tools/install-services.sh
[ ! -e "$T/services/etc/bulum/enabled/telnetd" ] ||
    fail "Telnet is not enabled by default"
grep -q 'dropbear -s ' "$T/services/etc/bulum/services/dropbear/control" ||
    fail "Dropbear disables password authentication"

for rootfs in rootfs rootfs-x86; do
    [ ! -e "$rootfs/etc/bulum/enabled/telnetd" ] ||
        fail "$rootfs does not enable Telnet by default"
done

echo "PASS  per-build root password and secure remote-service defaults"

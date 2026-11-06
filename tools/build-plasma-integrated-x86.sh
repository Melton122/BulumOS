#!/bin/bash
set -Eeuo pipefail

# Build an integrated BulumOS x86-64 image that boots into Plasma from the
# normal rootfs-x86, rather than a separate out/plasma-x86-image.
#
# Steps:
#  1. Integrate the plasma-x86 stage into rootfs-x86 (via integrate-plasma-x86.sh)
#  2. Rebuild the x86 disk.img from the integrated rootfs-x86
#  3. Rebuild the x86 UEFI image (bulumos-uefi-x86.img)
#  4. Rebuild the x86 ISO (bulumos-x86.iso)
#  5. Verify the integrated rootfs-x86 passes basic checks
#
# This does NOT modify the aarch64 rootfs or artifacts.

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
RFS="$ROOT_DIR/rootfs-x86"
STAGE="${BULUMOS_BUILD_DIR:-$ROOT_DIR/out/plasma-x86}"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

step() { echo -e "\n${GREEN}== $* ==${NC}"; }
ok()   { echo -e "${GREEN}PASS: $*${NC}"; }
fail() { echo -e "${RED}FAIL: $*${NC}" >&2; }

usage() {
    cat <<'EOF'
Usage: build-plasma-integrated-x86.sh [--help]

Build an integrated BulumOS x86-64 image that boots into Plasma from the
normal rootfs-x86 (not a separate standalone image).

Prerequisites:
  - rootfs-x86 exists with BulumOS native tools (bctl, buser, etc.)
  - A plasma-x86 stage exists at out/plasma-x86 (built with build_kde.sh --build)
  - x86-64 kernel at kernel/arch/x86/boot/bzImage
  - Switch initramfs at initramfs-switch-x86.cpio.gz
  - Run as root (for mount and file operations)

The script:
  1. Integrates plasma-x86 into rootfs-x86
  2. Rebuilds disk-x86.img from the integrated rootfs
  3. Rebuilds bulumos-uefi-x86.img and bulumos-x86.iso
  4. Runs verification checks on the integrated rootfs
EOF
}

case "${1:-}" in
    --help|-h) usage; exit 0 ;;
    "") : ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
esac

if [[ "$EUID" -ne 0 ]]; then
    echo "Run as root." >&2
    exit 1
fi

step "Checking prerequisites"
failed=0

# Check rootfs-x86
if [[ ! -d "$RFS" ]]; then
    fail "rootfs-x86 not found at $RFS"
    failed=1
fi

# Check plasma stage (out/plasma-x86 is an extracted root tree, not root.img)
if [[ ! -x "$STAGE/usr/bin/plasmashell" ]]; then
    fail "plasma-x86 stage not found or incomplete at $STAGE. Run build_kde.sh --build first."
    failed=1
fi

# Check kernel
if [[ ! -f "$ROOT_DIR/kernel/arch/x86/boot/bzImage" ]]; then
    fail "x86-64 kernel not found at kernel/arch/x86/boot/bzImage"
    failed=1
fi

# Check initramfs
if [[ ! -f "$ROOT_DIR/initramfs-switch-x86.cpio.gz" ]]; then
    fail "Switch initramfs not found at initramfs-switch-x86.cpio.gz"
    failed=1
fi

if [[ $failed -ne 0 ]]; then
    exit 1
fi

step "1. Integrating Plasma into rootfs-x86"
bash "$ROOT_DIR/tools/integrate-plasma-x86.sh"
ok "Plasma integrated into rootfs-x86"

step "2. Verifying integrated rootfs-x86"
failed=0

# Check critical binaries exist and are executable
for bin in sbin/init bin/bsh sbin/bctl usr/bin/buser; do
    if [[ ! -x "$RFS/$bin" ]]; then
        fail "Missing executable: $bin"
        failed=1
    fi
done

# Check Plasma binaries
for bin in usr/bin/plasmashell usr/bin/startplasma-x11 usr/bin/kwin_x11 \
           usr/bin/konsole usr/bin/systemsettings usr/bin/Xorg \
           usr/bin/dbus-daemon usr/sbin/bulum-plasma-session; do
    if [[ ! -x "$RFS/$bin" ]]; then
        fail "Missing Plasma executable: $bin"
        failed=1
    fi
done

# Check dynamic loader
if [[ ! -f "$RFS/lib64/ld-linux-x86-64.so.2" ]]; then
    fail "Missing x86-64 dynamic loader"
    failed=1
fi

# Check required configs
for cfg in etc/polkit-1/rules.d/50-bulum-user-management.rules \
           etc/X11/xorg.conf \
           etc/bulum/default-user \
           home/melton/.config/kwinrc; do
    if [[ ! -f "$RFS/$cfg" ]]; then
        fail "Missing config: $cfg"
        failed=1
    fi
done

# Check D-Bus service files
for svc in usr/share/dbus-1/system-services/org.freedesktop.Accounts.service \
           usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service; do
    if [[ ! -f "$RFS/$svc" ]]; then
        fail "Missing D-Bus service: $svc"
        failed=1
    fi
done

if [[ $failed -eq 0 ]]; then
    ok "Integrated rootfs-x86 passes verification checks"
else
    fail "rootfs-x86 verification failed (see above)"
    exit 1
fi

step "3. Rebuilding disk-x86.img from integrated rootfs-x86"
cd "$ROOT_DIR"
bash tools/populate-disk.sh x86 2>&1 | tail -5
if [[ -f "$ROOT_DIR/disk-x86.img" ]]; then
    ok "disk-x86.img rebuilt: $(du -h disk-x86.img | cut -f1)"
else
    fail "disk-x86.img was not created"
    exit 1
fi

step "4. Rebuilding bulumos-uefi-x86.img"
cd "$ROOT_DIR"
bash build-uefi-x86.sh 2>&1 | tail -10
if [[ -f "$ROOT_DIR/bulumos-uefi-x86.img" ]]; then
    ok "bulumos-uefi-x86.img rebuilt: $(du -h bulumos-uefi-x86.img | cut -f1)"
else
    fail "bulumos-uefi-x86.img was not created"
    exit 1
fi

step "5. Verifying bulumos-x86.iso"
# build-uefi-x86.sh already produced the hybrid ISO alongside the UEFI image.
if [[ -f "$ROOT_DIR/bulumos-x86.iso" ]]; then
    ok "bulumos-x86.iso rebuilt: $(du -h bulumos-x86.iso | cut -f1)"
else
    fail "bulumos-x86.iso was not created"
    exit 1
fi

step "6. Verification summary"
echo ""
echo "  Integrated rootfs-x86: $RFS"
echo "  Disk image: $ROOT_DIR/disk-x86.img"
echo "  UEFI image: $ROOT_DIR/bulumos-uefi-x86.img"
echo "  ISO: $ROOT_DIR/bulumos-x86.iso"
echo ""
echo "  The normal BulumOS x86-64 image now boots into KDE Plasma."
echo "  The bdesktop service is disabled; use 'bctl start desktop' for the native shell."
echo ""
echo "  To test interactively:"
echo "    bash tools/run-desktop-x86.sh"
echo ""
echo "  To capture a screenshot:"
echo "    bash tools/kde/screenshot-x86.sh"
echo ""
echo "  To run the user-flow test (input + shutdown):"
echo "    bash tools/kde/test-user-flow.sh"
echo ""

step "Build complete."

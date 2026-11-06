#!/bin/bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export BULUMOS_ROOT="${BULUMOS_ROOT:-$ROOT_DIR}"
source "$BULUMOS_ROOT/tools/toolchain/setup_kde_port.sh"

usage() {
    cat <<'EOF'
Usage: build_kde.sh [--check | --build | --image | --help]

  --check  Check host tools, architecture, and stage destination.
  --build  Bootstrap an isolated Ubuntu 24.04 x86-64 Plasma runtime under out/.
  --image  Create a bootable x86-64 kernel/initramfs/root-disk set under out/.

The build requires root (for debootstrap/chroot) and network access. It does
not modify rootfs-x86 or install packages into the WSL host.
EOF
}

check_prerequisites() {
    local failed=0
    local tool

    for tool in debootstrap chroot apt-get dpkg dpkg-query dpkg-deb gcc; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            printf 'missing build tool: %s\n' "$tool" >&2
            failed=1
        fi
    done

    if [[ "$(dpkg --print-architecture 2>/dev/null)" != amd64 ]]; then
        printf 'build must run on an amd64 host.\n' >&2
        failed=1
    fi

    if [[ ! -d "$BULUMOS_ROOT/rootfs-x86/etc/bulum" ]]; then
        printf 'missing BulumOS x86-64 source rootfs: %s\n' \
            "$BULUMOS_ROOT/rootfs-x86" >&2
        failed=1
    fi

    if [[ "$EUID" -ne 0 ]]; then
        printf 'run this build as root (required by debootstrap and chroot).\n' >&2
        failed=1
    fi

    if [[ -e "$BULUMOS_BUILD_DIR" &&
          ( ! -f "$BULUMOS_BUILD_DIR/var/lib/dpkg/status" ||
            ! -x "$BULUMOS_BUILD_DIR/usr/bin/plasmashell" ) ]]; then
        printf 'existing destination is not a resumable Plasma stage: %s\n' \
            "$BULUMOS_BUILD_DIR" >&2
        failed=1
    fi

    return "$failed"
}

case "${1:---check}" in
    --help|-h)
        usage
        exit 0
        ;;
    --check)
        if check_prerequisites; then
            printf 'x86-64 KDE build prerequisites are present.\n'
        else
            printf 'x86-64 KDE build is not ready; no files were changed.\n' >&2
            exit 1
        fi
        ;;
    --build)
        check_prerequisites || {
            printf 'x86-64 KDE build aborted before staging any files.\n' >&2
            exit 1
        }
        exec bash "$BULUMOS_ROOT/tools/kde/build.sh"
        ;;
    --image)
        if [[ "$EUID" -ne 0 ]]; then
            printf 'run this image build as root.\n' >&2
            exit 1
        fi
        exec bash "$BULUMOS_ROOT/tools/kde/build-image.sh"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

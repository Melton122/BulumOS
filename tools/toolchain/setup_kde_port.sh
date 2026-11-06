#!/bin/bash

_BULUMOS_KDE_TOOLCHAIN_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd
)"
export BULUMOS_ROOT="${BULUMOS_ROOT:-$(
    cd -- "$_BULUMOS_KDE_TOOLCHAIN_DIR/../.." && pwd
)}"
export BULUMOS_ARCH=x86_64
export BULUMOS_TARGET_TRIPLE=x86_64-linux-gnu
export BULUMOS_BUILD_DIR="${BULUMOS_BUILD_DIR:-$BULUMOS_ROOT/out/plasma-x86}"

unset _BULUMOS_KDE_TOOLCHAIN_DIR

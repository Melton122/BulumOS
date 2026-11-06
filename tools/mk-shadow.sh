#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
set -e

ROOTFS_DIR=${BULUMOS_ROOTFS:-"$ROOT_DIR/rootfs"}
if [ -z "${BULUMOS_ROOT_PASSWORD:-}" ]; then
    if [ ! -t 0 ]; then
        echo "BULUMOS_ROOT_PASSWORD must be set when stdin is not a terminal" >&2
        exit 1
    fi
    read -r -s -p "Choose a root password (at least 12 characters): " ROOT_PASSWORD
    echo
    read -r -s -p "Confirm root password: " ROOT_PASSWORD_CONFIRM
    echo
    [ "$ROOT_PASSWORD" = "$ROOT_PASSWORD_CONFIRM" ] || {
        echo "root passwords do not match" >&2
        exit 1
    }
else
    ROOT_PASSWORD=$BULUMOS_ROOT_PASSWORD
fi
unset BULUMOS_ROOT_PASSWORD

if [ "${#ROOT_PASSWORD}" -lt 12 ]; then
    echo "root password must be at least 12 characters" >&2
    exit 1
fi

ROOT_HASH=$(printf '%s\n' "$ROOT_PASSWORD" | openssl passwd -6 -stdin)
unset ROOT_PASSWORD ROOT_PASSWORD_CONFIRM

SHADOW_TMP=$(mktemp "$ROOTFS_DIR/etc/shadow.XXXXXX")
trap 'rm -f "$SHADOW_TMP"' EXIT
printf 'root:%s:19786:0:99999:7:::\nmelton:!:19786:0:99999:7:::\n' \
    "$ROOT_HASH" > "$SHADOW_TMP"
chmod 600 "$SHADOW_TMP"
mv "$SHADOW_TMP" "$ROOTFS_DIR/etc/shadow"
trap - EXIT

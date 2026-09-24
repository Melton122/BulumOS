#!/bin/bash
# seed-rootfs-db.sh - write a complete dpkg-compatible package database into a
# rootfs so the base OS files are owned by their official packages (they can be
# removed/updated via bpk and the ownership map is correct from first boot).
#
# Reads packages/*.pk manifests and mirrors, for each, a status record, a
# <pkg>.list and a <pkg>.md5sums under <rfs>/var/lib/dpkg. It only manages
# regular files (symlinks such as busybox applets are left unmanaged).
#
# usage: tools/seed-rootfs-db.sh <aarch64|x86_64>
set -e
cd /home/melton/BulumOS
case "$1" in
  aarch64|arm64) ARCH=aarch64; RFS=rootfs ;;
  x86_64|amd64)  ARCH=x86_64;  RFS=rootfs-x86 ;;
  *) echo "usage: seed-rootfs-db.sh <aarch64|x86_64>"; exit 2 ;;
esac
[ -d "$RFS" ] || { echo "no rootfs: $RFS"; exit 1; }

D="$RFS/var/lib/dpkg"
mkdir -p "$D/info"
rm -f "$D/status"
: > "$D/status"

npk=0
for m in packages/*.pk; do
    name=$(grep -m1 '^name=' "$m" | cut -d= -f2-)
    ver=$(grep -m1 '^version=' "$m" | cut -d= -f2-)
    sec=$(grep -m1 '^section=' "$m" | cut -d= -f2-)
    lic=$(grep -m1 '^license=' "$m" | cut -d= -f2-)
    desc=$(grep -m1 '^description=' "$m" | cut -d= -f2-)
    [ -n "$name" ] || continue

    files=$(awk 'f && NF {print} /^files[[:space:]]*$/{f=1}' "$m")

    : > "$D/info/$name.list"
    : > "$D/info/$name.md5sums"
    total=0
    nf=0
    for rel in $files; do
        f="$RFS/$rel"
        [ -f "$f" ] && [ ! -L "$f" ] || continue  # symlinks unmanaged
        echo "$rel" >> "$D/info/$name.list"
        md5sum "$f" | awk -v p="${rel#/}" '{print $1"  "p}' >> "$D/info/$name.md5sums"
        sz=$(stat -c %s "$f")
        total=$((total + sz))
        nf=$((nf + 1))
    done
    [ "$nf" -gt 0 ] || { echo "WARN: $name has no regular files to seed"; continue; }

    {
        echo "Package: $name"
        echo "Status: install ok installed"
        echo "Priority: required"
        echo "Section: $sec"
        echo "Installed-Size: $((total / 1024 + 1))"
        echo "Architecture: $ARCH"
        echo "Version: $ver"
        [ -n "$desc" ] && echo "Description: $desc"
        [ -n "$lic" ] && echo "License: $lic"
        echo "Origin: official"
        echo
    } >> "$D/status"
    npk=$((npk + 1))
done

echo "seeded $npk packages into $D/ (arch=$ARCH)"
ls "$D" | sed 's/^/   /'
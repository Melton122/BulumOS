#!/bin/bash
# build-package.sh - build a signed .bpk archive from a packages/<name>.pk
# manifest. Stages the files listed in the manifest from the matching
# architecture rootfs, applies the %ARCH% substitution, and signs via
# bpkbuild.py.
#
# usage: tools/build-package.sh <name> <arch> [key-file]
#   arch: aarch64 | arm64 | x86_64 | amd64 | all
#   key-file: default tools/official-repo.key
set -e
cd /home/melton/BulumOS
NAME="$1"
ARCH="$2"
KEY="${3:-tools/official-repo.key}"
outdir="${4:-out/bpk}"

[ -n "$NAME" ] || { echo "usage: build-package.sh <name> <arch> [key]"; exit 2; }
PK="packages/$NAME.pk"
[ -f "$PK" ] || { echo "no manifest: $PK"; exit 1; }

case "$ARCH" in
  aarch64|arm64) ARCH=aarch64; RFS=rootfs ;;
  x86_64|amd64)  ARCH=x86_64;  RFS=rootfs-x86 ;;
  all|any)       ARCH=all;      RFS=rootfs ;;
  *) echo "unknown arch: $ARCH"; exit 1 ;;
esac
[ -d "$RFS" ] || { echo "no rootfs: $RFS"; exit 1; }
[ -f "$KEY" ] || { echo "no signing key: $KEY"; exit 1; }

ctl=$(mktemp)
stage=$(mktemp -d)
trap 'rm -rf "$ctl" "$stage"' EXIT

awk -v a="$ARCH" -F= '
    /^files[[:space:]]*$/{ exit }
    $1=="name"        { print "Package: " $2;        next }
    $1=="version"     { print "Version: " $2;        next }
    $1=="architecture"{ print "Architecture: " a;    next }
    $1=="maintainer"  { print "Maintainer: " $2;     next }
    $1=="license"     { print "License: " $2;        next }
    $1=="description" { print "Description: " $2;    next }
    $1=="depends"     { print "Depends: " $2;        next }
    $1=="conflicts"   { print "Conflicts: " $2;      next }
    $1=="provides"    { print "Provides: " $2;       next }
    $1=="origin"      { print "Origin: " $2;         next }
    $1=="keywords"    { print "Keywords: " $2;       next }
    $1=="section"     { print "Section: " $2;        next }
    { print }
' "$PK" > "$ctl"
grep -q '^Architecture:' "$ctl" || { echo "manifest missing architecture"; exit 1; }

mkdir -p "$outdir"
awk 'f && NF {print} /^files[[:space:]]*$/{f=1}' "$PK" | while read -r rel; do
  [ -f "$RFS/$rel" ] || { echo "missing file: $RFS/$rel"; exit 1; }
  mkdir -p "$stage/$(dirname "$rel")"
  cp -a "$RFS/$rel" "$stage/$rel"
done

python3 tools/bpkbuild.py "$outdir/${NAME}_${ARCH}.bpk" "$KEY" "$ctl" "$stage"
echo "  built $outdir/${NAME}_${ARCH}.bpk"
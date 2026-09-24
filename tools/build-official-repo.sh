#!/bin/bash
# build-official-repo.sh - build the BulumOS official package repository, with
# signed Packages index and Release, covering both supported architectures.
#
#   tools/build-official-repo.sh          build repo + (re)generate signing key
#   tools/build-official-repo.sh --bootstrap  only create the signing key and
#                                       install the public key into the rootfs
#
# Outputs:
#   out/bpk/<name>_<arch>.bpk        signed native packages (both arches + all)
#   out/repo/Packages                combined index
#   out/repo/Release + Release.sig   signed release metadata
#   tools/official-repo.key          private signing key (gitignored)
#   rootfs/etc/bulum/keys/official.pub, rootfs-x86/...  public key in the OS
set -e
cd /home/melton/BulumOS
ROOT=$(pwd)
REPO=$ROOT/out/repo
PKGDIR=$ROOT/out/bpk
KEY=$ROOT/tools/official-repo.key

if [ "$1" = "--bootstrap" ]; then
    if [ ! -f "$KEY" ]; then
        echo "== generating official repo signing key =="
        openssl genrsa -out "$KEY" 2048 2>/dev/null
        MOD=$(openssl rsa -in "$KEY" -text -noout | awk 'BEGIN{IGNORECASE=1} /modulus:/{f=1;next} /exponent/{f=0} f{printf "%s",$0}' | tr -cd '0-9a-fA-F')
        EXP=$(openssl rsa -in "$KEY" -text -noout | awk '/publicExponent/{print $2; exit}')
        [ -n "$MOD" ] && [ -n "$EXP" ] || { echo "key export failed"; exit 1; }
    else
        MOD=$(openssl rsa -in "$KEY" -text -noout | awk 'BEGIN{IGNORECASE=1} /modulus:/{f=1;next} /exponent/{f=0} f{printf "%s",$0}' | tr -cd '0-9a-fA-F')
        EXP=$(openssl rsa -in "$KEY" -text -noout | awk '/publicExponent/{print $2; exit}')
    fi
    for R in rootfs rootfs-x86; do
        mkdir -p "$R/etc/bulum/keys"
        { echo "name=official"; echo "e=$EXP"; echo "n=$MOD"; } > "$R/etc/bulum/keys/official.pub"
    done
    echo "== official.pub -> rootfs + rootfs-x86 =="
    exit 0
fi

[ -f "$KEY" ] || bash "$0" --bootstrap

echo "=== building official repository (both arches) ==="
rm -rf "$REPO"
mkdir -p "$REPO"

for m in packages/*.pk; do
    name=$(basename "$m" .pk)
    for arch in aarch64 x86_64; do
        bash tools/build-package.sh "$name" "$arch" "$KEY" "$PKGDIR"
    done
done

# ---- Packages index (reuses the make-test-repo digest logic) ----
python3 - "$PKGDIR" "$REPO" <<'PY'
import hashlib, os, sys, glob
src, dst = sys.argv[1], sys.argv[2]
out = []
for bpk in sorted(glob.glob(src + "/*.bpk")):
    raw = open(bpk, "rb").read()
    idx = raw.index(b"BEGIN PACKAGE SIGNATURE\n")
    head = raw[:idx].decode()
    head = head.split("\n", 1)[1]              # drop the BPK1 magic line
    head = head.rsplit("SHA256:", 1)[0].rstrip()  # drop embedded payload sums
    size = os.path.getsize(bpk)
    sha = hashlib.sha256(raw).hexdigest()
    out.append(head + f"\nFormat: bpk\nFilename: {os.path.basename(bpk)}\n"
               f"Size: {size}\nSHA256: {sha}")
open(dst + "/Packages", "w").write("\n\n".join(out) + "\n")
PY
echo "== Packages: $(grep -c '^Package:' "$REPO/Packages") entries =="

# ---- Release + signature ----
SIZE=$(stat -c %s "$REPO/Packages")
SHA=$(sha256sum "$REPO/Packages" | cut -d' ' -f1)
{
    # emit the checksum block BulumOS update expects:
    echo "Origin: BulumOS Official Repository"
    echo "Label: BulumOS"
    echo "Suite: official"
    echo "SHA256:"
    echo " $SHA $SIZE Packages"
} > "$REPO/Release"
openssl dgst -sha256 -sign "$KEY" -out /tmp/bulum-official-sig.bin "$REPO/Release" 2>/dev/null
xxd -p /tmp/bulum-official-sig.bin > "$REPO/Release.sig"
rm -f /tmp/bulum-official-sig.bin

echo "=== official repo ready: $REPO ==="
ls "$REPO" | sed 's/^/   /'
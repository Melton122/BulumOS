#!/bin/bash
# BulumOS: build the test repository used by test-bulum.sh and test-bpk-vm.sh
#
#   tools/make-test-repo.sh            build debs + bpks + Packages + Release + sig
#   tools/make-test-repo.sh --tamper   corrupt Packages (tamper detection test)
#   tools/make-test-repo.sh --keyonly  generate signing key into rootfs
#
# Outputs:
#   /tmp/bulum-repo/            repository served over HTTP/HTTPS by the test
#   tools/testrepo.key          private signing key (gitignored)
#   rootfs/etc/bulum/keys/main.pub  public key shipped inside the OS image
set -e
cd /home/melton/myos
ROOT=$(pwd)
REPO=/tmp/bulum-repo
KEY=$ROOT/tools/testrepo.key
PUB=$ROOT/rootfs/etc/bulum/keys/main.pub

if [ "$1" = "--tamper" ]; then
    echo "== tampering with Packages =="
    echo "X-Tampered: yes" >> "$REPO/Packages"
    exit 0
fi

if [ "$1" = "--keyonly" ]; then
    [ -f "$KEY" ] || openssl genrsa -out "$KEY" 2048 2>/dev/null
    echo "== key present =="
    exit 0
fi

# ---- shared index + signing steps -----------------------------------------
emit_index() {
    python3 - "$REPO" <<'PY'
import hashlib, os, subprocess, sys, glob
repo = sys.argv[1]
out = []
for deb in sorted(glob.glob(repo + "/*.deb") + glob.glob(repo + "/*.bpk")):
    ext = deb.rsplit(".", 1)[1]
    if ext == "deb":
        ctrl = subprocess.run(["dpkg-deb", "-I", deb],
                              capture_output=True, text=True).stdout
        lines = ctrl.splitlines()
        i = next((k for k, l in enumerate(lines) if l.strip().startswith("Package:")), 0)
        body = "\n".join(l.strip() for l in lines[i:])
        fmt = "deb"
    else:
        raw = open(deb, "rb").read()
        idx = raw.index(b"BEGIN PACKAGE SIGNATURE\n")
        head = raw[:idx].decode()
        head = head.split("\n", 1)[1]              # drop the BPK1 magic line
        head = head.rsplit("SHA256:", 1)[0].rstrip()  # drop embedded payload sums
        body = head
        fmt = "bpk"
    size = os.path.getsize(deb)
    sha = hashlib.sha256(open(deb, "rb").read()).hexdigest()
    out.append(body + f"\nFormat: {fmt}\nFilename: {os.path.basename(deb)}\n"
               f"Size: {size}\nSHA256: {sha}")
open(repo + "/Packages", "w").write("\n\n".join(out) + "\n")
PY
    echo "== Packages: $(grep -c '^Package:' "$REPO/Packages") entries =="
}

sign_release() {
    SIZE=$(stat -c %s "$REPO/Packages")
    SHA=$(sha256sum "$REPO/Packages" | cut -d' ' -f1)
    {
        echo "Origin: BulumOS Test Repository"
        echo "Label: Bulum"
        echo "Suite: test"
        echo "SHA256:"
        echo " $SHA $SIZE Packages"
    } > "$REPO/Release"
    openssl dgst -sha256 -sign "$KEY" -out /tmp/bulum-sig.bin "$REPO/Release" 2>/dev/null
    xxd -p /tmp/bulum-sig.bin > "$REPO/Release.sig"
    rm -f /tmp/bulum-sig.bin
    echo "== Release signed with $KEY =="
}

# Upgrade-driver step: publish "upgh 1.1" into the index (the guest already has
# upgh 1.0 installed from a local file), then re-index + re-sign.
if [ "$1" = "--pub-upgh-1.1" ]; then
    echo "== publishing upgh 1.1 into the index =="
    for A in arm64 amd64; do
        d="$REPO/debs-upgh/upgh-1.1-$A"
        mkdir -p "$d/DEBIAN" "$d/usr/bin"
        {
            echo "Package: upgh"
            echo "Version: 1.1"
            echo "Architecture: $A"
            echo "Maintainer: Bulum Test <test@bulum.local>"
            echo "Description: upgrade test package (1.1)"
        } > "$d/DEBIAN/control"
        printf '#!/bin/sh\necho "upgh 1.1"\n' > "$d/usr/bin/upgh"
        chmod 755 "$d/usr/bin/upgh"
        dpkg-deb --build -Zgzip "$d" "$REPO/upgh_1.1_${A}.deb" >/dev/null
    done
    rm -rf "$REPO/debs-upgh"
    emit_index
    sign_release
    exit 0
fi

echo "=== bulum test repository (multi-arch) ==="

# ---- signing key (bootstrap once) ----
if [ ! -f "$KEY" ]; then
    echo "== generating RSA signing key =="
    openssl genrsa -out "$KEY" 2048 2>/dev/null
    MOD=$(openssl rsa -in "$KEY" -text -noout | awk 'BEGIN{IGNORECASE=1} /modulus:/{f=1;next} /exponent/{f=0} f{printf "%s",$0}' | tr -cd '0-9a-fA-F')
    EXP=$(openssl rsa -in "$KEY" -text -noout | awk '/publicExponent/{print $2; exit}')
    [ -n "$MOD" ] && [ -n "$EXP" ] || { echo "key export failed"; exit 1; }
    mkdir -p rootfs/etc/bulum/keys
    { echo "name=main"; echo "e=$EXP"; echo "n=$MOD"; } > "$PUB"
    echo "== public key -> $PUB =="
fi

# ---- package content (both architectures + one all-arch) ----
rm -rf "$REPO"
mkdir -p "$REPO"

# build_deb <name> <version> <arch> <depends> <description>
build_deb() {
    local name=$1 ver=$2 arch=$3 deps=$4 desc=$5
    local d="$REPO/debs/${name}-${ver}-${arch}"
    mkdir -p "$d/DEBIAN" "$d/bin"
    {
        echo "Package: $name"
        echo "Version: $ver"
        echo "Architecture: $arch"
        echo "Maintainer: Bulum Test <test@bulum.local>"
        [ -n "$deps" ] && echo "Depends: $deps"
        echo "Description: $desc"
    } > "$d/DEBIAN/control"
    case "$name" in
        hello) printf '#!/bin/sh\necho "hello from bulum %s"\n' "$ver" > "$d/bin/hello" ;;
        bar)   printf '#!/bin/sh\necho "bar uses hello: $(/bin/hello)"\n' > "$d/bin/bar" ;;
        baz)   printf '#!/bin/sh\necho baz\n' > "$d/bin/baz" ;;
    esac
    chmod 755 "$d/bin"/*
    dpkg-deb --build -Zgzip "$d" "$REPO/${name}_${ver}_${arch}.deb" >/dev/null
}

for A in arm64 amd64; do
    build_deb hello 1.0 "$A" "" "test hello package (1.0)"
    build_deb hello 1.1 "$A" "" "test hello package (1.1)"
    build_deb bar 1.0 "$A" "hello" "test package depending on hello"
    build_deb baz 1.0 "$A" "hello (= 1.0)" "test package requiring hello exactly 1.0"
done

d="$REPO/debs/netcat-1.0-all"; mkdir -p "$d/DEBIAN" "$d/sbin"
cat > "$d/DEBIAN/control" <<'EOF'
Package: netcat
Version: 1.0
Architecture: all
Maintainer: Bulum Test <test@bulum.local>
Description: test package served only over https
EOF
printf '#!/bin/sh\necho "netcat from https repo"\n' > "$d/sbin/nc"
chmod 755 "$d/sbin/nc"
dpkg-deb --build -Zgzip "$d" "$REPO/netcat_1.0_all.deb" >/dev/null

rm -rf "$REPO/debs"

# ---- official .bpk packages (native format), one set per architecture ----
echo "== building official .bpk packages (both arches) =="
rm -rf "$ROOT/.bpkstage"
mkdir -p "$ROOT/.bpkstage"
for spec in "bctl:sbin/bctl:usr/sbin/bctl" \
            "bstatus:sbin/bstatus:usr/bin/bstatus" \
            "blog:sbin/blog:usr/bin/blog"; do
    P=${spec%%:*}; REST=${spec#*:}; SRCF=${REST%%:*}; DSTF=${REST#*:}
    for A in arm64 x86_64; do
        if [ "$A" = x86_64 ]; then SRC=rootfs-x86; else SRC=rootfs; fi
        s="$ROOT/.bpkstage/${P}-${A}"
        mkdir -p "$s/$(dirname "$DSTF")"
        cp "$SRC/$SRCF" "$s/$DSTF"
        chmod 755 "$s/$DSTF"
        cat > "$ROOT/.bpkstage/ctrl-$P-$A" <<EOF
Package: $P
Version: 1.0
Architecture: $A
Maintainer: BulumOS Team <dev@bulum.local>
Description: BulumOS native BPK package ($P)
EOF
        python3 tools/bpkbuild.py "$REPO/${P}_1.0_${A}.bpk" "$KEY" \
            "$ROOT/.bpkstage/ctrl-$P-$A" "$s" \
            || { echo "bpkbuild failed for $P-$A"; exit 1; }
    done
done
rm -rf "$ROOT/.bpkstage"

emit_index
sign_release

# ---- hellob + upgh 1.0: local-install packages (built after the index so
# they are NOT listed in Packages).  hellob exercises local .bpk install; the
# upgrade test later installs upgh 1.0 from this file, then publishes upgh
# 1.1 with --pub-upgh-1.1 and upgrades. ----
rm -rf "$ROOT/.bpkstage/hellob"
mkdir -p "$ROOT/.bpkstage/hellob/usr/bin"
printf '#!/bin/sh\necho "hello from bpk 2.0"\n' > "$ROOT/.bpkstage/hellob/usr/bin/hellob"
chmod 755 "$ROOT/.bpkstage/hellob/usr/bin/hellob"
cat > "$ROOT/.bpkstage/ctrl-hellob" <<'EOF'
Package: hellob
Version: 2.0
Architecture: aarch64
Maintainer: BulumOS Team <dev@bulum.local>
Description: native BPK format test package (local install only)
EOF
python3 tools/bpkbuild.py "$REPO/hellob_2.0_aarch64.bpk" "$KEY" \
    "$ROOT/.bpkstage/ctrl-hellob" "$ROOT/.bpkstage/hellob" \
    || { echo "bpkbuild failed for hellob"; exit 1; }

for A in arm64 amd64; do
    d="$REPO/debs-upgh/upgh-1.0-$A"
    mkdir -p "$d/DEBIAN" "$d/usr/bin"
    {
        echo "Package: upgh"
        echo "Version: 1.0"
        echo "Architecture: $A"
        echo "Maintainer: Bulum Test <test@bulum.local>"
        echo "Description: upgrade test package (1.0)"
    } > "$d/DEBIAN/control"
    printf '#!/bin/sh\necho "upgh 1.0"\n' > "$d/usr/bin/upgh"
    chmod 755 "$d/usr/bin/upgh"
    dpkg-deb --build -Zgzip "$d" "$REPO/upgh_1.0_${A}.deb" >/dev/null
done
rm -rf "$REPO/debs-upgh"

rm -rf "$ROOT/.bpkstage"

echo "=== repo ready: $REPO ==="
ls "$REPO"/*.deb "$REPO"/*hellob* | sed 's/^/   /'
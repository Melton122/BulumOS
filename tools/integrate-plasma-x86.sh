#!/bin/bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
STAGE="${BULUMOS_BUILD_DIR:-$ROOT_DIR/out/plasma-x86}"
RFS="${BULUMOS_TARGET:-$ROOT_DIR/rootfs-x86}"

usage() {
    cat <<'EOF'
Usage: integrate-plasma-x86.sh [--help]

Merge the complete KDE Plasma x86-64 stage (out/plasma-x86) into rootfs-x86
so the normal BulumOS image boots into Plasma.

The merge is a full overlay of the stage's /usr, /etc and /var trees, while
preserving BulumOS-owned paths (/bin, /sbin, /lib, /etc/bulum, native tools,
accounts database entries, and the root password).

Environment:
  BULUMOS_BUILD_DIR  stage directory (default out/plasma-x86)
  BULUMOS_TARGET     target rootfs (default rootfs-x86)

Run as root (rsync must preserve numeric owners).
EOF
}

case "${1:-}" in
    --help|-h) usage; exit 0 ;;
    "") : ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
esac

[[ "$EUID" -eq 0 ]] || { echo "Run as root." >&2; exit 1; }
[[ -d "$STAGE" ]] || { echo "Stage directory not found: $STAGE" >&2; exit 1; }
[[ -d "$RFS" ]] || { echo "Target rootfs not found: $RFS" >&2; exit 1; }
[[ -x "$STAGE/usr/bin/plasmashell" ]] || {
    echo "Stage does not contain Plasma: $STAGE/usr/bin/plasmashell" >&2
    exit 1
}
[[ -x "$STAGE/bin/busybox" ]] || { echo "Stage is not a complete rootfs" >&2; exit 1; }

command -v rsync >/dev/null || { echo "rsync is required" >&2; exit 1; }

echo "== Integrating Plasma into $RFS =="
echo "   stage:  $STAGE"
echo "   target: $RFS"

BK="$(mktemp -d /tmp/bulumos-integrate.XXXXXX)"
trap 'rm -rf "$BK"' EXIT
mkdir -p "$BK/native-bin" "$BK/native-sbin" "$BK/desktop"

# ---- 1. Preserve BulumOS native userspace before overlaying the stage ----
echo "== 1. Preserving BulumOS native tools and package database =="
NATIVE_BINS="bdesktop bdevices bdriver bfirewall bgfx bgfxdemo bgfxinfo bhardware binputtest bnet bnetwork bpower bsecure bterm btime busb buser"
NATIVE_SBINS="bctl blog bpk bstatus bulum"
for name in $NATIVE_BINS; do
    [ -e "$RFS/usr/bin/$name" ] && cp -a "$RFS/usr/bin/$name" "$BK/native-bin/"
done
for name in $NATIVE_SBINS; do
    [ -e "$RFS/usr/sbin/$name" ] && cp -a "$RFS/usr/sbin/$name" "$BK/native-sbin/"
done
find "$RFS/usr/share/applications" -maxdepth 1 -type f -name '*.bapp' \
    -exec cp -a {} "$BK/desktop/" \; 2>/dev/null || true
[ -f "$RFS/var/lib/dpkg/status" ] && cp "$RFS/var/lib/dpkg/status" "$BK/bulumos-status"

# ---- 2. Overlay the complete stage tree ----
# Keep BulumOS' real /bin, /sbin, /lib, /lib64 (busybox layout).  The stage's
# /usr tree supplies the Ubuntu/KDE runtime; explicit lib paths are wired up
# in step 4.
echo "== 2. Overlaying stage runtime (this can take a few minutes) =="
rsync -aHAX --numeric-ids --force \
    --exclude='/proc' --exclude='/sys' --exclude='/dev' \
    --exclude='/run' --exclude='/tmp' \
    --exclude='/bin' --exclude='/sbin' --exclude='/lib' --exclude='/lib64' \
    --exclude='/home' --exclude='/root' \
    --exclude='/etc/bulum' --exclude='/etc/rc.d' --exclude='/etc/init.d' \
    --exclude='/etc/fstab' --exclude='/etc/hostname' --exclude='/etc/hosts' \
    --exclude='/etc/resolv.conf' --exclude='/etc/mtab' \
    --exclude='/var/log' --exclude='/var/cache' --exclude='/var/tmp' \
    --exclude='/var/run' --exclude='/var/lock' \
    --exclude='/var/lib/apt/lists' --exclude='/var/lib/systemd' \
    "$STAGE/" "$RFS/"

# Overlay the desktop user's KDE configuration / state (stage is authoritative
# for KDE defaults, but keep any extra files already present in the target).
mkdir -p "$RFS/home/melton"
rsync -aHAX --numeric-ids --force "$STAGE/home/" "$RFS/home/" 2>/dev/null || \
    cp -a "$STAGE/home/." "$RFS/home/"

# ---- 3. Restore BulumOS native tools and .bapp metadata ----
echo "== 3. Restoring BulumOS native tools =="
cp -a "$BK/native-bin/." "$RFS/usr/bin/" 2>/dev/null || true
cp -a "$BK/native-sbin/." "$RFS/usr/sbin/" 2>/dev/null || true
mkdir -p "$RFS/usr/share/applications"
cp -a "$BK/desktop/." "$RFS/usr/share/applications/" 2>/dev/null || true

# Rebuild init from the current BulumOS source.
if gcc -static -O2 -Wall -s -o "$RFS/sbin/init" "$ROOT_DIR/src/init.c"; then
    chmod 755 "$RFS/sbin/init"
    echo "   init rebuilt from src/init.c"
else
    echo "   WARN: init rebuild failed; keeping existing binary" >&2
fi

# ---- 4. Dynamic loader / library paths ----
echo "== 4. Wiring x86-64 dynamic loader and library cache =="
LOADER_SRC=""
for candidate in \
    "$STAGE/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
    "$STAGE/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"; do
    [ -f "$candidate" ] && { LOADER_SRC="$candidate"; break; }
done
if [ -n "$LOADER_SRC" ]; then
    mkdir -p "$RFS/lib64" "$RFS/lib"
    install -m 755 "$LOADER_SRC" "$RFS/lib/ld-linux-x86-64.so.2"
    install -m 755 "$LOADER_SRC" "$RFS/lib64/ld-linux-x86-64.so.2"
else
    echo "   WARN: stage dynamic loader not found" >&2
fi

# Make the Ubuntu/KDE runtime libs visible to the loader.
mkdir -p "$RFS/etc/ld.so.conf.d"
cat > "$RFS/etc/ld.so.conf.d/zz-bulumos-kde.conf" <<'EOF'
/lib
/usr/lib
/lib/x86_64-linux-gnu
/usr/lib/x86_64-linux-gnu
/usr/lib64
EOF
if command -v ldconfig >/dev/null 2>&1; then
    ldconfig -r "$RFS" 2>/dev/null || echo "   WARN: ldconfig -r failed" >&2
fi

# ---- 5. Merge the BulumOS package database into the stage database ----
echo "== 5. Merging BulumOS package database =="
if [ -f "$BK/bulumos-status" ] && [ -f "$RFS/var/lib/dpkg/status" ]; then
    python3 - "$BK/bulumos-status" "$RFS/var/lib/dpkg/status" <<'PY'
import sys

def blocks(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    out = []
    for block in text.split("\n\n"):
        block = block.strip("\n")
        if not block:
            continue
        pkg = None
        for line in block.splitlines():
            if line.startswith("Package:"):
                pkg = line.split(":", 1)[1].strip()
                break
        if pkg:
            out.append((pkg, block))
    return out

extra_path, base_path = sys.argv[1], sys.argv[2]
extra = blocks(extra_path)
base = blocks(base_path)
have = {pkg for pkg, _ in base}
merged = [b for _, b in base]
for pkg, block in extra:
    if pkg not in have:
        merged.append(block)
        have.add(pkg)
open(base_path, "w", encoding="utf-8").write("\n\n".join(merged) + "\n")
PY
fi

# ---- 6. Accounts and groups ----
echo "== 6. Fixing accounts and group membership =="
# Stage accounts are already valid Ubuntu-style entries.  Add BulumOS' httpd
# service account if the stage does not provide it.
if ! grep -q '^httpd:' "$RFS/etc/passwd" 2>/dev/null; then
    echo 'httpd:x:110:110:httpd:/var/www:/bin/false' >> "$RFS/etc/passwd"
    echo 'httpd:!*:19000:0:99999:7:::' >> "$RFS/etc/shadow"
fi
if ! grep -q '^httpd:' "$RFS/etc/group" 2>/dev/null; then
    echo 'httpd:x:110:' >> "$RFS/etc/group"
fi
chmod 640 "$RFS/etc/shadow"
chmod 644 "$RFS/etc/passwd" "$RFS/etc/group"
chown root:shadow "$RFS/etc/shadow" 2>/dev/null || true

mkdir -p "$RFS/home/melton" "$RFS/home/melton/.config" "$RFS/home/melton/.cache"
chown -R 1000:1000 "$RFS/home/melton" 2>/dev/null || true

# BulumOS polkit rule: the desktop user administers accounts without a prompt.
mkdir -p "$RFS/etc/polkit-1/rules.d"
cat > "$RFS/etc/polkit-1/rules.d/50-bulum-user-management.rules" <<'EOF'
polkit.addRule(function(action, subject) {
    if (action.id === "org.freedesktop.accounts.user-administration" &&
        subject.isInGroup("bulum-admin")) {
        return polkit.Result.YES;
    }
});
EOF
chmod 644 "$RFS/etc/polkit-1/rules.d/50-bulum-user-management.rules"

# D-Bus service activation must not depend on systemd.
for svc_file in "$RFS/usr/share/dbus-1/system-services/"*.service; do
    [ -f "$svc_file" ] && sed -i '/^SystemdService=/d' "$svc_file"
done

# ---- 7. X11 and KDE user configuration ----
echo "== 7. Installing X11 / KWin configuration =="
mkdir -p "$RFS/etc/X11"
if [ -f "$STAGE/etc/X11/xorg.conf" ]; then
    cp -a "$STAGE/etc/X11/xorg.conf" "$RFS/etc/X11/xorg.conf"
elif [ -f "$ROOT_DIR/tools/kde/xorg.conf" ]; then
    cp "$ROOT_DIR/tools/kde/xorg.conf" "$RFS/etc/X11/xorg.conf"
fi

cat > "$RFS/home/melton/.config/kwinrc" <<'EOF'
[Compositing]
Enabled=true
Backend=XRender
UseCompositing=true
OpenGLIsUnsafe=false
EOF
chown 1000:1000 "$RFS/home/melton/.config/kwinrc"
chmod 600 "$RFS/home/melton/.config/kwinrc"

cat > "$RFS/home/melton/.config/startkderc" <<'EOF'
[General]
systemdBoot=false
EOF
chown 1000:1000 "$RFS/home/melton/.config/startkderc"
chmod 600 "$RFS/home/melton/.config/startkderc"

APPLETSRC=""
for candidate in \
    "$STAGE/home/melton/.config/plasma-org.kde.plasma.desktop-appletsrc" \
    "$ROOT_DIR/tools/kde/plasma-org.kde.plasma.desktop-appletsrc"; do
    [ -f "$candidate" ] && { APPLETSRC="$candidate"; break; }
done
if [ -n "$APPLETSRC" ]; then
    cp -a "$APPLETSRC" "$RFS/home/melton/.config/plasma-org.kde.plasma.desktop-appletsrc"
    chown 1000:1000 "$RFS/home/melton/.config/plasma-org.kde.plasma.desktop-appletsrc"
fi

# ---- 8. BulumOS services, default user and runtime dirs ----
echo "== 8. Installing BulumOS services =="
BULUMOS_ROOTFS="$RFS" bash "$ROOT_DIR/tools/install-services.sh" >/dev/null
mkdir -p "$RFS/etc/bulum"
echo "melton" > "$RFS/etc/bulum/default-user"
for svc in dbus graphics xorg plasma-session; do
    [ -f "$RFS/etc/bulum/services/$svc/meta" ] || {
        echo "ERROR: service definition missing: $svc" >&2
        exit 1
    }
done
[ -e "$RFS/etc/bulum/enabled/desktop" ] && rm -f "$RFS/etc/bulum/enabled/desktop"

mkdir -p "$RFS/run" "$RFS/run/user/1000" "$RFS/var/run/bulum" "$RFS/var/log/bulum" "$RFS/tmp"
chmod 1777 "$RFS/tmp"
chmod 700 "$RFS/run/user/1000"
chown 1000:1000 "$RFS/run/user/1000" 2>/dev/null || true

# ---- 9. Verify everything Plasma needs is present ----
echo "== 9. Verifying integrated rootfs =="
missing=0
for path in \
    sbin/init bin/bsh bin/busybox sbin/bctl usr/bin/buser \
    usr/bin/plasmashell usr/bin/startplasma-x11 usr/bin/kwin_x11 \
    usr/bin/systemsettings usr/bin/Xorg usr/bin/dbus-daemon \
    usr/sbin/bulum-plasma-session \
    usr/lib/x86_64-linux-gnu/qt5/plugins/plasma/kcms/systemsettings/kcm_users.so \
    usr/lib/x86_64-linux-gnu/libexec/kactivitymanagerd \
    usr/lib/x86_64-linux-gnu/libkwin.so.5 \
    usr/share/plasma \
    usr/share/plasma/plasmoids/org.kde.panel \
    lib64/ld-linux-x86-64.so.2 \
    etc/bulum/default-user \
    etc/polkit-1/rules.d/50-bulum-user-management.rules \
    etc/X11/xorg.conf \
    home/melton/.config/kwinrc \
    usr/share/dbus-1/system-services/org.freedesktop.Accounts.service \
    usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service; do
    if [ ! -e "$RFS/$path" ]; then
        echo "ERROR: missing $path" >&2
        missing=1
    fi
done

# Executables must be executable (a previous partial integration left many
# 0644 binaries behind).
for path in usr/bin/plasmashell usr/bin/startplasma-x11 usr/bin/kwin_x11 \
            usr/bin/Xorg usr/bin/dbus-daemon usr/bin/systemsettings; do
    if [ -e "$RFS/$path" ] && [ ! -x "$RFS/$path" ]; then
        echo "ERROR: not executable: $path" >&2
        missing=1
    fi
done

# Every group entry must be well-formed (the old rootfs had e.g. audio::x::).
if awk -F: 'NF != 4 { bad=1 } END { exit bad }' "$RFS/etc/group"; then
    :
else
    echo "ERROR: malformed /etc/group entry" >&2
    missing=1
fi
if ! awk -F: '$1 == "bulum-admin" { n = split($4, m, ","); for (i = 1; i <= n; i++) if (m[i] == "melton") ok = 1 } END { exit !ok }' "$RFS/etc/group"; then
    echo "ERROR: melton is not in bulum-admin" >&2
    missing=1
fi
if [ ! -x "$RFS/lib64/ld-linux-x86-64.so.2" ]; then
    echo "ERROR: dynamic loader is not executable" >&2
    missing=1
fi

[ "$missing" -eq 0 ] || exit 1

echo ""
echo "== rootfs-x86 integrated with KDE Plasma =="
echo "   rootfs: $RFS ($(du -sh "$RFS" | cut -f1))"
echo "   BulumOS boots into Plasma via the plasma-session service."
echo "   bdesktop is disabled; use 'bctl start desktop' for the native shell."

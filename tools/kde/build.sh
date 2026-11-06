#!/bin/bash
set -Eeuo pipefail

ROOT="${BULUMOS_ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}"
STAGE="${BULUMOS_BUILD_DIR:-$ROOT/out/plasma-x86}"
SOURCE_ROOT="$ROOT/rootfs-x86"
SUITE=noble
MIRROR=http://archive.ubuntu.com/ubuntu

if [[ "$EUID" -ne 0 ]]; then
    echo "Run the KDE package build as root." >&2
    exit 1
fi
if [[ "$(dpkg --print-architecture)" != amd64 ]]; then
    echo "The first Plasma package stage supports x86-64 (amd64) only." >&2
    exit 1
fi
if [[ ! -x "$SOURCE_ROOT/sbin/init" || ! -d "$SOURCE_ROOT/etc/bulum" ]]; then
    echo "BulumOS x86-64 source rootfs is incomplete: $SOURCE_ROOT" >&2
    exit 1
fi

if [[ -e "$STAGE" ]]; then
    if [[ ! -f "$STAGE/var/lib/dpkg/status" ||
          ! -x "$STAGE/usr/bin/plasmashell" ]]; then
        echo "Existing stage is incomplete and cannot be resumed safely: $STAGE" >&2
        exit 1
    fi
    echo "== Resuming existing Ubuntu/Plasma package stage: $STAGE =="
else
    mkdir -p "$(dirname -- "$STAGE")"
    echo "== Bootstrapping Ubuntu $SUITE amd64 into $STAGE =="
    debootstrap --arch=amd64 --variant=minbase "$SUITE" "$STAGE" "$MIRROR"
fi

cat > "$STAGE/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main universe
deb $MIRROR ${SUITE}-updates main universe
deb http://security.ubuntu.com/ubuntu ${SUITE}-security main universe
EOF
rm -f "$STAGE/etc/apt/sources.list.d/ubuntu.sources"
cat > "$STAGE/usr/sbin/policy-rc.d" <<'EOF'
#!/bin/sh
exit 101
EOF
chmod 755 "$STAGE/usr/sbin/policy-rc.d"

echo "== Installing KDE Plasma, X11, D-Bus, fonts, and login tools =="
chroot "$STAGE" apt-get update
DEBIAN_FRONTEND=noninteractive chroot "$STAGE" apt-get install -y \
    --no-install-recommends \
    plasma-desktop \
    plasma-workspace-wallpapers \
    kwin-x11 \
    konsole \
    systemsettings \
    accountsservice \
    policykit-1 \
    polkit-kde-agent-1 \
    xserver-xorg-core \
    xserver-xorg-input-libinput \
    xserver-xorg-video-fbdev \
    x11-apps \
    xinput \
    python3-dbus \
    python3-gi \
    dbus-x11 \
    dbus-user-session \
    xinit \
    x11-xserver-utils \
    fonts-dejavu-core \
    login \
    passwd
chroot "$STAGE" apt-get clean
rm -rf "$STAGE/var/lib/apt/lists/"*
rm -f "$STAGE/usr/sbin/policy-rc.d"
rm -rf "$STAGE/tmp/runtime-root"

for service_file in \
    "$STAGE/usr/share/dbus-1/system-services/org.freedesktop.Accounts.service" \
    "$STAGE/usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service"
do
    if [[ -f "$service_file" ]]; then
        sed -i '/^SystemdService=/d' "$service_file"
    fi
done

# Plasma's Leave menu asks logind to power off; provide the narrow part of
# that interface BulumOS can honour, and let the desktop user reach it.
install -D -m 755 "$ROOT/tools/kde/blogind" "$STAGE/usr/sbin/blogind"
cat > "$STAGE/usr/share/dbus-1/system-services/org.freedesktop.login1.service" <<'EOF'
[D-BUS Service]
Name=org.freedesktop.login1
Exec=/usr/sbin/blogind
User=root
EOF
cat > "$STAGE/usr/share/dbus-1/system.d/org.freedesktop.login1.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
        "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
    <policy user="root">
        <allow own="org.freedesktop.login1"/>
        <allow send_destination="org.freedesktop.login1"/>
    </policy>
    <policy context="default">
        <allow send_destination="org.freedesktop.login1"/>
    </policy>
</busconfig>
EOF

install -d -m 755 "$STAGE/etc/X11"
cat > "$STAGE/etc/X11/xorg.conf" <<'EOF'
Section "ServerLayout"
    Identifier "BulumLayout"
    Screen 0 "BulumScreen"
EndSection

Section "Device"
    Identifier "BulumVirtioGPU"
    Driver "modesetting"
    Option "AccelMethod" "none"
EndSection

Section "Screen"
    Identifier "BulumScreen"
    Device "BulumVirtioGPU"
EndSection
EOF
cat > "$STAGE/usr/sbin/bulum-plasma-session" <<'EOF'
#!/bin/sh
set -eu

export KDE_FULL_SESSION=true
export KDE_SESSION_VERSION=5
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_TYPE=x11
export QT_QPA_PLATFORM=xcb
export LANG=C.UTF-8

activity_manager=/usr/lib/x86_64-linux-gnu/libexec/kactivitymanagerd
if [ ! -x "$activity_manager" ]; then
    echo "Plasma activity manager is missing: $activity_manager" >&2
    exit 1
fi
"$activity_manager" &
activity_manager_pid=$!
tries=0
while [ "$tries" -lt 15 ]; do
    if dbus_names=$(dbus-send --session --dest=org.freedesktop.DBus \
        --type=method_call --print-reply /org/freedesktop/DBus \
        org.freedesktop.DBus.ListNames 2>/dev/null) &&
       printf '%s\n' "$dbus_names" | grep -q 'org.kde.ActivityManager'; then
        break
    fi
    if ! kill -0 "$activity_manager_pid" 2>/dev/null; then
        wait "$activity_manager_pid"
        exit $?
    fi
    sleep 1
    tries=$((tries + 1))
done
if [ "$tries" -eq 15 ]; then
    echo "Plasma activity manager did not acquire its D-Bus name." >&2
    exit 1
fi

exec /usr/bin/startplasma-x11
EOF
chmod 755 "$STAGE/usr/sbin/bulum-plasma-session"

echo "== Overlaying BulumOS init, native tools, services, and app metadata =="
gcc -static -O2 -Wall -s -o "$STAGE/sbin/init" "$ROOT/src/init.c"
install -D -m 755 "$SOURCE_ROOT/bin/bsh" "$STAGE/bin/bsh"
install -D -m 755 "$SOURCE_ROOT/bin/busybox" "$STAGE/bin/busybox"

shadow_tmp="$STAGE/etc/shadow.bulumos"
awk -F: '
    FNR == NR {
        if ($1 == "root") replacement = $0
        next
    }
    $1 == "root" && replacement != "" {
        print replacement
        replaced = 1
        next
    }
    { print }
    END {
        if (!replaced && replacement != "")
            print replacement
    }
' "$SOURCE_ROOT/etc/shadow" "$STAGE/etc/shadow" > "$shadow_tmp"
cat "$shadow_tmp" > "$STAGE/etc/shadow"
rm -f "$shadow_tmp"
chmod 640 "$STAGE/etc/shadow"
chown root:shadow "$STAGE/etc/shadow"

while IFS= read -r -d '' binary; do
    rel="${binary#"$SOURCE_ROOT"/}"
    install -D -m 755 "$binary" "$STAGE/$rel"
done < <(find "$SOURCE_ROOT/sbin" "$SOURCE_ROOT/usr/bin" \
    -maxdepth 1 -type f -name 'b*' -print0)

if [[ -L "$SOURCE_ROOT/sbin/bpk" ]]; then
    install -d "$STAGE/sbin"
    cp -a "$SOURCE_ROOT/sbin/bpk" "$STAGE/sbin/bpk"
fi
if [[ -L "$SOURCE_ROOT/usr/bin/mkpasswd" ]]; then
    install -d "$STAGE/usr/bin"
    cp -a "$SOURCE_ROOT/usr/bin/mkpasswd" "$STAGE/usr/bin/mkpasswd"
fi

if [[ -d "$SOURCE_ROOT/etc/bulum" ]]; then
    mkdir -p "$STAGE/etc/bulum"
    cp -a "$SOURCE_ROOT/etc/bulum/." "$STAGE/etc/bulum/"
fi
BULUMOS_ROOTFS="$STAGE" bash "$ROOT/tools/install-services.sh"
if [[ -d "$SOURCE_ROOT/usr/share/applications" ]]; then
    mkdir -p "$STAGE/usr/share/applications"
    find "$SOURCE_ROOT/usr/share/applications" -maxdepth 1 \
        -type f -name '*.bapp' -exec cp -a -t \
        "$STAGE/usr/share/applications" {} +
fi

for group in users video input audio render; do
    if ! chroot "$STAGE" getent group "$group" >/dev/null 2>&1; then
        chroot "$STAGE" groupadd "$group"
    fi
done
if ! chroot "$STAGE" getent group melton >/dev/null 2>&1; then
    chroot "$STAGE" groupadd --gid 1000 melton
fi
if ! chroot "$STAGE" getent group bulum-admin >/dev/null 2>&1; then
    chroot "$STAGE" groupadd --system bulum-admin
fi
if ! chroot "$STAGE" id melton >/dev/null 2>&1; then
    chroot "$STAGE" useradd --uid 1000 --gid melton --groups \
        users,video,input,audio,render --create-home --shell /bin/bsh melton
fi
chroot "$STAGE" passwd --lock melton >/dev/null
chroot "$STAGE" usermod --shell /bin/bsh root
chroot "$STAGE" install -d -m 755 /var/run/bulum /var/log/bulum
chroot "$STAGE" install -d -m 1777 /tmp
chroot "$STAGE" usermod -a -G video,input,audio,render melton
chroot "$STAGE" usermod -a -G bulum-admin melton
chroot "$STAGE" install -d -m 700 -o melton -g melton /home/melton
install -d -m 700 -o melton -g melton "$STAGE/home/melton/.config"
install -d -m 700 -o melton -g melton "$STAGE/home/melton/.cache"
chown -R melton:melton "$STAGE/home/melton/.cache"
cat > "$STAGE/home/melton/.config/kwinrc" <<'EOF'
[Compositing]
Enabled=false
EOF
chown melton:melton "$STAGE/home/melton/.config/kwinrc"
chmod 600 "$STAGE/home/melton/.config/kwinrc"
install -m 600 -o melton -g melton \
    "$ROOT/tools/kde/plasma-org.kde.plasma.desktop-appletsrc" \
    "$STAGE/home/melton/.config/plasma-org.kde.plasma.desktop-appletsrc"
cat > "$STAGE/home/melton/.config/startkderc" <<'EOF'
[General]
systemdBoot=false
EOF
chown melton:melton "$STAGE/home/melton/.config/startkderc"
chmod 600 "$STAGE/home/melton/.config/startkderc"
install -d -m 755 "$STAGE/etc/polkit-1/rules.d"
cat > "$STAGE/etc/polkit-1/rules.d/50-bulum-user-management.rules" <<'EOF'
polkit.addRule(function(action, subject) {
    if (action.id === "org.freedesktop.accounts.user-administration" &&
        subject.isInGroup("bulum-admin")) {
        return polkit.Result.YES;
    }
});
EOF
chmod 644 "$STAGE/etc/polkit-1/rules.d/50-bulum-user-management.rules"

printf 'BulumOS KDE package stage\nSuite: %s\nArchitecture: amd64\nPlasma: ' \
    "$SUITE" > "$STAGE/etc/bulum-plasma-release"
chroot "$STAGE" dpkg-query -W -f='${Version}\n' plasma-workspace \
    >> "$STAGE/etc/bulum-plasma-release"
chroot "$STAGE" dpkg-query -W \
    -f='${binary:Package}=${Version}\n' |
    LC_ALL=C sort > "$STAGE/etc/bulum-plasma-packages.lock"

echo "== Verifying staged desktop executables and package state =="
for path in \
    /usr/bin/plasmashell \
    /usr/bin/startplasma-x11 \
    /usr/bin/kwin_x11 \
    /usr/bin/konsole \
    /usr/bin/systemsettings \
    /usr/bin/Xorg \
    /usr/bin/dbus-daemon \
    /usr/sbin/bulum-plasma-session \
    /usr/sbin/blogind \
    /sbin/init \
    /bin/bsh \
    /usr/bin/buser
do
    if [[ ! -x "$STAGE$path" ]]; then
        echo "Required executable missing from stage: $path" >&2
        exit 1
    fi
done
for path in \
    /usr/share/applications/kcm_users.desktop \
    /usr/lib/x86_64-linux-gnu/qt5/plugins/plasma/kcms/systemsettings/kcm_users.so \
    /usr/share/dbus-1/system-services/org.freedesktop.Accounts.service \
    /usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service
do
    if [[ ! -e "$STAGE$path" ]]; then
        echo "Required Plasma user-management component missing: $path" >&2
        exit 1
    fi
done

chroot "$STAGE" python3 -c "import dbus" || {
    echo "python3-dbus is not usable inside the stage" >&2
    exit 1
}
# Precompile so the power service does not spend seconds compiling imports
# at session start on emulated hardware.
chroot "$STAGE" python3 -m compileall -q /usr/sbin/blogind \
    /usr/lib/python3/dist-packages/dbus >/dev/null 2>&1 || true
for path in /usr/bin/udevadm /usr/lib/xorg/modules/input/libinput_drv.so; do
    if [[ ! -e "$STAGE$path" ]]; then
        echo "Required input-stack component missing from stage: $path" >&2
        exit 1
    fi
done
chroot "$STAGE" dpkg --audit
install -d -m 700 "$STAGE/run/user/0"
chroot "$STAGE" /usr/bin/env \
    XDG_RUNTIME_DIR=/run/user/0 \
    QT_QPA_PLATFORM=offscreen \
    /usr/bin/plasmashell --version
chroot "$STAGE" /usr/bin/Xorg -version
chroot "$STAGE" /usr/bin/dbus-daemon --version
for executable in /usr/bin/kwin_x11 /usr/bin/startplasma-x11; do
    if chroot "$STAGE" /usr/bin/ldd "$executable" 2>&1 | grep -q 'not found'; then
        echo "Unresolved shared library for $executable" >&2
        exit 1
    fi
done
chroot "$STAGE" dpkg-query -W \
    -f='${binary:Package} ${Version}\n' \
    plasma-desktop plasma-workspace kwin-x11 xserver-xorg-core \
    konsole systemsettings accountsservice policykit-1 polkit-kde-agent-1 \
    dbus-x11 dbus-user-session

echo "== Plasma x86-64 package stage ready: $STAGE =="
du -sh "$STAGE"

#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# BulumOS: install service definitions into rootfs/etc/bulum
# (meta = key=value, control = start|stop|restart|status script)
set -e
BULUMOS_ROOTFS=${BULUMOS_ROOTFS:-"$ROOT_DIR/rootfs"}
cd "$BULUMOS_ROOTFS"/etc/bulum

rm -rf services enabled
mkdir -p services enabled

mk() { # mk <name> <desc> <deps> <respawn> <oneshot> <pidfile> [enabled]
    local name=$1 desc=$2 deps=$3 respawn=$4 oneshot=$5 pidfile=$6 enabled=${7:-1}
    mkdir -p "services/$name"
    {
        echo "NAME=$name"
        echo "DESC=$desc"
        echo "DEPS=$deps"
        echo "RESPAWN=$respawn"
        echo "ONESHOT=$oneshot"
        echo "PIDFILE=$pidfile"
    } > "services/$name/meta"
    if [ "$enabled" != 0 ]; then
        : > "enabled/$name"
    fi
}

# ---- syslogd ----
mk syslogd "Kernel/system logger" "" 1 0 ""
cat > services/syslogd/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    mkdir -p /var/log /var/run
    syslogd -O /var/log/messages -L -s 1000 -b 4
    klogd -K -n > /dev/null 2>&1 &
    ;;
stop)
    killall klogd 2>/dev/null
    killall syslogd 2>/dev/null
    ;;
restart)
    $0 stop
    sleep 1
    $0 start
    ;;
esac
EOF
chmod 755 services/syslogd/control

# ---- mdev (oneshot) -------------------------------------------------
mk mdev "Device hotplug (mdev)" "" 0 1 ""
cat > services/mdev/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    if [ -e /proc/sys/kernel/hotplug ]; then
        echo /sbin/mdev > /proc/sys/kernel/hotplug
    fi
    mdev -s
    ;;
stop)
    if [ -e /proc/sys/kernel/hotplug ]; then
        echo "" > /proc/sys/kernel/hotplug
    fi
    ;;
esac
EOF
chmod 755 services/mdev/control

# ---- udev (Phase 15: device manager for Xorg/libinput) --------------
# Xorg's libinput driver enumerates input devices through libudev, so
# keyboard and pointer devices only exist for the session when a real
# udev daemon is running. busybox mdev creates /dev nodes but emits no
# udev database, so it cannot replace this service.
mk udev "Device manager (systemd-udevd)" "mdev" 0 0 "/run/udev/udevd.pid"
cat > services/udev/control <<'EOF'
#!/bin/sh
PIDFILE=/run/udev/udevd.pid
UDEVD=/lib/systemd/systemd-udevd
LOG=/var/log/bulum/udev-start.log

running() {
    if [ -s "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        return 0
    fi
    for proc in /proc/[0-9]*; do
        [ "$(cat "$proc/comm" 2>/dev/null)" = systemd-udevd ] || continue
        kill -0 "${proc#/proc/}" 2>/dev/null && return 0
    done
    return 1
}

case "$1" in
start)
    # The native (busybox) rootfs has no udev; stay out of the way there.
    if [ ! -x "$UDEVD" ]; then
        echo "udev: systemd-udevd not installed (skip)"
        exit 0
    fi
    mkdir -p /run/udev/rules.d /run/udev/data /var/log/bulum
    chmod 755 /run/udev
    : > "$LOG"
    if ! running; then
        "$UDEVD" --daemon >>"$LOG" 2>&1
        echo "udevd --daemon exit=$?" >>"$LOG"
    fi
    tries=0
    while [ "$tries" -lt 3 ] && ! running; do
        sleep 1
        tries=$((tries + 1))
    done
    if ! running; then
        # --daemon detaches to /dev/null, so the reason only shows in the
        # foreground; keep this short enough to stay inside bctl's timeout.
        {
            echo "--- foreground debug run ---"
            timeout -s KILL 3 "$UDEVD" --debug --children-max=1 2>&1
            echo "--- debug run finished (rc=$?) ---"
            udevadm --version 2>&1
            ls -la /run/udev 2>&1
        } >>"$LOG" 2>&1
        echo "udev: systemd-udevd did not start; see $LOG" >&2
        exit 1
    fi
    # Populate the udev database before returning: Xorg's libinput driver
    # only accepts devices that carry udev properties such as ID_INPUT_KEYBOARD,
    # and bctl reaps children of the control script, so this cannot be left
    # running in the background.
    udevadm control --reload >>"$LOG" 2>&1
    echo "control --reload rc=$?" >>"$LOG"
    # Bound the work to the subsystems the graphical session consumes: a full
    # device-tree trigger can outrun bctl's start timeout on slow (emulated)
    # hardware, and Xorg needs the input records before it enumerates devices.
    udevadm trigger --action=add --type=devices --subsystem-match=input >>"$LOG" 2>&1
    echo "trigger input rc=$?" >>"$LOG"
    udevadm trigger --action=add --type=devices --subsystem-match=usb >>"$LOG" 2>&1
    udevadm settle --timeout=3 >>"$LOG" 2>&1
    echo "settle rc=$?" >>"$LOG"
    echo "udev database entries: $(ls /run/udev/data 2>/dev/null | wc -l)" >>"$LOG"
    ;;
stop)
    udevadm control --exit >>"$LOG" 2>&1 || true
    if running; then
        kill "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
    fi
    rm -f "$PIDFILE"
    ;;
restart)
    $0 stop
    sleep 1
    $0 start
    ;;
status)
    if running; then echo running; else echo stopped; fi
    ;;
esac
EOF
chmod 755 services/udev/control

# ---- disk ----
mk disk "Prepare persistent storage" "" 0 1 ""
cat > services/disk/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    mkdir -p /mnt/disk /var/log /var/run /var/lock /var/www \
             /var/spool/cron/crontabs /etc/dropbear /root/.ssh
    ;;
stop)
    sync
    ;;
esac
EOF
chmod 755 services/disk/control

# ---- network ----
mk network "Network (loopback + bnet apply)" "" 0 0 "/var/run/bulum-network.pid"
cat > services/network/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    ifconfig lo 127.0.0.1 up
    bnet apply || {
        echo "network: bnet apply failed" >&2
        exit 1
    }
    ;;
stop)
    killall udhcpc 2>/dev/null
    if [ -f /var/run/bulum-network.pid ]; then
        kill $(cat /var/run/bulum-network.pid 2>/dev/null) 2>/dev/null
    fi
    for i in /sys/class/net/*; do
        [ -e "$i" ] || continue
        n=$(basename "$i")
        [ "$n" = "lo" ] && continue
        ifconfig "$n" down 2>/dev/null
    done
    ifconfig lo down 2>/dev/null
    ;;
status)
    bnet status
    ;;
esac
EOF
chmod 755 services/network/control

# ---- dropbear ----
mk dropbear "SSH server (dropbear)" "network" 1 0 "/var/run/dropbear.pid"
cat > services/dropbear/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    mkdir -p /etc/dropbear
    chmod 700 /etc/dropbear
    if [ ! -s /etc/dropbear/dropbear_rsa_host_key ]; then
        dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key 2>/dev/null
        chmod 600 /etc/dropbear/dropbear_rsa_host_key 2>/dev/null
    fi
    if [ ! -s /etc/dropbear/dropbear_ed25519_host_key ]; then
        dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key 2>/dev/null
        chmod 600 /etc/dropbear/dropbear_ed25519_host_key 2>/dev/null
    fi
    mkdir -p /root/.ssh
    chmod 700 /root /root/.ssh
    chown 0:0 /root /root/.ssh
    touch /root/.ssh/authorized_keys
    chmod 600 /root/.ssh/authorized_keys
    dropbear -s -R -I 300 -K 3600 -P /var/run/dropbear.pid
    ;;
stop)
    kill $(cat /var/run/dropbear.pid 2>/dev/null) 2>/dev/null
    killall dropbear 2>/dev/null
    ;;
esac
EOF
chmod 755 services/dropbear/control

# ---- httpd ----
mk httpd "Web server (busybox httpd)" "network" 1 0 ""
cat > services/httpd/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    mkdir -p /var/www
    chown httpd:httpd /var/www 2>/dev/null
    if [ ! -f /var/www/index.html ]; then
        echo '<html><body><h1>BulumOS</h1><p>It works!</p></body></html>' > /var/www/index.html
    fi
    chown httpd:httpd /var/www/index.html 2>/dev/null
    httpd -h /var/www -p 80 -u httpd
    ;;
stop)
    killall httpd 2>/dev/null
    ;;
esac
EOF
chmod 755 services/httpd/control

# ---- telnetd ----
mk telnetd "Telnet daemon (authenticated)" "network" 1 0 "" 0
cat > services/telnetd/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    telnetd -p 23 -l /bin/login
    ;;
stop)
    killall telnetd 2>/dev/null
    ;;
esac
EOF
chmod 755 services/telnetd/control

# ---- graphics ----
mk graphics "Graphics service (detect + init)" "" 0 1 ""
cat > services/graphics/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    mkdir -p /var/run/bulum /var/log/bulum
    if /usr/bin/bgfx status > /var/log/bulum/graphics.log 2>&1; then
        echo "available" > /var/run/bulum/graphics.state
    else
        echo "unavailable" > /var/run/bulum/graphics.state
        echo "graphics unavailable (console mode active)" >> /var/log/bulum/graphics.log
    fi
    ;;
stop)
    rm -f /var/run/bulum/graphics.state
    ;;
status)
    if [ -f /var/run/bulum/graphics.state ]; then
        cat /var/run/bulum/graphics.state
    else
        echo "not started"
    fi
    ;;
esac
EOF
chmod 755 services/graphics/control

# ---- desktop (Phase 9) ----
# Started only when the framebuffer is actually usable.  The graphics service
# is ONESHOT, so bctl overwrites /var/run/bulum/graphics.state with
# "DONE rc=0"; never trust that file's content.  Probe graphics live instead
# so the deferral only triggers while the framebuffer is genuinely not ready.
# NOTE: the control runs under /bin/sh (busybox ash), which supports $(...);
# use `kill $(cat pidfile)` + killall for the daemon stop (pkill -F is not
# portable across busybox builds).
#
# Phase 15: DISABLED at boot - plasma-session (defined below) is now the
# session service, so the system boots into KDE Plasma instead of bdesktop.
# The bdesktop service stays installed; start it manually with `bctl start desktop`.
mk desktop "Desktop shell (bdesktop)" "graphics" 1 0 "/var/run/bulum/desktop.pid" 0
cat > services/desktop/control <<'EOF'
#!/bin/sh
PIDFILE=/var/run/bulum/desktop.pid
case "$1" in
start)
    mkdir -p /var/run/bulum /var/log/bulum
    USER_CONFIG=/etc/bulum/default-user
    if [ ! -r "$USER_CONFIG" ]; then
        echo "desktop deferred: no regular user configured" > /var/run/bulum/desktop.state
        exit 0
    fi
    IFS= read -r DESKTOP_USER < "$USER_CONFIG" || DESKTOP_USER=
    case "$DESKTOP_USER" in
        ""|*[!a-z0-9_-]*|-*|[0-9]*)
            echo "desktop refused: invalid configured user" >&2
            exit 1
            ;;
    esac
    if ! awk -F: -v user="$DESKTOP_USER" \
        '$1 == user && $3 >= 1000 && $7 != "/bin/false" { found = 1 }
         END { exit !found }' /etc/passwd; then
        echo "desktop deferred: configured user is missing or not regular" >&2
        exit 1
    fi
    if ! /usr/bin/bgfx status >/dev/null 2>&1; then
        echo "desktop deferred: graphics unavailable" > /var/run/bulum/desktop.state
        exit 0
    fi
    su -l -s /bin/bsh -c /usr/bin/bdesktop "$DESKTOP_USER" \
        > /var/log/bulum/desktop.log 2>&1 &
    echo $! > "$PIDFILE"
    echo "running" > /var/run/bulum/desktop.state
    ;;
stop)
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
    killall bdesktop 2>/dev/null
    echo "stopped" > /var/run/bulum/desktop.state
    ;;
status)
    if [ -f /var/run/bulum/desktop.state ]; then
        cat /var/run/bulum/desktop.state
    else
        echo "not started"
    fi
    ;;
esac
EOF
chmod 755 services/desktop/control

# ---- crond ----
mk crond "Cron daemon" "" 1 0 ""
cat > services/crond/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    mkdir -p /var/spool/cron/crontabs
    crond -b -l 8
    ;;
stop)
    killall crond 2>/dev/null
    ;;
esac
EOF
chmod 755 services/crond/control

# ---- ntpd ----
mk ntpd "NTP time sync" "network" 1 0 ""
cat > services/ntpd/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    ntpd -n -N 2>/dev/null &
    ;;
stop)
    killall ntpd 2>/dev/null
    ;;
esac
EOF


# ---- logrotate (oneshot, callable manually or from cron) ----
mk logrotate "Log rotation (manual or cron)" "" 0 1 ""
cat > services/logrotate/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    /sbin/logrotate
    ;;
stop)
    : "logrotate has no persistent process"
    ;;
esac
EOF
chmod 755 services/logrotate/control



# ---- wifi (wpa_supplicant; graceful no-op without HW) ----
mk wifi "WiFi supplicant (wpa_supplicant)" "network" 0 0 "/run/wpa_supplicant.pid"
cat > services/wifi/control <<'EOF'
#!/bin/sh
WPA=/usr/sbin/wpa_supplicant
CONF=/etc/wpa_supplicant.conf
PID=/run/wpa_supplicant.pid
case "$1" in
start)
    [ -x "$WPA" ] || { echo "wifi: $WPA not installed (skip)"; exit 0; }
    IFACE=""
    for d in /sys/class/net/*; do
        if [ -e "$d/wireless" ]; then IFACE=${d##*/}; break; fi
    done
    [ -n "$IFACE" ] || { echo "wifi: no wireless interface (skip)"; exit 0; }
    [ -s "$CONF" ] || { echo "wifi: no $CONF (run 'bnet wifi connect'); skip"; exit 0; }
    mkdir -p /var/run/wpa_supplicant
    "$WPA" -B -i"$IFACE" -c"$CONF" -P "$PID" 2>/dev/null
    echo "wifi: wpa_supplicant started on $IFACE"
    ;;
stop)
    [ -f "$PID" ] && kill "$(cat "$PID")" 2>/dev/null
    killall wpa_supplicant 2>/dev/null
    true
    ;;
restart)
    $0 stop; sleep 1; $0 start
    ;;
esac
EOF
chmod 755 services/wifi/control


# ---- dbus (Phase 15: Plasma IPC) -----------------------------
# System message bus required by KWin/Plasma.  Skips cleanly when
# dbus-daemon is not installed, so a pre-KDE rootfs boots unchanged.
mk dbus "D-Bus system message bus (Plasma IPC)" "disk" 1 0 "/run/dbus/pid"
cat > services/dbus/control <<'EOF'
#!/bin/sh
case "$1" in
start)
    if ! command -v dbus-daemon >/dev/null 2>&1; then
        echo "dbus: dbus-daemon not installed (skip)"
        exit 0
    fi
    mkdir -p /run/dbus /var/run/dbus
    if [ -f /run/dbus/pid ] && kill -0 "$(cat /run/dbus/pid)" 2>/dev/null; then
        exit 0
    fi
    dbus-daemon --system
    ;;
stop)
    [ -f /run/dbus/pid ] && kill "$(cat /run/dbus/pid)" 2>/dev/null
    killall dbus-daemon 2>/dev/null
    rm -f /run/dbus/pid
    ;;
restart)
    $0 stop
    sleep 1
    $0 start
    ;;
esac
EOF
chmod 755 services/dbus/control

# ---- logind (Phase 15: power control for the Plasma Leave menu) -----
mk logind "Power control (logind-compatible service)" "dbus" 0 0 "/run/blogind.pid"
cat > services/logind/control <<'EOF'
#!/bin/sh
PIDFILE=/run/blogind.pid

running() {
    [ -s "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

case "$1" in
start)
    if [ ! -x /usr/sbin/blogind ]; then
        echo "logind: blogind not installed (skip)"
        exit 0
    fi
    mkdir -p /var/log/bulum /run
    if running; then
        exit 0
    fi
    # Python cold-start under emulation is slow, so do not hold the boot
    # sequence open for it: D-Bus activation covers the window, and the
    # session test waits for the bus name before it exercises the service.
    PYTHONDONTWRITEBYTECODE=1 /usr/sbin/blogind >> /var/log/bulum/blogind.log 2>&1 &
    echo $! > "$PIDFILE"
    echo "logind: started blogind pid $!" >&2
    exit 0
    ;;
stop)
    if running; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
    fi
    rm -f "$PIDFILE"
    ;;
restart)
    $0 stop
    sleep 1
    $0 start
    ;;
status)
    if running; then echo running; else echo stopped; fi
    ;;
esac
EOF
chmod 755 services/logind/control

# ---- xorg (Phase 15: X11 server for Plasma) ------------------
# Follows the same live-probe deferral as the desktop service: never
# trust /var/run/bulum/graphics.state (bctl overwrites it for oneshots).
mk xorg "X11 display server (Xorg)" "graphics dbus udev" 1 0 "/var/run/Xorg.pid"
cat > services/xorg/control <<'EOF'
#!/bin/sh
PIDFILE=/var/run/Xorg.pid
case "$1" in
start)
    mkdir -p /var/run /var/log/bulum
    if ! command -v Xorg >/dev/null 2>&1; then
        echo "xorg: Xorg not installed (skip)"
        exit 0
    fi
    if ! /usr/bin/bgfx status >/dev/null 2>&1; then
        echo "xorg deferred: graphics unavailable" > /var/run/bulum/xorg.state
        exit 0
    fi
    Xorg :0 -config /etc/X11/xorg.conf -nolisten tcp -noreset \
        > /var/log/bulum/xorg.log 2>&1 &
    echo $! > "$PIDFILE"
    tries=0
    while [ "$tries" -lt 15 ]; do
        if [ -S /tmp/.X11-unix/X0 ]; then
            echo "running" > /var/run/bulum/xorg.state
            exit 0
        fi
        if ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            echo "xorg failed to start; see /var/log/bulum/xorg.log" >&2
            exit 1
        fi
        sleep 1
        tries=$((tries + 1))
    done
    echo "xorg startup timed out; see /var/log/bulum/xorg.log" >&2
    exit 1
    ;;
stop)
    [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null
    killall Xorg 2>/dev/null
    rm -f "$PIDFILE"
    echo "stopped" > /var/run/bulum/xorg.state
    ;;
restart)
    $0 stop
    sleep 1
    $0 start
    ;;
status)
    if [ -f /var/run/bulum/xorg.state ]; then
        cat /var/run/bulum/xorg.state
    else
        echo "not started"
    fi
    ;;
esac
EOF
chmod 755 services/xorg/control

# ---- plasma-session (Phase 15 milestone 3) -------------------
# Mirrors the desktop service: validates /etc/bulum/default-user, defers
# when graphics or the KDE port is missing.  Runs as the configured user
# with a private XDG_RUNTIME_DIR and D-Bus session for KDE IPC.
#
# Phase 15: this is the boot session service - it replaces the bdesktop
# `desktop` service (disabled above), so `bctl boot` enters KDE Plasma.
mk plasma-session "KDE Plasma session (startplasma)" "graphics xorg" 1 0 "/var/run/bulum/plasma.pid"
cat > services/plasma-session/control <<'EOF'
#!/bin/sh
PIDFILE=/var/run/bulum/plasma.pid
case "$1" in
start)
    mkdir -p /var/run/bulum /var/log/bulum
    USER_CONFIG=/etc/bulum/default-user
    if [ ! -r "$USER_CONFIG" ]; then
        echo "plasma-session deferred: no regular user configured" > /var/run/bulum/plasma.state
        exit 0
    fi
    IFS= read -r PUSER < "$USER_CONFIG" || PUSER=
    case "$PUSER" in
        ""|*[!a-z0-9_-]*|-*|[0-9]*)
            echo "plasma-session refused: invalid configured user" >&2
            exit 1
            ;;
    esac
    if ! awk -F: -v user="$PUSER" \
        '$1 == user && $3 >= 1000 && $7 != "/bin/false" { found = 1 }
         END { exit !found }' /etc/passwd; then
        echo "plasma-session deferred: configured user is missing or not regular" >&2
        exit 1
    fi
    if ! /usr/bin/bgfx status >/dev/null 2>&1; then
        echo "plasma-session deferred: graphics unavailable" > /var/run/bulum/plasma.state
        exit 0
    fi
    if [ -x /usr/sbin/bulum-plasma-session ]; then
        SESSION=/usr/sbin/bulum-plasma-session
    elif [ -x /usr/bin/startplasma-x11 ]; then
        SESSION=/usr/bin/startplasma-x11
    else
        echo "plasma-session deferred: Plasma is not installed" > /var/run/bulum/plasma.state
        exit 0
    fi
    PUID=$(id -u "$PUSER") || exit 1
    RUNTIME_DIR=/run/user/$PUID
    mkdir -p "$RUNTIME_DIR"
    chown "$PUSER" "$RUNTIME_DIR"
    chmod 700 "$RUNTIME_DIR"
    if [ ! -S /tmp/.X11-unix/X0 ]; then
        echo "plasma-session deferred: X11 display :0 is not ready" >&2
        exit 1
    fi
    su -l -s /bin/sh -c \
        "export DISPLAY=:0 XDG_RUNTIME_DIR=$RUNTIME_DIR; exec dbus-run-session -- $SESSION" \
        "$PUSER" \
        > /var/log/bulum/plasma.log 2>&1 &
    echo $! > "$PIDFILE"
    echo "running" > /var/run/bulum/plasma.state
    ;;
stop)
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
    killall startplasma-x11 plasma_session plasmashell kwin_x11 kwin_wayland \
        kdeinit5 klauncher ksmserver kactivitymanagerd 2>/dev/null
    echo "stopped" > /var/run/bulum/plasma.state
    ;;
restart)
    $0 stop
    sleep 1
    $0 start
    ;;
status)
    if [ -f /var/run/bulum/plasma.state ]; then
        cat /var/run/bulum/plasma.state
    else
        echo "not started"
    fi
    ;;
esac
EOF
chmod 755 services/plasma-session/control
echo "=== installed services ==="
ls services/ | tr '\n' ' '
echo
echo "enabled: $(ls enabled/ | wc -l)"

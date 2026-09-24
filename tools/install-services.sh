#!/bin/bash
# BulumOS: install service definitions into rootfs/etc/bulum
# (meta = key=value, control = start|stop|restart|status script)
set -e
cd /home/melton/myos/rootfs/etc/bulum

rm -rf services enabled
mkdir -p services enabled

mk() { # mk <name> <desc> <deps> <respawn> <oneshot> <pidfile>
    local name=$1 desc=$2 deps=$3 respawn=$4 oneshot=$5 pidfile=$6
    mkdir -p "services/$name"
    {
        echo "NAME=$name"
        echo "DESC=$desc"
        echo "DEPS=$deps"
        echo "RESPAWN=$respawn"
        echo "ONESHOT=$oneshot"
        echo "PIDFILE=$pidfile"
    } > "services/$name/meta"
    : > "enabled/$name"
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
    dropbear -R -I 300 -K 3600 -P /var/run/dropbear.pid
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
mk telnetd "Telnet daemon (authenticated)" "network" 1 0 ""
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
mk desktop "Desktop shell (bdesktop)" "graphics" 1 0 "/var/run/bulum/desktop.pid"
cat > services/desktop/control <<'EOF'
#!/bin/sh
PIDFILE=/var/run/bulum/desktop.pid
case "$1" in
start)
    mkdir -p /var/run/bulum /var/log/bulum
    if ! /usr/bin/bgfx status >/dev/null 2>&1; then
        echo "desktop deferred: graphics unavailable" > /var/run/bulum/desktop.state
        exit 0
    fi
    /usr/bin/bdesktop > /var/log/bulum/desktop.log 2>&1 &
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

echo "=== installed services ==="
ls services/ | tr '\n' ' '
echo
echo "enabled: $(ls enabled/ | wc -l)"
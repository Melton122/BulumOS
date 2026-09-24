#!/bin/bash
set -e
cd /home/melton/myos
echo "=== rebuilding init ==="
aarch64-linux-gnu-gcc -static -O2 -Wall -o src/init src/init.c 2>&1 | grep -E 'error|warning' | head -5 || true
file src/init | head -c 100
echo
echo "=== restoring busybox ==="
cp kernel/busybox/busybox rootfs/bin/busybox
chmod 755 rootfs/bin/busybox
echo "=== installing init (real file, not symlink) ==="
rm -f rootfs/sbin/init
cp src/init rootfs/sbin/init
chmod 755 rootfs/sbin/init
echo "=== bstatus + blog ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/sbin/bstatus src/bstatus.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/sbin/blog src/blog.c
chmod 755 rootfs/sbin/bstatus rootfs/sbin/blog
echo "=== bulum package manager ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/sbin/bulum src/bulum.c src/bulum_crypto.c
chmod 755 rootfs/sbin/bulum
ln -sf /sbin/bulum rootfs/sbin/bpk
echo "=== bterm terminal ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bterm src/bterm.c
chmod 755 rootfs/usr/bin/bterm
echo "=== bsh shell ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/bin/bsh src/bsh.c
chmod 755 rootfs/bin/bsh
echo "=== bnet / bsecure / bfirewall ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bnet src/bnet.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bsecure src/bsecure.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bfirewall src/bfirewall.c
chmod 755 rootfs/usr/bin/bnet rootfs/usr/bin/bsecure rootfs/usr/bin/bfirewall
echo "=== Phase 9.4: USB ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/busb src/busb.c
chmod 755 rootfs/usr/bin/busb
chmod 644 rootfs/etc/mdev.conf
echo "=== Phase 9.3: user management ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/buser src/buser.c
chmod 755 rootfs/usr/bin/buser
echo "=== Phase 9.1: time ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/btime src/btime.c
chmod 755 rootfs/usr/bin/btime
chmod 644 rootfs/etc/TZ rootfs/etc/ntp.conf
echo "=== Phase 8: graphics + input ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bgfxinfo src/bgfxinfo.c src/bgfx.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bgfxdemo src/bgfxdemo.c src/bgfx.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/binputtest src/binputtest.c src/binput.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bgfx src/bgfxcli.c src/bgfx.c src/binput.c
chmod 755 rootfs/usr/bin/bgfxinfo rootfs/usr/bin/bgfxdemo rootfs/usr/bin/binputtest rootfs/usr/bin/bgfx
chmod 644 rootfs/etc/bulum/graphics.conf rootfs/etc/mdev.conf
echo "=== Phase 9: desktop shell + compositor ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bdesktop \
    src/bdesktop.c src/bwm.c src/bshell.c \
    src/bgfx.c src/binput.c src/bwindow.c src/btheme.c \
    src/bapp.c src/bnotify.c src/bicon.c \
    src/bappui.c src/bsys.c src/bapps.c \
    src/bapp_files.c src/bapp_settings.c src/bapp_about.c \
    src/bapp_monitor.c src/bapp_network.c src/bapp_security.c \
    src/bapp_services.c src/bapp_logs.c src/bapp_users.c \
    src/bapp_packages.c src/bapp_terminal.c src/bapp_hardware.c src/bhw.c
chmod 755 rootfs/usr/bin/bdesktop
chmod 644 rootfs/etc/bulum/desktop.conf rootfs/etc/bulum/theme.conf
echo "=== Phase 10: application metadata + preferences ==="
mkdir -p rootfs/usr/share/applications
cat > rootfs/usr/share/applications/bulum-files.bapp <<'EOF'
name=Bulum Files
icon=files
exec=internal:files
category=Files
version=1.0.0
description=Browse and manage files
EOF
cat > rootfs/usr/share/applications/bulum-settings.bapp <<'EOF'
name=Bulum Settings
icon=settings
exec=internal:settings
category=System
version=1.0.0
description=System and appearance settings
EOF
cat > rootfs/usr/share/applications/bulum-packages.bapp <<'EOF'
name=Bulum Packages
icon=packages
exec=internal:packages
category=System
version=1.0.0
description=Install and remove packages
EOF
cat > rootfs/usr/share/applications/bulum-monitor.bapp <<'EOF'
name=System Monitor
icon=monitor
exec=internal:monitor
category=System
version=1.0.0
description=CPU, memory and processes
EOF
cat > rootfs/usr/share/applications/bulum-hardware.bapp <<'EOF'
name=Hardware
icon=settings
exec=internal:hardware
category=System
version=1.0.0
description=CPU, memory, GPU, devices and kernel driver framework
EOF
cat > rootfs/usr/share/applications/bulum-network.bapp <<'EOF'
name=Network
icon=network
exec=internal:network
category=System
version=1.0.0
description=Network interfaces and DNS
EOF
cat > rootfs/usr/share/applications/bulum-security.bapp <<'EOF'
name=Security
icon=security
exec=internal:security
category=Security
version=1.0.0
description=Security audit and status
EOF
cat > rootfs/usr/share/applications/bulum-services.bapp <<'EOF'
name=Services
icon=services
exec=internal:services
category=System
version=1.0.0
description=Manage bctl services
EOF
cat > rootfs/usr/share/applications/bulum-logs.bapp <<'EOF'
name=Logs
icon=log
exec=internal:logs
category=System
version=1.0.0
description=View system logs
EOF
cat > rootfs/usr/share/applications/bulum-users.bapp <<'EOF'
name=Users
icon=users
exec=internal:users
category=System
version=1.0.0
description=View user accounts
EOF
cat > rootfs/usr/share/applications/bulum-terminal.bapp <<'EOF'
name=Terminal
icon=terminal
exec=internal:terminal
category=Utilities
version=1.0.0
description=BulumOS terminal (bsh)
EOF
cat > rootfs/usr/share/applications/bulum-about.bapp <<'EOF'
name=About BulumOS
icon=about
exec=internal:about
category=System
version=1.0.0
description=About BulumOS
EOF
chmod 644 rootfs/usr/share/applications/*.bapp
if [ ! -f rootfs/etc/bulum/preferences.conf ]; then
    cat > rootfs/etc/bulum/preferences.conf <<'EOF'
theme=dark
panel=1
desktop_icons=1
clock_format=24h
launcher_auto_close=1
EOF
fi
chmod 644 rootfs/etc/bulum/preferences.conf
if [ ! -f rootfs/etc/bulum/associations.conf ]; then
    cat > rootfs/etc/bulum/associations.conf <<'EOF'
# File associations: extension=app name (or internal:exec token)
.txt=Terminal
.md=Terminal
.log=Logs
.conf=Terminal
.sh=Terminal
.txt=Terminal
EOF
fi
chmod 644 rootfs/etc/bulum/associations.conf
echo "=== Phase 11: hardware tools ==="
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bhardware src/bhardware.c src/bhw.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bdevices src/bdevices.c src/bhw.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bdriver src/bdriver.c src/bhw.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bnetwork src/bnetwork.c src/bhw.c
aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/usr/bin/bpower src/bpower.c src/bhw.c
chmod 755 rootfs/usr/bin/bhardware rootfs/usr/bin/bdevices rootfs/usr/bin/bdriver rootfs/usr/bin/bnetwork rootfs/usr/bin/bpower
echo "=== network config + permissions ==="
chmod 644 rootfs/etc/bulum/network.conf rootfs/etc/hosts rootfs/etc/resolv.conf
chmod 700 rootfs/root
cp /usr/aarch64-linux-gnu/lib/libnss_dns.so.2 /usr/aarch64-linux-gnu/lib/libnss_files.so.2 rootfs/lib/ 2>/dev/null
chmod 755 rootfs/lib/libnss_*.so.2 2>/dev/null
bash tools/install-services.sh 2>&1 | tail -3
echo "=== Phase 12: repo signing key + seeded package db (arm64) ==="
bash tools/build-official-repo.sh --bootstrap
bash tools/seed-rootfs-db.sh aarch64
echo "=== rebuilding full initramfs ==="
cd rootfs
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > /home/melton/myos/initramfs.cpio.gz
cd /home/melton/myos
gzip -t initramfs.cpio.gz && echo "full initramfs OK: $(du -h initramfs.cpio.gz | cut -f1)"
echo "=== rebuilding switch initramfs ==="
bash build-switch-initramfs.sh 2>&1 | tail -1
echo "=== rebuilding disk.img (persistent root) ==="
tools/populate-disk.sh 2>&1 | grep -E '===|error|OK' | tail -6
echo "=== all userspace artifacts rebuilt ==="
#!/bin/bash
# Generate packages/*.pk manifests for official BulumOS components.
# Each manifest lists control fields + a "files" section of rootfs-relative
# paths. Shared by tools/build-package.sh (archive building) and
# tools/seed-rootfs-db.sh (base DB seeding).
set -e
cd /home/melton/BulumOS
mkdir -p packages

maint="BulumOS Team <dev@bulum.local>"

write_pk() {
  local name="$1" ver="$2" arch="$3" sec="$4" lic="$5" desc="$6"; shift 6
  local f="packages/$name.pk"
  {
    echo "name=$name"
    echo "version=$ver"
    echo "architecture=$arch"
    echo "section=$sec"
    echo "maintainer=$maint"
    echo "license=$lic"
    echo "description=$desc"
    echo "depends="
    echo "files"
    for p in "$@"; do echo "$p"; done
  } > "$f"
  echo "  wrote $f"
}

write_pk bsh       1.0.0 "%ARCH%" base    MIT  "BulumOS POSIX shell"                     /bin/bsh
write_pk busybox   1.36.1 "%ARCH%" base   GPL-2.0 "BusyBox multi-call binary"            /bin/busybox
write_pk bterm     1.0.0 "%ARCH%" base    MIT  "BulumOS terminal emulator"               /usr/bin/bterm
write_pk bnet      1.0.0 "%ARCH%" base    MIT  "Network configuration tool"              /usr/bin/bnet
write_pk btime     1.0.0 "%ARCH%" base    MIT  "Clock / time settings"                   /usr/bin/btime
write_pk blog      1.0.0 "%ARCH%" base    MIT  "System log viewer"                       /sbin/blog
write_pk busb      1.0.0 "%ARCH%" base    MIT  "USB bus viewer"                          /usr/bin/busb
write_pk buser     1.0.0 "%ARCH%" base    MIT  "User management tool"                    /usr/bin/buser
write_pk bsecure   1.0.0 "%ARCH%" base    MIT  "System security status"                  /usr/bin/bsecure
write_pk bfirewall 1.0.0 "%ARCH%" base    MIT  "Firewall rules tool"                     /usr/bin/bfirewall
write_pk bulum     1.3.0 "%ARCH%" base    MIT  "Package manager (bpk)"                   /sbin/bulum
write_pk bhardware 1.0.0 "%ARCH%" base    MIT  "Hardware detection"                      /usr/bin/bhardware
write_pk bdevices  1.0.0 "%ARCH%" base    MIT  "Device inventory"                        /usr/bin/bdevices
write_pk bdriver   1.0.0 "%ARCH%" base    MIT  "Driver information"                      /usr/bin/bdriver
write_pk bnetwork  1.0.0 "%ARCH%" base    MIT  "Network manager"                         /usr/bin/bnetwork
write_pk bpower    1.0.0 "%ARCH%" base    MIT  "Power management"                        /usr/bin/bpower
write_pk bdesktop  1.0.0 "%ARCH%" base    MIT  "Desktop environment"                     /usr/bin/bdesktop
write_pk bgfx      1.0.0 "%ARCH%" base    MIT  "Graphics framework + demos"              /usr/bin/bgfx /usr/bin/bgfxinfo /usr/bin/bgfxdemo
write_pk binput    1.0.0 "%ARCH%" base    MIT  "Input framework test"                    /usr/bin/binputtest
write_pk init      1.0.0 "%ARCH%" base    MIT  "BulumOS init"                            /sbin/init
write_pk bstatus   1.0.0 "%ARCH%" base    MIT  "System status daemon"                    /sbin/bstatus
write_pk bctl      1.0.0 "%ARCH%" base    MIT  "System control utility"                  /sbin/bctl
echo "done: $(ls packages | wc -l) manifests"
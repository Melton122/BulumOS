#!/bin/bash
set -e
echo "== host dropbear ldd =="
ldd /usr/sbin/dropbear 2>&1 | head -15
echo
echo "== dbclient/dropbearkey/scp ldd =="
ldd /usr/bin/dbclient 2>&1 | head -8
RFS=/home/melton/myos/rootfs-x86
echo "== copy x86 glibc set =="
for l in libc.so.6 libcrypt.so.1 libutil.so.1 libm.so.6 libresolv.so.2 libnsl.so.1 libz.so.1 libtomcrypt.so.1 libtommath.so.1 libgmp.so.10; do
    if [ -f /lib/x86_64-linux-gnu/$l ]; then
        cp /lib/x86_64-linux-gnu/$l "$RFS/lib/$l"
        echo "copied $l"
    fi
done
echo "== lib64 loader =="
mkdir -p "$RFS/lib64"
cp /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$RFS/lib64/ld-linux-x86-64.so.2"
echo "== final lib contents =="
ls -la "$RFS/lib" "$RFS/lib64"
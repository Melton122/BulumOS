#!/bin/bash
# Fix x86 rootfs stragglers: rebuild bctl + init, swap in x86 dropbear + glibc.
set -e
cd /home/melton/myos
RFS=rootfs-x86

echo "=== rebuild init + bctl for x86 ==="
gcc -static -O2 -Wall -s -o "$RFS/sbin/init" src/init.c 2>&1 | grep -E 'error' | head -5 || true
gcc -static -O2 -s -o "$RFS/sbin/bctl" src/bctl.c
chmod 755 "$RFS/sbin/init" "$RFS/sbin/bctl"

echo "=== x86 dropbear family + glibc ==="
mkdir -p "$RFS/lib64" "$RFS/lib"
cp /usr/sbin/dropbear "$RFS/usr/sbin/dropbear"
for b in dbclient dropbearkey scp dropbearconvert; do
    if [ -x /usr/bin/$b ]; then cp /usr/bin/$b "$RFS/usr/bin/$b"; chmod 755 "$RFS/usr/bin/$b"; fi
done
for l in ld-linux-x86-64.so.2 libc.so.6 libcrypt.so.1 libutil.so.1 libm.so.6 libnsl.so.1 libresolv.so.2; do
    if [ -f /lib/x86_64-linux-gnu/$l ]; then
        cp /lib/x86_64-linux-gnu/$l "$RFS/lib/$l"
    fi
done
cp /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$RFS/lib64/ld-linux-x86-64.so.2"
chmod 755 "$RFS"/lib/ld-linux-x86-64.so.2 "$RFS"/lib64/ld-linux-x86-64.so.2 2>/dev/null || true
chmod 755 "$RFS"/lib/*.so.2 "$RFS"/lib/*.so.1 2>/dev/null || true

echo "=== remove aarch64 libs ==="
rm -f "$RFS/lib/ld-linux-aarch64.so.1" "$RFS/lib/libnss_dns.so.2" "$RFS/lib/libnss_files.so.2"

echo "=== residual aarch64 scan (should show nothing) ==="
find "$RFS" -type f 2>/dev/null | while read -r f; do
    file "$f" 2>/dev/null | grep -q 'aarch64' && echo "STRAY: $f"
done
echo "=== scan done ==="
#!/bin/bash
echo "=== exact filename ==="
curl -s http://ftp.debian.org/debian/dists/trixie/main/binary-arm64/Packages.gz | zcat | grep -A30 '^Package: grub-efi-arm64-bin$' | grep '^Filename:' | head -2

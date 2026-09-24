#!/bin/bash
F=$(curl -s http://ftp.debian.org/debian/dists/trixie/main/binary-amd64/Packages.gz | zcat | grep -A30 '^Package: grub-common$' | grep '^Filename:' | head -1 | awk '{print $2}')
echo "file: $F"
mkdir -p ~/grub-common
cd ~/grub-common
curl -sO "http://ftp.debian.org/debian/$F"
ls -la *.deb
dpkg-deb -x grub-common_*_amd64.deb extracted
ls extracted/usr/bin/ | grep -E 'mkimage|mkstandalone|mkrescue'

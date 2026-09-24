#!/bin/bash
mkdir -p ~/grub-arm64-ubuntu
cd ~/grub-arm64-ubuntu
curl -sO http://ports.ubuntu.com/pool/main/g/grub2-unsigned/grub-efi-arm64-bin_2.12-1ubuntu7.3_arm64.deb
ls -la *.deb
dpkg-deb -x grub-efi-arm64-bin_2.12-1ubuntu7.3_arm64.deb extracted
echo "=== virtio check ==="
ls extracted/usr/lib/grub/arm64-efi/ | grep -E '^virtio'
echo "=== version check ==="
ls extracted/usr/lib/grub/arm64-efi/ | grep -cE '\.mod$'

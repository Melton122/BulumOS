#!/bin/bash
echo '6374' | sudo -S apt-get update 2>&1 | tail -2
echo "==="
apt-cache policy grub-efi-arm64 xorriso 2>/dev/null | grep -A1 'grub-efi-arm64:\|xorriso:'
echo "==="
echo '6374' | sudo -S apt-get install -y grub-efi-arm64 xorriso 2>&1 | tail -3
echo "==="
dpkg -l grub-efi-arm64 xorriso 2>/dev/null | grep -E '^ii'

#!/bin/bash
set -euo pipefail
cd /home/melton/myos
bash build-all-uefi.sh 2>&1 | grep -E '===|BOOTAA64|error|Written|sectors|.iso|.img|Total|Image|grub\.cfg|initramfs'
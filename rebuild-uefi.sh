#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
set -euo pipefail
cd "$ROOT_DIR"
bash build-all-uefi.sh 2>&1 | grep -E '===|BOOTAA64|error|Written|sectors|.iso|.img|Total|Image|grub\.cfg|initramfs'
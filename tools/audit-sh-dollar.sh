#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
echo "=== service control scripts containing command substitution \$() ==="
grep -rn '$(' rootfs/etc/bulum/services/
echo "=== done ==="

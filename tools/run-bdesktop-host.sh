#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
echo "--- version ---"
/tmp/bdesk-host/bdesktop2 --version
echo "--- frames (no graphics on host -> clean unavailable) ---"
/tmp/bdesk-host/bdesktop2 --frames 3 --backend auto
echo "rc=$?"
echo "DONE"

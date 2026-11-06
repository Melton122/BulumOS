#!/bin/bash
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
grep -n 'strcmp\|name ==\|"launcher"\|"terminal"\|"help"' src/bicon.c | head -50
echo '--- sizes ---'
wc -l src/bicon.c
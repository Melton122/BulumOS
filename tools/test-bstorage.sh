#!/bin/bash
set -eu
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
OUT="$(mktemp -d /tmp/test-bstorage.XXXXXX)"
trap 'rm -rf "$OUT"' EXIT

echo "== compiling fixture test and read-only CLIs =="
gcc -std=c11 -Wall -Wextra -Werror -DBSTORAGE_TESTING \
    -o "$OUT/test-bstorage" tools/test-bstorage.c src/bstorage.c
for tool in bdisks bmount bdf bdu bfsck bformat; do
    gcc -std=c11 -Wall -Wextra -Werror -o "$OUT/$tool" \
        "src/$tool.c" src/bstorage.c
done

"$OUT/test-bstorage"
for tool in bdisks bmount bdf; do
    "$OUT/$tool" --json > "$OUT/$tool.json"
    python3 -m json.tool "$OUT/$tool.json" >/dev/null
    echo "PASS $tool JSON parses"
done
"$OUT/bdu" --json --max-depth 1 "$OUT" > "$OUT/bdu.json"
python3 -m json.tool "$OUT/bdu.json" >/dev/null
echo "PASS bdu bounded traversal JSON parses"
if "$OUT/bfsck" /dev/no-such-bstorage-device >"$OUT/bfsck.out" 2>&1; then
    echo "FAIL bfsck accepted absent device"
    exit 1
fi
"$OUT/bformat" --help | grep -q 'not implemented'
if "$OUT/bformat" --confirm /dev/null ext4 >"$OUT/bformat.out" 2>&1; then
    echo "FAIL bformat accepted a formatting request"
    exit 1
fi
grep -q 'no changes were made' "$OUT/bformat.out"
echo "PASS bfsck/bformat remain diagnostic-only"
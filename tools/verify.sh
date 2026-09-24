#!/bin/bash
# BulumOS: end-to-end host verification of the bulum crypto stack.
#  - builds the signed test repository
#  - runs ctest (hash known-answer vectors + RSA PKCS#1 verify over real artifacts)
#  - runs randomized big-number mulmod differential tests vs python
set -e
cd "$(dirname "$0")/.."

echo "=== 1. test repository ==="
rm -rf /tmp/bulum-repo
bash tools/make-test-repo.sh

echo
echo "=== 2. ctest (crypto unit tests) ==="
gcc -O2 -o /tmp/ctest tools/ctest.c src/bulum_crypto.c
EXP=$(python3 -c "import hashlib; print(hashlib.sha256(bytes(i&0xff for i in range(1000000))).hexdigest())")
/tmp/ctest /tmp/bulum-repo/Release /tmp/bulum-repo/Release.sig \
    rootfs/etc/bulum/keys/main.pub "$EXP"

echo
echo "=== 3. 300 small + 40 big randomized mulmod tests vs python ==="
E=0
D=0
for S in 42 99; do
  python3 - "$S" <<'PY' || E=1
import random, subprocess, sys
random.seed(int(sys.argv[1]))
bad = 0
limit = 300 if int(sys.argv[1]) == 42 else 40
for t in range(limit):
    if int(sys.argv[1]) == 42:
        bits = random.choice([31, 32, 33, 60, 63, 64, 65, 100, 127, 128, 129, 200])
        a = random.getrandbits(random.randint(1, 80))
        b = random.getrandbits(random.randint(1, 80))
        m = random.getrandbits(bits) | (1 << (bits - 1))
    else:
        a = random.getrandbits(random.randint(1900, 2047))
        b = random.getrandbits(random.randint(1900, 2047))
        mbits = random.randint(1920, 2048)
        m = random.getrandbits(mbits) | (1 << (mbits - 1))
    if a >= m or b >= m or a == 0 or b == 0:
        continue
    out = subprocess.run(["/tmp/dbgbn", "mulhex", f"{a:x}", f"{b:x}", f"{m:x}"],
                         capture_output=True, text=True).stdout.strip()
    want = f"{a * b % m:0{512}x}"
    if out != want:
        bad += 1
        if bad <= 2:
            print(f"FAIL seed={sys.argv[1]} t={t} mbits={m.bit_length()}")
            print("  got:", out[-32:])
            print("  want:", want[-32:])
print(f"seed {sys.argv[1]}: bad = {bad} of {limit}")
if bad:
    sys.exit(1)
PY
done
echo "randomized suites done"

echo
echo "=== 4. BPK roundtrip (builder + native extractor) ==="
sed 's|/etc/bulum/keys|/tmp/bpkkeys|' src/bulum.c > /tmp/bulum-bpk.c
mkdir -p /tmp/bpkkeys /tmp/bpkstage
cp rootfs/etc/bulum/keys/main.pub /tmp/bpkkeys/main.pub
gcc -O2 -I src -I /tmp -o /tmp/bpktest tools/bpktest.c src/bulum_crypto.c

if /tmp/bpktest /tmp/bulum-repo/hellob_2.0_arm64.bpk /tmp/bpkstage usr/bin/hellob; then
    echo "PASS bpk build+verify+extract (signature + sha256 + tar + content)"
else
    echo "FAIL bpk roundtrip"; exit 1
fi

# tamper inside the signed header region -> must be rejected
cp /tmp/bulum-repo/hellob_2.0_arm64.bpk /tmp/hellob_bad.bpk
printf X | dd of=/tmp/hellob_bad.bpk bs=1 seek=220 conv=notrunc 2>/dev/null
if /tmp/bpktest /tmp/hellob_bad.bpk /tmp/bpkstage usr/bin/hellob; then
    echo "FAIL tampered bpk was accepted"; exit 1
else
    echo "PASS tampered header rejected (bad signature)"
fi

# tamper inside the payload -> must be rejected (sha256)
cp /tmp/bulum-repo/hellob_2.0_arm64.bpk /tmp/hellob_bad2.bpk
SZ=$(stat -c %s /tmp/hellob_bad2.bpk)
printf X | dd of=/tmp/hellob_bad2.bpk bs=1 seek=$((SZ - 64)) conv=notrunc 2>/dev/null
if /tmp/bpktest /tmp/hellob_bad2.bpk /tmp/bpkstage usr/bin/hellob; then
    echo "FAIL tampered payload was accepted"; exit 1
else
    echo "PASS tampered payload rejected (sha256 mismatch)"
fi
rm -rf /tmp/bpkstage /tmp/bpkkeys

echo
echo "=== ALL HOST CHECKS DONE ==="
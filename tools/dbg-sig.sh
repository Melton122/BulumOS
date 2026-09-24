#!/bin/bash
set -e
cd "/home/melton/Bulum OS"
K=tools/testrepo.key
B=/tmp/bulum-repo/hellob_2.0_arm64.bpk
openssl rsa -in "$K" -pubout -out /tmp/bpk.pub 2>/dev/null

python3 - "$B" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
idx = data.index(b"BEGIN PACKAGE SIGNATURE\n")
open("/tmp/head.bin", "wb").write(data[:idx])
end = data.index(b"\nEND PACKAGE SIGNATURE\n")
sig = data[idx + len(b"BEGIN PACKAGE SIGNATURE\n"):end].decode().replace("\n", "")
open("/tmp/sig.bin", "wb").write(bytes.fromhex(sig))
print("signed_len:", idx, "sig_len:", len(sig) // 2)
PY

openssl dgst -sha256 -verify /tmp/bpk.pub -signature /tmp/sig.bin /tmp/head.bin
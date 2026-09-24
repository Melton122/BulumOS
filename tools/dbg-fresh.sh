#!/bin/bash
set -e
cd "/home/melton/Bulum OS"
bash tools/make-test-repo.sh >/dev/null 2>&1
echo "== repo fresh =="
python3 - <<'PY'
data = open("/tmp/bulum-repo/hellob_2.0_arm64.bpk", "rb").read()
n = len(data)
sigb = data.find(b"BEGIN PACKAGE SIGNATURE\n")
sige = data.find(b"\nEND PACKAGE SIGNATURE\n", sigb)
mark = b"BEGIN PACKAGE SIGNATURE\n"
signed = data[:sigb]
sigtext = data[sigb + len(mark):sige]
digits = sigtext.replace(b"\n", b"")
print("file len", n, "| sigb", sigb, "| sige", sige)
print("signed len", len(signed), "| sigtext len", len(sigtext),
      "| hex digits", len(digits), "| newlines", sigtext.count(b"\n"))
print("starts BPK1:", data[:5] == b"BPK1\n")

open("/tmp/t2signed.bin", "wb").write(signed)
open("/tmp/t2sig.bin", "wb").write(bytes.fromhex(digits.decode()))
PY
echo "== openssl check =="
openssl rsa -in tools/testrepo.key -pubout -out /tmp/t2.pub 2>/dev/null
openssl dgst -sha256 -verify /tmp/t2.pub -signature /tmp/t2sig.bin /tmp/t2signed.bin
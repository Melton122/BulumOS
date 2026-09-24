import sys

data = open("/tmp/bulum-repo/hellob_2.0_arm64.bpk", "rb").read()
n = len(data)

sigb = data.find(b"BEGIN PACKAGE SIGNATURE\n")
sige = data.find(b"\nEND PACKAGE SIGNATURE\n", sigb)
print("sigb", sigb, "sige", sige)
signed = data[:sigb]
sigtext = data[sigb + len(b"BEGIN PACKAGE SIGNATURE\n"):sige]
print("sigtext len", len(sigtext), "has newline:", b"\n" in sigtext)

h1 = open("/tmp/head.bin", "rb").read()
print("signed == head.bin:", signed == h1)

sighex = sigtext.decode()
import binascii
try:
    sb = binascii.unhexlify(sighex)
    print("unhex ok, len", len(sb))
    s0 = open("/tmp/sig.bin", "rb").read()
    print("sig bytes == sig.bin:", sb == s0)
except Exception as e:
    print("unhex FAIL:", e)

# payload position
payload = data[sige + len(b"\nEND PACKAGE SIGNATURE\n"):]
while payload[:1] in (b"\n", b"\r"):
    payload = payload[1:]
print("payload len", len(payload))
import hashlib
print("sha ok:", hashlib.sha256(payload).hexdigest() == sighex and False or "n/a")
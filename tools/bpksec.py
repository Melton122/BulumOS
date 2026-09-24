#!/usr/bin/env python3
"""BulumOS Phase 4 BPK security test suite.

Runs the 13-case checklist from the Phase 4 plan against the real
extraction code (src/bulum.c compiled as a host harness):

  Valid package       -> PASS (extracts)
  Modified payload    -> REJECT
  Modified metadata   -> REJECT
  Invalid signature   -> REJECT
  Wrong signing key   -> REJECT
  Corrupted SHA256    -> REJECT
  Incorrect file size -> REJECT
  Truncated package   -> REJECT
  Missing metadata    -> REJECT
  Duplicate metadata  -> REJECT
  Path traversal      -> REJECT
  Absolute path       -> REJECT
  Malformed package   -> REJECT

usage: python3 tools/bpksec.py
"""
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = "/tmp/bpksec"
KEYS = "/tmp/bpkkeys"
HARNESS = "/tmp/bpktest"
CONTROL = (
    "Package: hellob\n"
    "Version: 2.0\n"
    "Architecture: arm64\n"
    "Maintainer: Bulum Test <test@bulum.local>\n"
    "Description: BPK security test package\n"
)
PAYLOAD_TXT = "hello from bpk 2.0\n"

passed = failed = 0


def report(name, expect_reject, rc, note=""):
    global passed, failed
    ok = (rc != 0) if expect_reject else (rc == 0)
    status = "PASS" if ok else "FAIL"
    if ok:
        passed += 1
    else:
        failed += 1
    print(f"{status}  {name:<24} rc={rc} {note}")


def genkey(name):
    key = f"{WORK}/{name}.key"
    pub = f"{KEYS}/{name}.pub"
    if not os.path.exists(key):
        subprocess.run(["openssl", "genrsa", "-out", key, "2048"],
                       check=True, capture_output=True)
    mod = subprocess.run(
        ["openssl", "rsa", "-in", key, "-text", "-noout"],
        check=True, capture_output=True, text=True).stdout
    lines = mod.splitlines()
    modtxt = ""
    on = False
    for l in lines:
        if re.match(r"modulus:", l, re.I):
            on = True
            continue
        if re.match(r"publicExponent:", l, re.I):
            break
        if on:
            modtxt += l
    modtxt = re.sub(r"[^0-9a-fA-F]", "", modtxt)
    expm = [l for l in lines if re.match(r"publicExponent", l, re.I)]
    exp = re.sub(r"[^0-9]", "", expm[0].split()[1])
    with open(pub, "w") as f:
        f.write(f"name={name}\ne={exp}\nn={modtxt}\n")
    return key, pub


def sign(region, key):
    return subprocess.run(["openssl", "dgst", "-sha256", "-sign", key, "-"],
                          input=region, capture_output=True, check=True).stdout


def assemble(region, key, payload, sha=None, size=None, omit_sha=False):
    if not omit_sha and sha is None:
        sha = hashlib.sha256(payload).hexdigest()
    if size is None:
        size = len(payload)
    extra = b""
    if not omit_sha:
        extra += f"SHA256: {sha}\n".encode()
    extra += f"Size: {size}\n".encode()
    signed = b"BPK1\n" + region + extra
    sig = sign(signed, key)
    wrapped = "\n".join(sig.hex()[i:i + 64] for i in range(0, len(sig.hex()), 64))
    return signed + b"BEGIN PACKAGE SIGNATURE\n" + wrapped.encode() + \
        b"\nEND PACKAGE SIGNATURE\n\n" + payload


def make_payload(tar_name=None):
    if tar_name is None:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
            for arc, data in [("usr/bin/hellob", PAYLOAD_TXT.encode())]:
                ti = tarfile.TarInfo(arc)
                ti.size = len(data)
                ti.mode = 0o755
                t.addfile(ti, io.BytesIO(data))
        return buf.getvalue()
    data = b"evil payload"
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        ti = tarfile.TarInfo(tar_name)
        ti.size = len(data)
        ti.mode = 0o644
        t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def run(pkg):
    stage = f"{WORK}/stage"
    shutil.rmtree(stage, ignore_errors=True)
    r = subprocess.run([HARNESS, pkg, stage, "usr/bin/hellob"],
                       capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(f"{WORK}/payload/usr/bin", exist_ok=True)
    with open(f"{WORK}/payload/usr/bin/hellob", "w") as f:
        f.write(PAYLOAD_TXT)
    os.chmod(f"{WORK}/payload/usr/bin/hellob", 0o755)
    os.makedirs(KEYS, exist_ok=True)
    main_key, main_pub = genkey("main")
    alt_key, _ = genkey("alt")

    print("== building harness ==")
    src = open(f"{ROOT}/src/bulum.c").read().replace("/etc/bulum/keys", KEYS)
    open(f"{WORK}/bulum-bpk.c", "w").write(src)
    r = subprocess.run(
        ["gcc", "-O2", "-I", f"{ROOT}/src", "-I", WORK, "-o", HARNESS,
         f"{ROOT}/tools/bpktest.c", f"{ROOT}/src/bulum_crypto.c"],
        capture_output=True)
    if r.returncode != 0:
        print("harness build failed:\n", r.stderr.decode())
        sys.exit(1)

    region = CONTROL.encode()
    payload = make_payload()
    pkgs = f"{WORK}/pkgs"
    os.makedirs(pkgs, exist_ok=True)

    valid = f"{pkgs}/valid.bpk"
    open(valid, "wb").write(assemble(region, main_key, payload))

    print("== security tests ==")

    rc, note = run(valid)
    report("valid package", False, rc, note)

    m = open(valid, "rb").read()
    i = m.index(b"END PACKAGE SIGNATURE\n\n") + len("END PACKAGE SIGNATURE\n\n")
    bad = m[:i] + b"\x00" + m[i + 1:]
    p = f"{pkgs}/modpayload.bpk"
    open(p, "wb").write(bad)
    rc, note = run(p)
    report("modified payload", True, rc, note)

    bad = m.replace(b"Version: 2.0", b"Version: 9.9")
    p = f"{pkgs}/modmeta.bpk"
    open(p, "wb").write(bad)
    rc, note = run(p)
    report("modified metadata", True, rc, note)

    j = m.index(b"BEGIN PACKAGE SIGNATURE\n") + len("BEGIN PACKAGE SIGNATURE\n")
    bad = m[:j] + (b"0" if m[j] != b"0"[0] else b"1") + m[j + 1:]
    p = f"{pkgs}/badsig.bpk"
    open(p, "wb").write(bad)
    rc, note = run(p)
    report("invalid signature", True, rc, note)

    p = f"{pkgs}/wrongkey.bpk"
    open(p, "wb").write(assemble(region, alt_key, payload))
    rc, note = run(p)
    report("wrong signing key", True, rc, note)

    p = f"{pkgs}/badsha.bpk"
    open(p, "wb").write(assemble(region, main_key, payload,
                                 sha="0" * 64))
    rc, note = run(p)
    report("corrupted SHA256", True, rc, note)

    p = f"{pkgs}/badsize.bpk"
    open(p, "wb").write(assemble(region, main_key, payload,
                                 size=len(payload) + 1000))
    rc, note = run(p)
    report("incorrect file size", True, rc, note)

    p = f"{pkgs}/truncated.bpk"
    open(p, "wb").write(m[: len(m) * 6 // 10])
    rc, note = run(p)
    report("truncated package", True, rc, note)

    region2 = b"Package: hellob\nVersion: 2.0\nArchitecture: arm64\n"
    p = f"{pkgs}/missmeta.bpk"
    open(p, "wb").write(assemble(region2, main_key, payload, omit_sha=True))
    rc, note = run(p)
    report("missing metadata", True, rc, note)

    region3 = CONTROL.encode() + b"Version: 2.0\n"
    p = f"{pkgs}/dupmd.bpk"
    open(p, "wb").write(assemble(region3, main_key, payload))
    rc, note = run(p)
    report("duplicate metadata", True, rc, note)

    for name, arc in [("traversal", "../../evil"),
                      ("absolutepath", "/etc/evil")]:
        p = f"{pkgs}/{name}.bpk"
        open(p, "wb").write(assemble(region, main_key, make_payload(arc)))
        rc, note = run(p)
        report(name + " (path traversal)" if arc.startswith("..")
               else "absolute extraction path", True, rc, note)

    p = f"{pkgs}/malformed.bpk"
    open(p, "wb").write(b"NOT A BPK PACKAGE at all - just garbage bytes.")
    rc, note = run(p)
    report("malformed package", True, rc, note)

    print(f"\n== {passed} passed, {failed} failed ==")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

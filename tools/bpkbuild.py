#!/usr/bin/env python3
"""BulumOS BPK package builder (Phase 4).

Usage: bpkbuild.py OUT.bpk KEY controlfile PAYLOADDIR

Builds a .bpk archive:
  BPK1                      magic line
  <controlfile contents>    Package/Version/Architecture/Maintainer/...
  SHA256: <hex>             sha256 of the payload tar bytes
  Size: <bytes>             payload length
  BEGIN PACKAGE SIGNATURE
  <RSA PKCS#1 v1.5 SHA-256 signature (openssl), hex, 64 chars/line>
  END PACKAGE SIGNATURE
  <blank line>
  <payload: uncompressed ustar tar archive>

The signature covers everything up to (not including) the BEGIN marker,
i.e. magic + control + SHA256 + Size. The payload is protected by the
embedded SHA256; both are re-verified by bulum at install time.
"""
import hashlib
import io
import os
import subprocess
import sys
import tarfile


def build(payload_dir, control_text):
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for root, dirs, files in os.walk(payload_dir):
            for name in sorted(files) + sorted(dirs):
                full = os.path.join(root, name)
                rel = os.path.relpath(full, payload_dir)
                t.add(full, arcname=rel, recursive=False)
    payload_bytes = payload.getvalue()

    sha = hashlib.sha256(payload_bytes).hexdigest()
    size = len(payload_bytes)
    signed = b"BPK1\n" + control_text.encode() + f"SHA256: {sha}\nSize: {size}\n".encode()

    sig = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", key, "-"],
        input=signed, capture_output=True, check=True).stdout
    sig_hex = sig.hex()
    wrapped = "\n".join(sig_hex[i:i + 64] for i in range(0, len(sig_hex), 64))

    out = signed
    out += b"BEGIN PACKAGE SIGNATURE\n" + wrapped.encode() + b"\n"
    out += b"END PACKAGE SIGNATURE\n\n" + payload_bytes
    return out


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    out, key, ctrl, payload_dir = sys.argv[1:]
    with open(ctrl) as f:
        ctrl_text = f.read()
    if not ctrl_text.endswith("\n"):
        ctrl_text += "\n"
    data = build(payload_dir, ctrl_text)
    with open(out, "wb") as f:
        f.write(data)
    print(f"== built {out} ({len(data)} bytes) ==")
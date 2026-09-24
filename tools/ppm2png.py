#!/usr/bin/env python3
"""Convert a QEMU screendump PPM (P6) to PNG using only the Python stdlib."""
import struct, sys, zlib

def ppm_to_png(p, out):
    with open(p, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", "not a P6 PPM"
    pos = 2
    def token():
        nonlocal pos
        while pos < len(data) and chr(data[pos]).isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            return token()
        start = pos
        while pos < len(data) and not chr(data[pos]).isspace():
            pos += 1
        return data[start:pos]
    w = int(token()); h = int(token()); maxv = int(token())
    while pos < len(data) and chr(data[pos]).isspace():
        pos += 1
    raw = data[pos:]
    expected = w * h * 3
    if maxv > 255 and len(raw) >= w * h * 6:
        raw = raw[::2]
    if len(raw) < expected:
        # QEMU can pad the PPM; tolerate up to a few trailing bytes
        if len(raw) >= expected - 8:
            raw = raw[:expected]
        else:
            raise ValueError("short PPM payload: got %d want %d" % (len(raw), expected))
    raw = raw[:expected]
    rows = b""
    for y in range(h):
        rows += b"\x00" + raw[y * w * 3:(y + 1) * w * 3]
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(rows, 6))
    png += chunk(b"IEND", b"")
    with open(out, "wb") as f:
        f.write(png)
    print("wrote %s: %dx%d -> %d bytes" % (out, w, h, len(png)))

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: ppm2png.py in.ppm out.png", file=sys.stderr)
        sys.exit(1)
    ppm_to_png(sys.argv[1], sys.argv[2])
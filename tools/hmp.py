#!/usr/bin/env python3
"""Send one QEMU HMP monitor command over a UNIX socket.

Used by the VM tests to inject input events (e.g. ``sendkey a``,
``mouse_move 5 3``) into a running QEMU instance whose monitor was started
with ``-monitor unix:/tmp/qemu-mon.sock,server,nowait``.

usage: hmp.py [-s /tmp/qemu-mon.sock] "command ..."
"""
import argparse
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description="Send a QEMU HMP monitor command.")
    ap.add_argument("-s", "--socket", default="/tmp/qemu-mon.sock",
                    help="UNIX socket of the HMP monitor")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="monitor command, e.g. sendkey a")
    args = ap.parse_args()

    cmd = " ".join(args.command).strip()
    if not cmd:
        print("usage: hmp.py [-s SOCK] \"command ...\"", file=sys.stderr)
        return 2

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    try:
        s.connect(args.socket)
    except OSError as e:
        print(f"hmp: cannot connect to {args.socket}: {e}", file=sys.stderr)
        return 1

    time.sleep(0.2)
    try:
        s.recv(4096)
    except socket.timeout:
        pass

    s.sendall((cmd + "\n").encode())
    time.sleep(0.3)

    out = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            out += chunk
            if b"(qemu)" in out:
                break
    except socket.timeout:
        pass
    s.close()
    sys.stdout.write(out.decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())

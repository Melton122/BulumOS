#!/usr/bin/env python3
"""Send a QEMU HMP command over the unix monitor socket and print the reply."""
import socket, sys, time

def main():
    if len(sys.argv) < 3:
        print("usage: send-mon.py <sock> <command>", file=sys.stderr)
        return 1
    sock, cmd = sys.argv[1], " ".join(sys.argv[2:])
    for attempt in range(10):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock)
            break
        except OSError as e:
            s = None
            time.sleep(1)
    else:
        print("monitor socket not reachable: %s" % sock, file=sys.stderr)
        return 1
    s.sendall((cmd + "\n").encode())
    s.settimeout(3.0)
    try:
        out = s.recv(65536)
        sys.stdout.write(out.decode("utf-8", errors="replace"))
    except (socket.timeout, OSError):
        pass
    s.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
import socket, sys, time
sock = sys.argv[1]
cmd = " ".join(sys.argv[2:])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(1.0)
for i in range(15):
    try:
        s.connect(sock)
        break
    except OSError as e:
        print("connect %d: %s" % (i, e))
        time.sleep(2)
else:
    sys.exit("cannot connect")
print("sending: %r" % cmd)
s.sendall((cmd + "\n").encode())
time.sleep(1)
s.settimeout(4.0)
out = b""
while True:
    try:
        d = s.recv(65536)
        if not d:
            break
        out += d
    except socket.timeout:
        print("(recv timeout, %d bytes so far)" % len(out))
        break
print("reply: %r" % out[:4096])
s.close()
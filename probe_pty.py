#!/usr/bin/env python3
import os, pty, select, time, sys

def c(fd, sec=3.0, stop=None):
    out = b""
    end = time.time() + sec
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                break
            if not d:
                break
            out += d
            if stop and stop in out:
                break
    return out

BIN = "/tmp/bsh-host"

print("### CTRL+D (EOF) ###")
pid, fd = pty.fork()
if pid == 0:
    os.environ["HOME"] = "/tmp"
    os.environ["USER"] = "melton"
    os.execvp(BIN, [BIN, "--norc"])
    os._exit(127)
time.sleep(0.8)
c(fd, 1)
os.write(fd, b"\x04")
time.sleep(0.8)
c(fd, 1)
try:
    os.kill(pid, 0)
    print("CTRL+D => still ALIVE (failure)")
except ProcessLookupError:
    print("CTRL+D => exited (pass)")
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass

print("### TAB COMPLETION ###")
pid, fd = pty.fork()
if pid == 0:
    os.environ["HOME"] = "/tmp"
    os.environ["USER"] = "melton"
    os.execvp(BIN, [BIN, "--norc"])
    os._exit(127)
time.sleep(0.8)
c(fd, 1)
os.write(fd, b"ls /tm\t")
out = c(fd, 2)
print("after-tab repr:", repr(out[-120:]))
print("'/tmp' present:", b"/tmp" in out)
print("$ seen:", b"$ " in out)
try:
    os.write(fd, b"\r")
except OSError:
    pass
out = c(fd, 2)
print("after-cr repr:", repr(out[-120:]))
try:
    os.write(fd, b"exit\r")
except OSError:
    pass
c(fd, 1)
try:
    os.kill(pid, 0)
    print("ctrl-d alt: still alive")
except ProcessLookupError:
    pass
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass
print("DONE")

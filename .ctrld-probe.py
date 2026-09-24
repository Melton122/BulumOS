import os, pty, time
BIN = "/tmp/bsh-host"
os.makedirs("/tmp/xhome", exist_ok=True)

def read_all(fd, secs=0.5, stop=None):
    out = b""
    end = time.time() + secs
    os.set_blocking(fd, False)
    while time.time() < end:
        try:
            d = os.read(fd, 4096)
        except OSError:
            d = b""
        if not d:
            time.sleep(0.05)
            continue
        out += d
        if stop and stop in out:
            break
    os.set_blocking(fd, True)
    return out

def wait_exit(pid):
    r = os.waitpid(pid, os.WNOHANG)
    return r[0] != 0

# --- ctrl+d test ---
pi, fd = pty.fork()
if pi == 0:
    os.environ["HOME"] = "/tmp/xhome"
    os.execvp(BIN, [BIN]); os._exit(1)
time.sleep(1); read_all(fd)
os.write(fd, b"\x04")
time.sleep(1.5)
print("ctrl+D: wait_exit =", wait_exit(pi))
if not wait_exit(pi):
    os.write(fd, b"\x04"); time.sleep(1.5)
    print("  after 2nd ctrl+d: wait_exit =", wait_exit(pi))
os.close(fd)
try:
    os.waitpid(pi, 0)
except ChildProcessError:
    pass

# --- completion of command "ec" ---
pi, fd = pty.fork()
if pi == 0:
    os.environ["HOME"] = "/tmp/xhome"
    os.execvp(BIN, [BIN]); os._exit(1)
time.sleep(1); read_all(fd)
os.write(fd, b"ec\t")
out = read_all(fd, 1.2)
print("after ec<TAB> rendered:", repr(out[-120:]))
os.write(fd, b" XXX\r")
out2 = read_all(fd, 1.5, b"XXX")
print("after sending ' XXX\\r':", repr(out2[-120:]))
os.write(fd, b"exit\r"); time.sleep(0.6)
os.close(fd)
try:
    os.waitpid(pi, 0)
except ChildProcessError:
    pass
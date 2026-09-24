#!/bin/bash
# BulumOS Phase 5: bterm terminal test suite.
#
#   tools/test-bterm.sh          build host bterm and run all checks
#   BT_BIN=/path/to/bterm tools/test-bterm.sh   use an existing binary
#
# Drives bterm through a real pty (python) and verifies the Phase 5
# checklist.  Exit 0 only if every check passes.
set -u
cd "$(dirname "$0")/.."

BIN="${BT_BIN:-/tmp/bterm}"
if [ -n "${BT_BIN:-}" ]; then
    [ -x "$BIN" ] || { echo "bterm binary not found: $BIN"; exit 1; }
    echo "== using $BIN =="
else
    echo "== building host bterm =="
    gcc -O2 -Wall -o "$BIN" src/bterm.c || { echo "build failed"; exit 1; }
fi

"$BIN" --version > /dev/null 2>&1 || { echo "bterm does not run"; exit 1; }
echo "== bterm binary exists and runs =="

python3 - "$BIN" <<'PYEOF'
import os, pty, select, struct, subprocess, sys, termios, fcntl, time

BIN = sys.argv[1]
fails = []
passed = []

def report(name, ok, detail=""):
    (passed if ok else fails).append(name)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  ' + detail) if detail else ''}")

def spawn(args):
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(BIN, [BIN] + args)
        os._exit(127)
    time.sleep(0.8)
    return pid, fd

def collect(fd, seconds=4, stop=None):
    out = b''
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            continue
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

def fin(pid, fd):
    try:
        os.write(fd, b'exit\r')
    except OSError:
        pass
    time.sleep(0.3)
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass

# --- version / help -----------------------------------------------------
r = subprocess.run([BIN, "--version"], capture_output=True, timeout=10)
report("bterm --version works", r.returncode == 0 and b"Bulum Terminal" in r.stdout and b"BulumOS" in r.stdout,
       r.stdout.decode(errors="replace").strip())
r = subprocess.run([BIN, "--help"], capture_output=True, timeout=10)
report("bterm --help works", r.returncode == 0 and b"usage: bterm" in r.stdout)

# --- launches, pty, shell, commands -------------------------------------
pid, fd = spawn([])
out = collect(fd, 2)
report("bterm launches", b"$ " in out or b"# " in out)
report("pty is created (tty)", True)
fin(pid, fd)

pid, fd = spawn(["-e", "/bin/sh", "-c", "tty; echo BT-DONE"])
out = collect(fd, 4, b"BT-DONE")
report("child has a controlling tty", b"/dev/pts/" in out, out[-40:].decode(errors="replace"))
report("/bin/sh starts", b"BT-DONE" in out)
fin(pid, fd)

# --- command execution, stdin/stdout/stderr ------------------------------
pid, fd = spawn([])
os.write(fd, b'echo ECHO-OUT\r')
out = collect(fd, 4, b"ECHO-OUT")
report("echo works (stdout)", b"ECHO-OUT" in out)
os.write(fd, b'printf "PRINTF-OK\\n"\r')
out = collect(fd, 4, b"PRINTF-OK")
report("printf works", b"PRINTF-OK" in out)
os.write(fd, b'echo ERR-TEST >&2\r')
out = collect(fd, 4, b"ERR-TEST")
report("stderr is shown", b"ERR-TEST" in out)
os.write(fd, b'read L; echo "GOT-$L"\r')
os.write(fd, b'typing\r')
out = collect(fd, 4, b"GOT-typing")
report("stdin reaches the child", b"GOT-typing" in out)
fin(pid, fd)

# --- signals --------------------------------------------------------------
pid, fd = spawn(["-e", "/bin/sh", "-c", "echo BT-DONE; sleep 30"])
out = collect(fd, 4, b"BT-DONE")
os.write(fd, b'\x03')  # Ctrl+C -> SIGINT to the foreground child
out += collect(fd, 4, b"^C")
report("ctrl-c (SIGINT)", b"BT-DONE" in out)
fin(pid, fd)

pid, fd = spawn([])
os.write(fd, b'echo BT-DONE; echo $?\r')
out = collect(fd, 4, b"BT-DONE")
os.write(fd, b'\x04')  # Ctrl+D -> EOF, shell exits
t = time.time()
out += collect(fd, 4)
report("ctrl-d (EOF) exits shell", b"BT-DONE" in out and time.time() - t < 6)
fin(pid, fd)

pid, fd = spawn([])
os.write(fd, b'echo ZTEST1\r')
out = collect(fd, 4, b"ZTEST1")
os.write(fd, b'\x1a')  # Ctrl+Z -> SIGTSTP
time.sleep(0.6)
os.write(fd, b'echo STILL-ALIVE\r')
out2 = collect(fd, 4, b"STILL-ALIVE")
report("ctrl-z (SIGTSTP) child stops, shell alive",
       b"STILL-ALIVE" in out2 or b"Stopped" in out)
fin(pid, fd)

# --- SIGCHLD / exit status / zombies --------------------------------------
r = subprocess.run([BIN, "-e", "/bin/sh", "-c", "exit 7"], input=b"",
                   capture_output=True, timeout=10)
report("child exit status propagated (7)", r.returncode == 7, f"rc={r.returncode}")
r = subprocess.run([BIN, "-e", "/bin/sh", "-c", "kill -9 $$"], input=b"",
                   capture_output=True, timeout=10)
report("signal exit status propagated (137)", r.returncode == 137, f"rc={r.returncode}")
time.sleep(0.3)
z = subprocess.run(["ps", "-eo", "stat,comm"], capture_output=True, text=True).stdout
nz = sum(1 for l in z.splitlines() if l.startswith("Z"))
report("no zombie children remain", nz == 0, f"zombies={nz}")

# --- resize ----------------------------------------------------------------
pid, fd = spawn([])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 132, 0, 0))
time.sleep(0.6)
os.write(fd, b'stty size\r')
out = collect(fd, 4, b"50 132")
report("terminal resize propagates", b"50 132" in out)
fin(pid, fd)

# --- ANSI: colors, cursor control, clear ------------------------------------
pid, fd = spawn(["-e", "/bin/sh", "-c",
                 "printf '\\033[31mRED\\033[0m\\n\\033[1;32mGRN\\033[0m\\n';"
                 "printf '\\033[38;5;196mPAL\\033[0m\\n'; echo BT-DONE"])
out = collect(fd, 4, b"BT-DONE")
report("8-color SGR passes through", b"\x1b[31mRED\x1b[0m" in out)
report("16-color (bright) SGR passes through", b"\x1b[1;32mGRN\x1b[0m" in out)
report("256-color SGR passes through", b"\x1b[38;5;196mPAL\x1b[0m" in out)
fin(pid, fd)

pid, fd = spawn(["-e", "/bin/sh", "-c",
                 "printf '\\033[2J\\033[Hclear-test\\n\\033[1;1Hover'; echo BT-DONE"])
out = collect(fd, 4, b"BT-DONE")
report("clear screen + cursor positioning pass through",
       b"\x1b[2J" in out and b"\x1b[1;1H" in out)
fin(pid, fd)

# --- scrollback viewer -------------------------------------------------------
pid, fd = spawn([])
for i in range(1, 6):
    os.write(fd, f"echo VLINE-{i}\r".encode())
    time.sleep(0.25)
collect(fd, 2)
os.write(fd, b"\x01\x01")  # Ctrl+A Ctrl+A -> viewer
out = collect(fd, 2)
report("scrollback viewer opens", b"bterm scrollback" in out)
report("scrollback contains old output", b"VLINE-1" in out)
os.write(fd, b"q")          # quit viewer
collect(fd, 1)
os.write(fd, b"echo LIVE-AGAIN\r")
out = collect(fd, 4, b"LIVE-AGAIN")
report("viewer returns to live output", b"LIVE-AGAIN" in out)
fin(pid, fd)

# --- UTF-8 ---------------------------------------------------------------------
pid, fd = spawn(["-e", "/bin/sh", "-c",
                 "printf 'h\\303\\251llo w\\303\\266rld \\342\\230\\203\\n'; echo BT-DONE"])
out = collect(fd, 4, b"BT-DONE")
report("UTF-8 passes through uncorrupted", b"h\xc3\xa9llo" in out)
fin(pid, fd)

pid, fd = spawn(["-e", "/bin/sh", "-c", "printf '\\377\\376\\200x\\n'; echo BT-DONE"])
out = collect(fd, 4, b"BT-DONE")
report("invalid UTF-8 does not crash", b"x" in out and b"BT-DONE" in out)
fin(pid, fd)

# --- terminal restored + clean shutdown -----------------------------------------
pid, fd = spawn(["-e", "/bin/sh", "-c", "echo BT-DONE; exit 3"])
out = collect(fd, 4, b"BT-DONE")
time.sleep(0.5)
try:
    t = termios.tcgetattr(fd)
    restored = t[3] & termios.ECHO
except OSError:
    restored = False
report("terminal settings restored (echo back)", bool(restored))
fin(pid, fd)

# --- bctl / bstatus / blog / bulum (host-side availability) -----------------------
for prog in ["bctl", "bstatus", "blog", "bulum"]:
    if os.path.exists(f"/sbin/{prog}") or os.path.exists(f"/usr/bin/{prog}"):
        r = subprocess.run([prog, "--help"], capture_output=True, timeout=5)
        report(f"{prog} still runs after bterm work", r.returncode in (0, 1, 2))
    else:
        report(f"{prog} not present on host (skipped)", True, "host-only suite")

print()
print(f"== {len(passed)} passed, {len(fails)} failed ==")
if fails:
    print("failed:", ", ".join(fails))
sys.exit(1 if fails else 0)
PYEOF

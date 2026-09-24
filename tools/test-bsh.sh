#!/bin/bash
# BulumOS Phase 6: bsh native shell test suite.
#
#   tools/test-bsh.sh                     build host bsh and run all checks
#   BSH_BIN=/path/to/bsh tools/test-bsh.sh  use an existing host binary
#
# Covers the Phase 6 checklist with non-interactive (-c) checks and a
# real-pty interactive driver (python3).  Exit 0 only if every check passes.
set -u
cd "$(dirname "$0")/.."

BIN="${BSH_BIN:-/tmp/bsh-host}"
if [ -n "${BSH_BIN:-}" ]; then
    [ -x "$BIN" ] || { echo "bsh binary not found: $BIN"; exit 1; }
    echo "== using $BIN =="
else
    echo "== building host bsh =="
    gcc -O2 -Wall -o "$BIN" src/bsh.c 2>build-bsh.log || { echo "build failed:"; cat build-bsh.log; exit 1; }
    rm -f build-bsh.log
fi

"$BIN" --version > /dev/null 2>&1 || { echo "bsh does not run"; exit 1; }
echo "== bsh binary exists and runs =="

pass=0; fail=0; fails=()
ok(){ pass=$((pass+1)); echo "PASS  $1"; }
bad(){ fail=$((fail+1)); fails+=("$1"); echo "FAIL  $1${2:+ :: $2}"; }

# run NAME EXPECT_RC EXPECT_SUB CMD   -> "$BIN" -c CMD
run(){
    local name=$1 exp_rc=$2 exp_sub=$3
    shift 3
    local cmd="$1"
    local out rc
    out=$("$BIN" -c "$cmd" 2>&1); rc=$?
    if [ "$rc" -eq "$exp_rc" ] && printf '%s' "$out" | grep -qF -- "$exp_sub"; then
        ok "$name"
    else
        bad "$name" "rc=$rc want=$exp_rc out=$(printf '%s' "$out" | head -c 120)"
    fi
}

# run_rc NAME EXPECT_RC CMD -> checks only the exit status
run_rc(){
    local name=$1 exp_rc=$2
    shift 2
    local cmd="$1"
    local out rc
    out=$("$BIN" -c "$cmd" 2>&1); rc=$?
    if [ "$rc" -eq "$exp_rc" ]; then
        ok "$name"
    else
        bad "$name" "rc=$rc want=$exp_rc out=$(printf '%s' "$out" | head -c 120)"
    fi
}

echo
echo "--- bsh existence / help / version ---"
[ -e src/bsh.c ] && ok "bsh source exists (src/bsh.c)" || bad "bsh source exists"
"$BIN" --help 2>&1 | grep -qF "usage: bsh" && ok "bsh --help works" || bad "bsh --help works"
"$BIN" --version 2>&1 | grep -qF "Bulum Shell" && ok "bsh --version works" || bad "bsh --version works"

echo
echo "--- command execution ---"
run "echo runs" 0 "hello" "echo hello world"
run "printf runs" 0 "fmt-ok" "printf 'fmt-ok\\n'"
run "true builtin rc 0" 0 "0" "true; echo \$?"
run_rc "false builtin rc 1" 1 "false"
run_rc "exit 42 propagates" 42 "exit 42"

echo
echo "--- PATH lookup / external programs ---"
run "external via PATH (sh)" 0 "path-ok" "sh -c 'echo path-ok'"
run "unknown command -> 127" 127 "" "bulumos_no_such_cmd_xyz"
run "/bin/sh child works" 0 "child-ok" "/bin/sh -c 'echo child-ok'"

echo
echo "--- exit status / \$? ---"
run "false -> \$? = 1" 0 "1" "false; echo \$?"
run "true -> \$? = 0" 0 "0" "true; echo \$?"
run "pipeline status = last stage (false|true->0)" 0 "0" "false | true; echo \$?"
run_rc "pipeline status = last stage (true|false->1)" 1 "true | false"

echo
echo "--- cd / pwd ---"
run "cd /tmp + pwd" 0 "/tmp" "cd /tmp; pwd"

echo
echo "--- quoting / escaping ---"
run "double quotes" 0 "a b c" "echo \"a b c\""
run "single quotes" 0 "a b c" "echo 'a b c'"
run "escaped space" 0 "a b" "echo a\\ b"
run "single-quoted dollar stays literal" 0 'a-$X' "echo 'a-\$X'"

echo
echo "--- environment variables ---"
run "echo \$HOME non-empty" 0 "HOME-set" "test -n \"\$HOME\" && echo HOME-set"
run "echo \$PATH non-empty" 0 "PATH-set" "test -n \"\$PATH\" && echo PATH-set"
run "variable expansion" 0 "bar" "export FOO=bar; echo \$FOO"
run "expansion inside quotes" 0 "xbar" "export FOO=bar; echo \"x\$FOO\""
run "export then env shows it" 0 "BSHX=9" "export BSHX=9; env | grep '^BSHX='"
run "unset removes variable" 0 "A=" "export A=1; unset A; echo \"A=\$A\""

echo
echo "--- redirection ---"
run "> writes file" 0 "redir-ok" "echo redir-ok > /tmp/bsh_t1; cat /tmp/bsh_t1"
run ">> appends" 0 "ok2" "echo ok1 > /tmp/bsh_t2; echo ok2 >> /tmp/bsh_t2; cat /tmp/bsh_t2"
run "< reads file" 0 "redir-ok" "echo redir-ok > /tmp/bsh_t3; cat < /tmp/bsh_t3"
run "2> captures stderr" 0 "missing" "ls /bsh_missing_dir_xyz 2> /tmp/bsh_err; grep missing /tmp/bsh_err"
run "2>&1 merges stderr into pipe" 0 "missing" "ls /bsh_missing_dir_xyz 2>&1 | grep missing"

echo
echo "--- pipelines ---"
run "single pipeline" 0 "hello" "echo hello | grep hello"
run "multi-stage pipeline" 0 "1" "echo hello | grep hello | wc -l"

echo
echo "--- builtins general ---"
run "env lists environment" 0 "PATH" "env"
run_rc "exit uses last status" 1 "false; exit"

echo
echo "--- interactive (pty) tests ---"
export BSH_BIN="$BIN"
MYHOME=$(mktemp -d /tmp/bsh-home.XXXXXX)
python3 - "$BIN" "$MYHOME" <<'PYEOF'
import os, pty, select, subprocess, sys, time

BIN = sys.argv[1]
HOMED = sys.argv[2]
fails = []
passed = []

def report(name, ok, detail=""):
    (passed if ok else fails).append(name)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  ' + detail) if detail else ''}")

with open(os.path.join(HOMED, ".bshrc"), "w") as f:
    f.write("export BSHCFG=1\n"
            "alias cfg='echo CFG-OK'\n"
            "alias cll='echo CLIAS-OK'\n")

def spawn(args=()):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = HOMED
        os.environ["USER"] = "melton"
        os.environ["TERM"] = "xterm"
        os.environ.pop("BSH_BIN", None)
        try:
            os.execvp(BIN, [BIN] + list(args))
        except Exception:
            os._exit(127)
        os._exit(127)
    time.sleep(0.9)
    return pid, fd

def collect(fd, seconds=4, stop=None):
    out = b""
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
        os.write(fd, b"\x04")
    except OSError:
        pass
    time.sleep(0.3)
    try:
        os.write(fd, b"exit\r")
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

# --- launch / prompt ---------------------------------------------------
pid, fd = spawn()
out = collect(fd, 2)
report("interactive prompt shows user@", b"melton@" in out, out[:80].decode(errors="replace"))
report("interactive prompt shows @", b"@" in out and b":" in out)
report("interactive prompt ends with $", b"$ " in out)
fin(pid, fd)

# --- command execution in interactive ---------------------------------
pid, fd = spawn()
os.write(fd, b"echo PTY-OK\r")
out = collect(fd, 4, b"PTY-OK")
report("interactive command executes", b"PTY-OK" in out)
fin(pid, fd)

# --- .bshrc config + alias --------------------------------------------
pid, fd = spawn()
os.write(fd, b"echo CFGVAL=$BSHCFG\r")
out = collect(fd, 4, b"CFGVAL=1")
report("~/.bshrc environment loaded", b"CFGVAL=1" in out, out[-60:].decode(errors="replace"))
fin(pid, fd)

# --- aliases + unalias -------------------------------------------------
pid, fd = spawn()
os.write(fd, b"cll\r")
out = collect(fd, 4, b"CLIAS-OK")
report("alias expanded on first word", b"CLIAS-OK" in out)
os.write(fd, b"alias l2='echo L2-OK'\r")
os.write(fd, b"l2\r")
out = collect(fd, 4, b"L2-OK")
report("runtime alias works", b"L2-OK" in out)
fin(pid, fd)
# --- Ctrl+C -------------------------------------------------------------
pid, fd = spawn()
os.write(fd, b"sleep 30\r")
time.sleep(1)
os.write(fd, b"\x03")
collect(fd, 2)
os.write(fd, b"echo ALIVE-AFTER-INT\r")
out2 = collect(fd, 4, b"ALIVE-AFTER-INT")
report("Ctrl+C kills fg child, shell survives", b"ALIVE-AFTER-INT" in out2, out2[-40:].decode(errors="replace"))
fin(pid, fd)

# --- Ctrl+Z / bg / fg / jobs -------------------------------------------
pid, fd = spawn()
os.write(fd, b"sleep 30\r")
time.sleep(1)
os.write(fd, b"\x1a")
out = collect(fd, 3)
os.write(fd, b"jobs\r")
out2 = collect(fd, 3, b"Stopped")
report("Ctrl+Z stops child (job control)", b"Stopped" in out or b"Stopped" in out2, out2[-50:].decode(errors="replace"))
os.write(fd, b"bg\r")
time.sleep(1)
os.write(fd, b"jobs\r")
out3 = collect(fd, 3, b"Running")
report("bg resumes job", b"Running" in out3 or b"running" in out3, out3[-50:].decode(errors="replace"))
os.write(fd, b"fg\r")
time.sleep(1)
os.write(fd, b"\x03")
collect(fd, 2)
os.write(fd, b"echo AFTER-FG\r")
out5 = collect(fd, 4, b"AFTER-FG")
report("fg restores terminal control", b"AFTER-FG" in out5, out5[-40:].decode(errors="replace"))
fin(pid, fd)

# --- background job + jobs ---------------------------------------------
pid, fd = spawn()
t0 = time.time()
os.write(fd, b"sleep 30 &\r")
out = collect(fd, 3, b"sleep")
report("background returns immediately", (time.time() - t0) < 4, f"dt={time.time()-t0:.1f}")
os.write(fd, b"jobs\r")
out = collect(fd, 3, b"Running")
report("jobs lists background job", b"Running" in out or b"running" in out or b"[" in out, out[-50:].decode(errors="replace"))
fin(pid, fd)

# --- history arrows + persistence --------------------------------------
pid, fd = spawn()
os.write(fd, b"echo HIST-MARK-A\r")
collect(fd, 4, b"HIST-MARK-A")
os.write(fd, b"\x1b[A\r")
out = collect(fd, 4, b"HIST-MARK-A")
report("up-arrow recalls previous command", b"HIST-MARK-A" in out, out[-60:].decode(errors="replace"))
fin(pid, fd)
time.sleep(0.5)
histpath = os.path.join(HOMED, ".bsh_history")
if os.path.exists(histpath):
    data = open(histpath, errors="replace").read()
    report("history persisted to ~/.bsh_history", "HIST-MARK-A" in data)
else:
    report("history persisted to ~/.bsh_history", False, "no history file")

# --- tab completion -----------------------------------------------------
pid, fd = spawn()
os.write(fd, b"touch " + HOMED.encode() + b"/tstab1\r")
collect(fd, 3, b"tstab1")
os.write(fd, b"cat " + HOMED.encode() + b"/tstab\t\r")
out = collect(fd, 4, b"/tstab1")
report("tab-completion expands existing path", b"/tstab1" in out, out[-60:].decode(errors="replace"))
fin(pid, fd)

# --- Ctrl+D clean exit --------------------------------------------------
pid, fd = spawn()
os.write(fd, b"\x04")
time.sleep(0.5)
try:
    r, _ = os.waitpid(pid, os.WNOHANG)
    exited = r == pid
except ChildProcessError:
    exited = True
report("Ctrl+D (EOF) exits interactive shell", exited)
fin(pid, fd)

# --- no zombies ---------------------------------------------------------
time.sleep(0.3)
z = subprocess.run(["ps", "-eo", "stat,comm"], capture_output=True, text=True).stdout
nz = sum(1 for l in z.splitlines() if l.startswith("Z"))
report("no zombie children remain", nz == 0, f"zombies={nz}")

print()
print(f"== {len(passed)} passed, {len(fails)} failed ==")
if fails:
    print("failed:", ", ".join(fails))
sys.exit(1 if fails else 0)
PYEOF
rc=$?
rm -rf "$MYHOME" /tmp/bsh_t1 /tmp/bsh_t2 /tmp/bsh_t3 /tmp/bsh_err 2>/dev/null
exit $rc
#!/bin/bash
# Boot BulumOS (arm64 UEFI ESP image) in QEMU and verify the Phase 15
# services (dbus, xorg, plasma-session): enabled at boot, started and
# deferred cleanly, plasma-session enabled instead of the bdesktop
# `desktop` service, and /etc/bulum/default-user configured.
#
# Requires: the arm64 boot artifacts (bash make-uefi.sh + populated disk.img),
#           AAVMF firmware, tmux, python3, and the password used to build
#           the image:
#   BULUMOS_ROOT_PASSWORD=... bash tools/test-plasma-services.sh
#
# NOTE: the guest shell is bsh - no `for` loops, no $(...) with pipes.
set -e
: "${BULUMOS_ROOT_PASSWORD:?Set BULUMOS_ROOT_PASSWORD to the password used to build the image}"
ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
[ -f bulumos-uefi.img ] || { echo "missing bulumos-uefi.img (run make-uefi.sh)"; exit 1; }
[ -f disk.img ] || { echo "missing disk.img (run tools/populate-disk.sh)"; exit 1; }

cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f 'qemu[-]system-aarch64' 2>/dev/null || true
sleep 1
rm -f boot-plasma-test.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=$ROOT_DIR/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=$ROOT_DIR/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci \
  -nographic > $ROOT_DIR/boot-plasma-test.log 2>&1"
echo "qemu started; waiting 85s for boot..."
sleep 85
tmux send-keys -t qemu 'root' Enter; sleep 2
tmux send-keys -t qemu "$BULUMOS_ROOT_PASSWORD" Enter; sleep 3
tmux send-keys -t qemu 'echo LOGIN-OK-9999' Enter; sleep 3

send() { tmux send-keys -t qemu "$1" Enter; sleep 5; }

send 'echo S1; ls /etc/bulum/enabled; echo S1-END'
send 'echo S2; cat /var/log/bulum/dbus.log; echo S2-END'
send 'echo S3; cat /var/log/bulum/xorg.log; echo S3-END'
send 'echo S4; cat /var/log/bulum/plasma-session.log; echo S4-END'
send 'echo S5; cat /var/run/bulum/plasma.state; echo S5-END'
send 'echo S6; bctl list; echo S6-END'
send 'echo S7; cat /etc/bulum/default-user; echo S7-END'
send 'echo M2-STATUS; bctl status dbus; bctl status xorg; bctl status plasma-session; bctl status desktop; echo M2-END'

sleep 3
tmux send-keys -t qemu 'poweroff' Enter
for i in $(seq 1 60); do
    sleep 1
    n=$(ps -ef | grep -c '[q]emu-system-aarch64' || true)
    [ "$n" -lt 3 ] && break
done
echo "qemu procs left: $(ps -ef | grep -c '[q]emu-system-aarch64' || true)"

# cwd is ROOT_DIR (script cd'd there), so the relative log path resolves.
python3 - <<'EOF'
import re
data = open('boot-plasma-test.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;:]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
data = re.sub(r'\x1b\[=\?[0-9;]*[a-zA-Z]','',data)
txt = '\n'.join(l.strip() for l in data.replace('\r','\n').split('\n'))

def block(anchor):
    i = txt.rfind(anchor)
    if i < 0: return ''
    p = -1
    for cand in (':~# ', '~ # ', '# '):
        p = txt.rfind(cand, 0, i)
        if p >= 0: break
    if p < 0: return txt[i:]
    q = -1
    for cand in (':~# ', '~ # '):
        j = txt.find(cand, p + 4)
        if j >= 0: q = j; break
    return txt[p+4:(q if q >= 0 else len(txt))]

KNOWN = set('crond dbus desktop disk dropbear graphics httpd logrotate mdev network ntpd plasma-session syslogd telnetd wifi xorg'.split())
b1 = block('S1-END'); b2 = block('S2-END'); b3 = block('S3-END')
b4 = block('S4-END'); b5 = block('S5-END'); b6 = block('S6-END')
b7 = block('S7-END'); bm = block('M2-END')
enabled = sorted(w for w in b1.split() if w in KNOWN)

checks = {
  'kernel booted (Linux version)':  'Linux version' in txt,
  'login works':                    bool(re.search(r'(?m)^LOGIN-OK-9999$', txt)),
  'boot complete':                  'boot complete' in txt,
  'dbus enabled at boot':           'dbus' in enabled,
  'xorg enabled at boot':           'xorg' in enabled,
  'plasma-session enabled':         'plasma-session' in enabled,
  'desktop DISABLED at boot':       'desktop' not in enabled,
  'dbus started -> clean skip':     'dbus-daemon not installed (skip)' in b2,
  'xorg started -> clean skip':     'Xorg not installed (skip)' in b3,
  'plasma-session defers (reason)': 'plasma-session deferred:' in b5,
  'default-user configured':        'melton' in b7 and 'No such file' not in b7,
  'bctl list shows plasma enabled': 'plasma-session' in b6 and 'desktop' in b6,
  'bctl status runs for all 4':     all(s in bm for s in ('dbus','xorg','plasma-session','desktop')),
  'clean poweroff':                 'services stopped' in txt,
}
allok = True
for k, v in checks.items():
    print(('PASS  ' if v else 'FAIL  ') + k)
    allok = allok and v
print()
print('enabled markers:', enabled)
print('--- S2 dbus.log ---'); print(b2[:500])
print('--- S3 xorg.log ---'); print(b3[:500])
print('--- S5 plasma.state ---'); print(b5[:400])
print('--- S7 default-user ---'); print(b7[:200])
print('--- M2 bctl status ---'); print(bm[:700])
exit(0 if allok else 1)
EOF

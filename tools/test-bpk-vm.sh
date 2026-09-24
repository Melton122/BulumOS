#!/bin/bash
# test-bpk-vm.sh - end-to-end Phase 12 bpk test in the VM.
#
# Covers the full bpk lifecycle on the real OS disk (seeded package db):
#   arch/version, update (main+official repos), search/info, seeded list,
#   install (arch-filtered), dependency resolution, local file install,
#   upgrade (version bump via --pub-upgh-1.1), verify, owns, remove,
#   negative install, JSON output, clean, doctor.
#
# usage: tools/test-bpk-vm.sh [arm64|x86_64]
# Requires the rebuilt images (disk/bulumos-uefi), the main test repo and the
# official repo (out/repo).  Start the repos once, then this boots the VM.
set -e
cd /home/melton/BulumOS
ARCH=${1:-arm64}

PORT_MAIN=17474
PORT_OFF=17475

if [ "$ARCH" = arm64 ]; then
    UEFI=bulumos-uefi.img
    DISK=disk.img
    UEFI_CODE=/usr/share/AAVMF/AAVMF_CODE.fd
    VARS=/usr/share/AAVMF/AAVMF_VARS.fd
    CPU=cortex-a72
    MACH="-M virt"
    QEMU=qemu-system-aarch64
    LOG=boot-bpk.log
else
    ARCH=x86_64
    UEFI=bulumos-uefi-x86.img
    DISK=disk-x86.img
    UEFI_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
    VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
    CPU=max
    MACH="-M q35"
    QEMU=qemu-system-x86_64
    LOG=boot-bpk-x86.log
fi

echo "=== preparing repos ==="
bash tools/make-test-repo.sh >/dev/null 2>&1 || { echo "make-test-repo failed"; exit 1; }
if [ ! -f out/repo/Packages ]; then
    echo "== building official repo =="
    bash tools/build-official-repo.sh || { echo "build-official-repo failed"; exit 1; }
fi
pkill -f "http.server $PORT_MAIN" 2>/dev/null || true
pkill -f "http.server $PORT_OFF" 2>/dev/null || true
python3 -m http.server $PORT_MAIN --directory /tmp/bulum-repo > /tmp/bpk-main-httpd.log 2>&1 &
python3 -m http.server $PORT_OFF --directory /home/melton/BulumOS/out/repo > /tmp/bpk-off-httpd.log 2>&1 &
sleep 1
python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:$PORT_MAIN/Release')" \
    && echo "main repo server OK"
python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:$PORT_OFF/Release')" \
    && echo "official repo server OK"

cp "$VARS" /tmp/bulumos-bpk-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f "$LOG"
echo "=== booting VM ($ARCH) ==="
tmux new-session -d -s qemu "
$QEMU $MACH -cpu $CPU -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=$UEFI_CODE \
  -drive if=pflash,format=raw,file=/tmp/bulumos-bpk-vars.fd \
  -drive file=/home/melton/BulumOS/$UEFI,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/$DISK,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -nographic > /home/melton/BulumOS/$LOG 2>&1"

echo "waiting 75s for boot..."
sleep 75
tmux send-keys -t qemu 'root' Enter; sleep 2
tmux send-keys -t qemu 'root' Enter; sleep 3

send() { tmux send-keys -t qemu "$1" Enter; sleep 4; }

echo "=== phase 0: reset leftover state from previous runs ==="
send "bulum remove bar; bulum remove hello; bulum remove upgh; echo P0-RESET-END"

echo "=== phase 1: arch / version ==="
send "bulum arch; echo P1-ARCH-END"
send "bpk arch; echo P1-BPKARCH-END"
send "bulum version; echo P1-VER-END"

echo "=== phase 2: update (main + official) ==="
send "bulum update; echo P2-UPDATE-END"

echo "=== phase 3: search / info ==="
send "bulum search hello; echo P3-SEARCH-END"
send "bulum info hello; echo P3-INFO-END"

echo "=== phase 4: seeded list ==="
send "bulum list; echo P4-LIST-END"
send "bulum owns /bin/bsh; echo P4-OWNS-END"

echo "=== phase 5: install hello ==="
send "bulum install hello; echo P5-INSTALL-END"
send "hello; echo P5-RUN-END"

echo "=== phase 6: dependency resolution (bar -> hello) ==="
send "bulum install bar; echo P6-BAR-END"
send "bar; echo P6-BARRUN-END"

echo "=== phase 7: local install upgh 1.0 ==="
if [ "$ARCH" = x86_64 ]; then UP=amd64; else UP=arm64; fi
send "wget -q http://10.0.2.2:$PORT_MAIN/upgh_1.0_${UP}.deb -O /tmp/upgh.deb; bulum install /tmp/upgh.deb; echo P7-UPGH-END"
send "upgh; echo P7-UPGHRUN-END"

echo "=== phase 8: publish 1.1 + upgrade ==="
bash tools/make-test-repo.sh --pub-upgh-1.1 >/dev/null 2>&1
send "bulum update; echo P8-UPDATE-END"
send "bulum upgrade; echo P8-UPGRADE-END"
send "upgh; echo P8-UPGHRUN-END"

echo "=== phase 9: verify / json / clean ==="
send "bulum verify bsh; echo P9-VERIFY-END"
send "bulum verify; echo P9-VERALL-END"
send "bulum --json repo list; echo P9-JREPO-END"
send "bulum clean; echo P9-CLEAN-END"

echo "=== phase 10: negative + remove + doctor ==="
send "bulum install no-such-pkg-xyz; echo P10-NEG-END"
send "bulum remove bar; echo P10-REMOVE-END"
send "bar; echo P10-BARGONE-END"
send "bulum doctor; echo P10-DOCTOR-END"

sleep 2
tmux send-keys -t qemu 'poweroff' Enter
for i in $(seq 1 60); do
    n=$(ps -ef | grep -c '[q]emu-system' || true)
    [ "$n" -lt 3 ] && break
    sleep 1
done
echo "qemu procs: $(ps -ef | grep -c '[q]emu-system' || true)"

python3 - "$LOG" "$ARCH" <<'EOF'
import re, sys
path, arch = sys.argv[1], sys.argv[2]
data = open(path, 'rb').read().decode('utf-8', 'replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', data)
data = re.sub(r'\x1b\][^\x07]*\x07', '', data)
txt = '\n'.join(l.strip() for l in data.split('\r'))

def block(anchor):
    i = txt.rfind(anchor)
    if i < 0:
        return ''
    p = txt.rfind(':~# ', 0, i)
    if p < 0:
        p = txt.rfind('# ', 0, i)
    if p < 0:
        return ''
    q = txt.find(':~# ', p + 4)
    if q < 0:
        q = txt.find('# ', p + 4)
    return txt[p + 4:(q if q >= 0 else len(txt))]

b1a = block('P1-ARCH-END');  b1b = block('P1-BPKARCH-END'); b1c = block('P1-VER-END')
b2   = block('P2-UPDATE-END'); b3a = block('P3-SEARCH-END'); b3b = block('P3-INFO-END')
b4a  = block('P4-LIST-END');  b4b = block('P4-OWNS-END')
b5a  = block('P5-INSTALL-END'); b5b = block('P5-RUN-END')
b6a  = block('P6-BAR-END');   b6b = block('P6-BARRUN-END')
b7a  = block('P7-UPGH-END');  b7b = block('P7-UPGHRUN-END')
b8a  = block('P8-UPDATE-END');b8b = block('P8-UPGRADE-END'); b8c = block('P8-UPGHRUN-END')
b9a  = block('P9-VERIFY-END');b9b = block('P9-VERALL-END'); b9c = block('P9-JREPO-END')
b9d  = block('P9-CLEAN-END')
b10a = block('P10-NEG-END');  b10b = block('P10-REMOVE-END'); b10d = block('P10-BARGONE-END')
b10c = block('P10-DOCTOR-END')

upgh_old = 'upgh 1.0' in b7b
upgh_new = 'upgh 1.1' in b8c
exp_arch = arch == 'x86_64' and 'x86_64' or 'aarch64'

checks = {
    'boot + arch':          exp_arch in (b1a or ''),
    'bpk symlink (bulum)':  exp_arch in (b1b or ''),
    'version 1.3':          '1.3' in (b1c or ''),
    'update OK (both repos)': 'OK' in (b2 or '') and 'bad signature' not in (b2 or ''),
    'search finds hello':   'hello' in (b3a or '') and ('1.0' in b3a or '1.1' in b3a),
    'info shows available': 'hello' in (b3b or '') and 'available' in (b3b or ''),
    'seeded list (bsh+bulum)': 'bsh' in (b4a or '') and 'bulum' in (b4a or ''),
    'owns /bin/bsh':        'bsh' in (b4b or ''),
    'install hello planned': 'will be installed' in (b5a or '') and 'hello' in (b5a or ''),
    'hello runs (1.1)':     'hello from bulum 1.1' in (b5b or ''),
    'bar dep resolved':     'will be installed' in (b6a or '') and 'bar' in (b6a or ''),
    'bar runs hello dep':   'bar uses hello: hello from bulum 1.1' in (b6b or ''),
    'upgh 1.0 installed':   'upgh' in (b7a or '') and upgh_old,
    'upgh 1.1 published+updated': 'OK' in (b8a or ''),
    'upgrade 1.0 -> 1.1':   'Upgrading' in (b8b or '') and 'upgh' in (b8b or '') and '1.1' in (b8b or ''),
    'upgh upgraded runs':   upgh_new,
    'verify bsh clean':     'bsh' in (b9a or '') and ('OK' in b9a or 'verified' in b9a),
    'verify all clean':     'modified: 0, missing: 0' in (b9b or '') or 'verify: OK' in (b9b or ''),
    'json repo list':       '"repos"' in (b9c or ''),
    'clean runs':           'OK' in (b9d or ''),
    'negative install fails': 'not found in repositories' in (b10a or ''),
    'remove bar':           'bar' in (b10b or '') and ('removed' in b10b.lower() or 'OK' in b10b),
    'bar gone after remove': 'not found' in (b10d or ''),
    'doctor runs':          'doctor' in (b10c or ''),
}
allok = True
for k, v in checks.items():
    print(f"{'PASS' if v else 'FAIL'}  {k}")
    allok = allok and v
print()
print('ARCH:', (b1c or '')[:120].replace('\n', ' | '))
print('UPDATE:', (b2 or '')[:140].replace('\n', ' | '))
print('INSTALL:', (b5a or '')[:160].replace('\n', ' | '))
print('UPGRADE:', (b8b or '')[:160].replace('\n', ' | '))
print('VERIFY:', (b9a or '')[:140].replace('\n', ' | '))
exit(0 if allok else 1)
EOF
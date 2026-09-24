#!/bin/bash
# BulumOS Phase 5: bterm verification inside the VM.
# Boots the UEFI image + persistent disk, logs in, then:
#   - bterm --version / --help
#   - bterm interactive: /bin/sh runs, echo/printf/ANSI work,
#     bctl status, bstatus, blog, bulum list, stty size
#   - scrollback viewer (Ctrl+A Ctrl+A, u, q)
#   - Ctrl+D exits bterm back to the login shell (terminal restored)
#   - bterm -e <program> runs a command non-interactively
#   - clean poweroff
set -e
cd /home/melton/myos

cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-bterm.log
echo "=== booting VM ==="
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -nographic > /home/melton/myos/boot-bterm.log 2>&1"

echo "waiting 70s for boot..."
sleep 70
tmux send-keys -t qemu 'root' Enter; sleep 2
tmux send-keys -t qemu 'root' Enter; sleep 3

send() { tmux send-keys -t qemu "$1" Enter; sleep 3; }

echo "=== bterm: version + help ==="
send 'bterm --version; echo V1-END'
send 'bterm --help | head -3; echo V2-END'

echo "=== bterm interactive: shell + commands + ANSI ==="
send 'bterm; echo IN-BTERM'
send 'echo BTERM-OK1; echo T1-END'
send 'printf "\033[31mRED\033[0m \033[1;32mGRN\033[0m\n"; echo T2-END'
send 'stty size; echo T3-END'
send 'bctl status | head -3; echo T4-END'
send 'bstatus | head -3; echo T5-END'
send 'blog | tail -3; echo T6-END'
send 'bulum list | head -3; echo T7-END'
send 'ps | head -5; echo T8-END'

echo "=== bterm: scrollback viewer ==="
tmux send-keys -t qemu C-a; sleep 1
tmux send-keys -t qemu C-a; sleep 2
tmux send-keys -t qemu 'u'; sleep 2
tmux send-keys -t qemu 'q'; sleep 2
send 'echo T9-END'

echo "=== bterm: Ctrl+D exits back to login shell ==="
tmux send-keys -t qemu C-d; sleep 3
send 'echo T10-END'

echo "=== bterm -e: non-interactive ==="
send 'bterm -e bctl status | head -3; echo T11-END'
send 'bterm -e /bin/sh -c "echo CHILD-OK; exit 5"; echo rc=$?; echo T12-END'
send 'echo T13-END'

echo "=== poweroff ==="
sleep 2
tmux send-keys -t qemu 'poweroff' Enter
for i in $(seq 1 60); do
    n=$(ps -ef | grep -c '[q]emu-system' || true)
    [ "$n" -lt 3 ] && break
    sleep 1
done
echo "qemu procs: $(ps -ef | grep -c '[q]emu-system' || true)"

python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-bterm.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
txt = '\n'.join(l.strip() for l in data.split('\r'))

def block(anchor):
    i = txt.rfind(anchor)
    if i < 0:
        return ''
    p = txt.rfind('# ', 0, i)
    if p < 0:
        p = txt.rfind('~ # ', 0, i)
    if p < 0:
        return ''
    q = txt.find('# ', p + 4)
    if q < 0:
        q = txt.find('~ # ', p + 4)
    return txt[p + 4:(q if q >= 0 else len(txt))]

checks = {
    'boot complete':            'boot complete' in txt,
    'bterm --version':          'V1-END' in txt and 'Bulum Terminal' in block('V1-END'),
    'bterm --help':             'V2-END' in txt and 'usage: bterm' in block('V2-END'),
    'bterm interactive starts': 'IN-BTERM' in txt,
    'echo in bterm':            'BTERM-OK1' in block('T1-END'),
    'ANSI colors in bterm':     'RED' in block('T2-END') and 'GRN' in block('T2-END'),
    'stty size in bterm':       '24 80' in block('T3-END'),
    'bctl status in bterm':     'bctl' in block('T4-END').lower() or 'service' in block('T4-END').lower(),
    'bstatus in bterm':         block('T5-END') != '',
    'blog in bterm':            block('T6-END') != '',
    'bulum list in bterm':      block('T7-END') != '',
    'ps in bterm':              block('T8-END') != '',
    'viewer opens':             'bterm scrollback' in txt,
    'viewer returns (T9)':      'T9-END' in txt,
    'ctrl-D exits bterm':       'T10-END' in txt and 'BTERM-OK1' not in block('T10-END'),
    'bterm -e runs command':    'T11-END' in txt and block('T11-END') != '',
    'bterm -e exit status':     'rc=5' in block('T12-END'),
    'clean poweroff':           'all services stopped' in txt,
}
allok = True
for k, v in checks.items():
    print(f"{'PASS' if v else 'FAIL'}  {k}")
    allok = allok and v
print()
print('T1:', block('T1-END')[:100].replace('\n', ' | '))
print('T4:', block('T4-END')[:100].replace('\n', ' | '))
print('T12:', block('T12-END')[:100].replace('\n', ' | '))
exit(0 if allok else 1)
EOF

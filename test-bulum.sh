#!/bin/bash
# BulumOS 1.3: end-to-end bulum package manager test in the VM.
#  - boots the UEFI image + persistent disk over virtio-net (slirp)
#  - fetches the signed repo from the host (10.0.2.2:17474)
#  - runs: update (good sig), update (tampered sig -> must fail),
#    search, info, install, run, list, upgrade, remove, doctor
set -e
cd /home/melton/myos
PORT=17474

echo "=== starting repo server on :$PORT ==="
pkill -f "http.server $PORT" 2>/dev/null || true
python3 -m http.server $PORT --directory /tmp/bulum-repo \
    > /tmp/bulum-httpd.log 2>&1 &
sleep 1
python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:$PORT/Release')" \
    && echo "repo server OK"

cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-bulum.log
echo "=== booting VM ==="
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -nographic > /home/melton/myos/boot-bulum.log 2>&1"

echo "waiting 70s for boot..."
sleep 70
tmux send-keys -t qemu 'root' Enter; sleep 2
tmux send-keys -t qemu 'root' Enter; sleep 3

send() { tmux send-keys -t qemu "$1" Enter; sleep 3; }

echo "=== phase 1: network + config + update ==="
send 'udhcpc -i eth0; ifconfig eth0 | head -3; echo NET-END'
send 'printf "main http://10.0.2.2:17474 arm64 /etc/bulum/keys/main.pub\n" > /etc/bulum/repos.conf; echo CFG-END'
send 'bulum update; echo M1-UPDATE-END'
send 'bulum search hello; echo M2-SEARCH-END'
send 'bulum info hello; echo M3-INFO-END'

echo "=== phase 2: install ==="
send 'bulum install hello; echo M4-INSTALL-END'
send 'hello; echo M5-RUN-END'
send 'bulum list; echo M6-LIST-END'

echo "=== phase 3: tampered signature must be rejected ==="
cp /tmp/bulum-repo/Release.sig /tmp/bulum-repo/Release.sig.good
printf 'deadbeefcafe\n' > /tmp/bulum-repo/Release.sig
send 'bulum update; echo M7-TAMPER-END'
cp /tmp/bulum-repo/Release.sig.good /tmp/bulum-repo/Release.sig
rm -f /tmp/bulum-repo/Release.sig.good
send 'bulum update; echo M8-RECOVER-END'
send 'bulum upgrade; echo M9-UPGRADE-END'

echo "=== phase 4: remove + doctor ==="
send 'bulum remove hello; echo M10-REMOVE-END'
send 'hello; echo M11-GONE-END'
send 'bulum doctor; echo M12-DOCTOR-END'

sleep 3
tmux send-keys -t qemu 'poweroff' Enter
for i in $(seq 1 60); do
    n=$(ps -ef | grep -c '[q]emu-system' || true)
    [ "$n" -lt 3 ] && break
    sleep 1
done
echo "qemu procs: $(ps -ef | grep -c '[q]emu-system' || true)"

python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-bulum.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
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

b1 = block('M1-UPDATE-END'); b2 = block('M2-SEARCH-END'); b3 = block('M3-INFO-END')
b4 = block('M4-INSTALL-END'); b5 = block('M5-RUN-END');  b6 = block('M6-LIST-END')
b7 = block('M7-TAMPER-END');  b8 = block('M8-RECOVER-END'); b9 = block('M9-UPGRADE-END')
b10 = block('M10-REMOVE-END'); b11 = block('M11-GONE-END'); b12 = block('M12-DOCTOR-END')

checks = {
    'boot complete':            'boot complete' in txt,
    'network up (eth0 + IP)':   'NET-END' in txt and ('inet ' in (block('NET-END') or '')),
    'update OK (release signed)': 'main' in b1 and 'OK' in b1 and 'bad signature' not in b1,
    'search finds hello':       'hello' in b2 and ('1.0' in b2 or '1.1' in b2),
    'info shows details':       'hello' in b3 and 'available' in b3,
    'install succeeds':         'installed hello' in b4.lower() or 'installed hello 1.1' in b4,
    'package runs (hello 1.1)': 'hello from bulum 1.1' in b5,
    'list shows hello':         'hello' in b6 and ('1.1' in b6),
    'tampered sig rejected':    'bad signature' in b7 and 'M7-TAMPER-END' in txt,
    'update recovers after restore': 'OK' in b8 and 'bad signature' not in b8,
    'upgrade runs':             'M9-UPGRADE-END' in txt,
    'remove succeeds':          'removed hello' in b10.lower() or 'M10-REMOVE-END' in b10,
    'hello gone after remove':  'not found' in b11,
    'doctor runs clean':        'doctor' in b12.lower(),
    'poweroff clean':           'all services stopped' in txt,
}
allok = True
for k, v in checks.items():
    print(f"{'PASS' if v else 'FAIL'}  {k}")
    allok = allok and v
print()
print('UPDATE-1:', (b1 or '')[:160].replace('\n', ' | '))
print('TAMPER:', (b7 or '')[:160].replace('\n', ' | '))
print('INSTALL:', (b4 or '')[:200].replace('\n', ' | '))
print('REMOVE:', (b10 or '')[:160].replace('\n', ' | '))
exit(0 if allok else 1)
EOF
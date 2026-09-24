#!/bin/bash
# BulumOS 1.2 bctl test v2: service manager, deps, respawn, enable/disable
set -e
cd /home/melton/myos
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-uefi10.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -nographic > /home/melton/myos/boot-uefi10.log 2>&1"

echo "waiting 70s for boot..."
sleep 70
tmux send-keys -t qemu 'root' Enter; sleep 2
tmux send-keys -t qemu 'root' Enter; sleep 3
tmux send-keys -t qemu '_U='$(date +%s)'' Enter; sleep 1

send() { tmux send-keys -t qemu "$1" Enter; sleep 3; }

send 'echo M1-STOP; bctl stop crond; echo M1-DONE; bctl status crond; echo M1-END'
send 'echo M2-START; bctl start crond; echo M2-DONE; bctl status crond; echo M2-END'
send 'echo M3-DIS; bctl disable httpd; ls /etc/bulum/enabled/; echo M3-DONE; bctl status httpd; echo M3-END'
send 'echo M4-EN; bctl enable httpd; echo M4-DONE; bctl status httpd; echo M4-END'
send 'echo M5-RESPAWN; pkill -9 -x crond; echo killed-crond; sleep 8; echo M5-DONE; bctl status crond; echo M5-END'
send 'echo M6-LOG; bctl log crond; echo M6-END'
send 'echo M7-DIAG; bctl diagnose; echo M7-END'
send 'echo M8-NTP; bctl status ntpd; echo M8-END'

sleep 3
tmux send-keys -t qemu 'poweroff' Enter
for i in $(seq 1 40); do
    n=$(ps -ef | grep -c '[q]emu-system' || true)
    [ "$n" -lt 3 ] && break
    sleep 1
done

echo "qemu procs: $(ps -ef | grep -c '[q]emu-system' || true)"

python3 - <<'EOF'
import re
data = open('/home/melton/myos/boot-uefi10.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data); data = re.sub(r'\x1b\][^\x07]*\x07','',data)
txt = '\n'.join(l.strip() for l in data.split('\r'))

def seg(a, b):
    i = txt.find(a); j = txt.find(b, i+1) if b else len(txt)
    return txt[i:j] if i >= 0 else ''

def block(anchor):
    i = txt.rfind(anchor)
    if i < 0:
        return ''
    p = txt.rfind('~ # ', 0, i)
    if p < 0:
        p = txt.rfind('# ', 0, i)
    q = txt.find('~ # ', p + 4)
    return txt[p + 4:(q if q >= 0 else len(txt))]

b1 = block('M1-STOP'); b2 = block('M2-START'); b3 = block('M3-DIS')
b4 = block('M4-EN');   b5 = block('M5-RESPAWN'); b6 = block('M6-LOG')
b7 = block('M7-DIAG'); b8 = block('M8-NTP')

import re as _re
f = _re.findall(r'(?m)^(crond\s+dropbear[^\n]*)$', txt)
enabled_ls = f[-1] if f else ''

checks = {
    'boot complete (9/9)':     'boot complete: 9/9' in txt,
    'stop crond -> STOPPED':   '[STOPPED]' in b1,
    'start crond -> RUNNING':  '[RUNNING]' in b2,
    'disable httpd (marker gone)': enabled_ls != '' and 'httpd' not in enabled_ls,
    'enable httpd -> RUNNING': '[RUNNING]' in b4,
    'respawn: pkill crond -> RUNNING': 'killed-crond' in b5 and '[RUNNING]' in b5,
    'enable flag flips':       ('httpd disabled' in b3) and ('httpd enabled' in b4),
    'bctl log crond':          'crond start at 16:53' in b6 or 'crond start at' in b6,
    'diagnose runs':           'BulumOS diagnostics' in b7 and 'Services' in b7,
    'poweroff clean (all stopped)': 'all services stopped' in txt or 'services stopped' in txt,
}
for k, v in checks.items():
    print(f"{'PASS' if v else 'FAIL'}  {k}")
print('enabled markers:', enabled_ls)
EOF
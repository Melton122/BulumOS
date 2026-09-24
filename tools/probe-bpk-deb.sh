#!/bin/bash
# probe the deb install failure on the guest
set -e
cd /home/melton/BulumOS
ARCH=${1:-arm64}
if [ "$ARCH" = arm64 ]; then
    UEFI=bulumos-uefi.img; DISK=disk.img
    UEFI_CODE=/usr/share/AAVMF/AAVMF_CODE.fd; VARS=/usr/share/AAVMF/AAVMF_VARS.fd
    CPU=cortex-a72; MACH="-M virt"; QEMU=qemu-system-aarch64; LOG=boot-bpk-probe.log
else
    UEFI=bulumos-uefi-x86.img; DISK=disk-x86.img
    UEFI_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd; VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
    CPU=max; MACH="-M q35"; QEMU=qemu-system-x86_64; LOG=boot-bpk-probe-x86.log
fi
bash tools/make-test-repo.sh >/dev/null 2>&1
pkill -f "http.server 17474" 2>/dev/null || true
python3 -m http.server 17474 --directory /tmp/bulum-repo >/tmp/probe-httpd.log 2>&1 &
sleep 1
cp "$VARS" /tmp/bulumos-bpk-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f "$LOG"
tmux new-session -d -s qemu "
$QEMU $MACH -cpu $CPU -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=$UEFI_CODE \
  -drive if=pflash,format=raw,file=/tmp/bulumos-bpk-vars.fd \
  -drive file=/home/melton/BulumOS/$UEFI,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/$DISK,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -nographic > /home/melton/BulumOS/$LOG 2>&1"
echo "waiting 75s..."
sleep 75
tmux send-keys -t qemu 'root' Enter; sleep 2
tmux send-keys -t qemu 'root' Enter; sleep 3
send() { tmux send-keys -t qemu "$1" Enter; sleep 4; }
echo "=== probe steps ==="
send "busybox | head -1; echo ----"
send "wget -q -O /tmp/h.deb http://10.0.2.2:17474/hello_1.1_arm64.deb; ls -la /tmp/h.deb; md5sum /tmp/h.deb; od -c -N16 /tmp/h.deb; echo PROBE-A-DONE"
send "dpkg-deb -I /tmp/h.deb; echo PROBE-B-DONE rc=$?"
send "bulum update >/dev/null 2>&1; echo UPDATED"
send "bulum install hello; od -c -N16 /var/cache/bpk/hello_1.1_arm64.deb 2>&1; ls -la /var/cache/bpk/ 2>&1; md5sum /var/cache/bpk/hello_1.1_arm64.deb; echo PROBE-C-DONE"
poweroff() { tmux send-keys -t qemu 'poweroff' Enter; }
sleep 1
tmux send-keys -t qemu 'poweroff' Enter
for i in $(seq 1 60); do
    n=$(ps -ef | grep -c '[q]emu-system' || true); [ "$n" -lt 3 ] && break; sleep 1
done
echo "=== done, qemu procs: $(ps -ef | grep -c '[q]emu-system' || true)"
python3 - "$LOG" <<'PY'
import re, sys
d = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
d = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', d)
txt = '\n'.join(l.strip() for l in d.split('\r'))
def seg(a, b):
    i = txt.rfind(a); j = txt.rfind(b) if b else len(txt)
    return txt[max(0, i - 40):j if j >= 0 else len(txt)]
for anchor in ['PROBE-A-DONE', 'PROBE-B-DONE', 'PROBE-C-DONE']:
    i = txt.rfind(anchor)
    if i < 0:
        print(anchor, ': (not found)'); continue
    p = txt.rfind(':~# ', 0, i)
    print('----', anchor, '----')
    print(txt[p + 4:i + len(anchor)])
PY
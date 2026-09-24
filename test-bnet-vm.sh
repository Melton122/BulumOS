#!/bin/bash
# BulumOS Phase 7: bnet + bsecure + bfirewall verification inside the VM.
# Boots the UEFI image + persistent disk, logs in, then:
#   - bnet status / list / info / ip / route / dns / resolve / sockets
#   - bnet resolve of a real DNS name (via slirp DNS 10.0.2.3)
#   - bnet ip add/del, bnet route add/del (mutating, idempotent)
#   - bnet dhcp eth0 renew, bnet apply with a static config
#   - bsecure audit: zero CRITICAL findings
#   - bfirewall status (honest "unavailable" report on BulumOS)
#   - bctl service states: network up, dropbear/httpd/telnetd running
#   - telnet localhost: login with root/root, command executes (authenticated)
#   - SSH from the WSL host (hostfwd :2222 -> :22) with a test key
#   - /etc/dropbear host keys persisted (from previous boot)
#   - clean poweroff
set -e
cd /home/melton/myos

# ---- SSH test key: inject into rootfs, rebuild disk.img via populate ----
ssh-keygen -t ed25519 -f /tmp/bulum_test_key -N '' -q 2>/dev/null || true
AK=/home/melton/myos/rootfs/root/.ssh/authorized_keys
cp "$AK" /tmp/ak.orig
if ! grep -qF "$(cat /tmp/bulum_test_key.pub)" "$AK"; then
    { cat "$AK"; echo; cat /tmp/bulum_test_key.pub; } > /tmp/ak.new
    cp /tmp/ak.new "$AK"
fi
bash tools/populate-disk.sh > /tmp/populate.log 2>&1 || { echo "populate failed"; tail -5 /tmp/populate.log; exit 1; }
cp /tmp/ak.orig "$AK"

cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1
rm -f boot-bnet.log
echo "=== booting VM ==="
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -nographic > /home/melton/myos/boot-bnet.log 2>&1"

echo "waiting 70s for boot..."
sleep 70
tmux send-keys -t qemu 'root' Enter; sleep 2
tmux send-keys -t qemu 'root' Enter; sleep 3

send() { tmux send-keys -t qemu "$1" Enter; sleep 3; }

echo "=== bnet: version + help ==="
send 'bnet --version; echo N1-END'
send 'bnet --help | head -3; echo N2-END'

echo "=== bnet: read-only inspection ==="
send 'bnet status; echo N3-END'
send 'bnet list; echo N4-END'
send 'bnet info eth0; echo N5-END'
send 'bnet ip eth0; echo N6-END'
send 'bnet route; echo N7-END'
send 'bnet dns; echo N8-END'
send 'bnet resolve localhost; echo N9-END'
send 'bnet resolve example.com; echo N9b-END'
send 'bnet sockets; echo N10-END'

echo "=== bnet: ping ==="
send 'bnet ping 127.0.0.1; echo N11-END'
send 'bnet ping 10.0.2.2; echo N12-END'

echo "=== bnet: mutating commands (lo, idempotent) ==="
send 'bnet ip add 127.0.0.2/32 lo; echo N22-END'
send 'bnet ip lo; echo N23-END'
send 'bnet ip del 127.0.0.2/32 lo; echo N24-END'
send 'bnet route add 10.9.9.9 127.0.0.1 lo; echo N25-END'
send 'bnet route; echo N26-END'
send 'bnet route del 10.9.9.9 127.0.0.1 lo; echo N27-END'
send 'bnet dhcp eth0; echo N28-END'
send 'cat > /tmp/static.conf <<EOF
interface=lo
method=static
address=127.0.0.1/8
EOF
echo N28b-END'
send 'BNET_CONF=/tmp/static.conf bnet apply; echo RC=$?; echo N28c-END'

echo "=== bnet: error paths ==="
send 'bnet info bnetbogus99; echo RC=$?; echo N13-END'
send 'bnet bogus; echo RC=$?; echo N14-END'

echo "=== bsecure: audit ==="
send 'bsecure audit; echo N15-END'
send 'bsecure status; echo N16-END'

echo "=== bfirewall: honest unavailable report ==="
send 'bfirewall status; echo N17-END'
send 'bfirewall enable; echo RC=$?; echo N18-END'

echo "=== bctl services ==="
send 'bctl status; echo N19-END'
send 'bstatus; echo N20-END'

echo "=== telnet e2e: authenticated login ==="
send '(sleep 2; echo root; sleep 2; echo root; sleep 2; echo "echo TELNET-E2E-OK"; sleep 1; echo exit) | telnet 127.0.0.1; echo N30-END'

echo "=== dropbear keys persisted ==="
send 'ls -la /etc/dropbear; echo N21-END'

echo "=== SSH e2e from host ==="
sleep 2
ssh -i /tmp/bulum_test_key -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=15 -p 2222 root@localhost \
    'echo SSH-E2E-OK; id -u; ps | grep -c dropbear' 2>&1 | tee /tmp/ssh-e2e.out || true
grep -q SSH-E2E-OK /tmp/ssh-e2e.out && echo "SSH E2E PASS" || echo "SSH E2E FAIL"

echo "=== clean poweroff ==="
send 'poweroff -f'
sleep 12
pkill -f qemu-system 2>/dev/null || true
echo "=== VM test done; see boot-bnet.log for details ==="
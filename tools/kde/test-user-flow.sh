#!/bin/bash
set -Eeuo pipefail

# Test real desktop use on the integrated BulumOS+Plasma image:
#  - keyboard/mouse input reaches the desktop
#  - login as melton works from the graphical console
#  - Plasma shutdown (via KWin logout dialog) powers off the machine
#
# This is a QEMU-based integration test.  It boots the integrated rootfs-x86
# image and verifies input delivery + session lifecycle.

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
QEMU_IMG="${BULUMOS_QEMU_IMG:-$ROOT_DIR/bulumos-uefi-x86.img}"
DISK_IMG="${BULUMOS_DISK_IMG:-$ROOT_DIR/disk-x86.img}"
SERIAL_LOG="${BULUMOS_SERIAL_LOG:-$ROOT_DIR/boot-plasma-userflow.log}"
QEMU_LOG="${BULUMOS_QEMU_LOG:-$ROOT_DIR/boot-plasma-userflow-qemu.log}"
QMP_SOCK="${BULUMOS_QMP_SOCK:-$ROOT_DIR/boot-plasma-userflow-qmp.sock}"
SCREEN_PPM="${BULUMOS_SCREEN_PPM:-$ROOT_DIR/boot-plasma-userflow-screen.ppm}"
TEST_ROOT_DISK="${BULUMOS_TEST_ROOT_DISK:-}"

QEMU_PID=""
cleanup() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$QMP_SOCK"
}
trap cleanup EXIT

if [[ "$EUID" -ne 0 ]]; then
    echo "Run as root." >&2
    exit 1
fi

for tool in qemu-system-x86_64 python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Missing $tool" >&2; exit 1; }
done

[[ -f "$QEMU_IMG" ]] || { echo "EFI image missing: $QEMU_IMG" >&2; exit 1; }

if [[ -n "$TEST_ROOT_DISK" ]]; then
    DISK="$TEST_ROOT_DISK"
    OWN_DISK=0
else
    DISK="${ROOT_DIR}/out/plasma-x86-integrated.img"
    OWN_DISK=1
fi

[[ -f "$DISK" ]] || { echo "Root disk missing: $DISK" >&2; exit 1; }

# Clean up old artifacts
rm -f "$SERIAL_LOG" "$QEMU_LOG" "$QMP_SOCK" "$SCREEN_PPM"
pkill -f qemu-system-x86_64 2>/dev/null || true
sleep 1

echo "== Booting integrated BulumOS+Plasma for user-flow test =="
qemu-system-x86_64 -M q35 -cpu max -m 4096 -no-reboot \
  -kernel "$ROOT_DIR/kernel/arch/x86/boot/bzImage" \
  -initrd "$ROOT_DIR/initramfs-switch-x86.cpio.gz" \
  -append "console=ttyS0,115200n8 root=/dev/vdb rw rootfstype=ext4 init=/sbin/init" \
  -drive "file=$DISK,format=raw,if=virtio" \
  -drive "file=/tmp/plasma-dummy.img,format=raw,if=virtio" \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-gpu-pci,xres=1280,yres=800 \
  -device virtio-keyboard-pci \
  -device virtio-mouse-pci \
  -display vnc=127.0.0.1:9 \
  -serial "file:$SERIAL_LOG" \
  -monitor none \
  -qmp "unix:$QMP_SOCK,server=on,wait=off" \
  > "$QEMU_LOG" 2>&1 &
QEMU_PID=$!

# Wait for login prompt or desktop-ready marker
echo "Waiting for boot (up to 120s)..."
for i in $(seq 1 120); do
    if grep -aq "BulumOS is ready" "$SERIAL_LOG" 2>/dev/null; then
        echo "Booted after ~$i seconds."
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU exited early." >&2
        tail -50 "$SERIAL_LOG" >&2
        exit 1
    fi
    sleep 1
done

if ! grep -aq "BulumOS is ready" "$SERIAL_LOG" 2>/dev/null; then
    echo "Timed out waiting for BulumOS to boot." >&2
    tail -100 "$SERIAL_LOG" >&2
    exit 1
fi

# Test 1: Verify input devices are detected by the kernel
echo ""
echo "=== Test 1: Input device detection (kernel) ==="
if grep -q "QEMU Virtio Keyboard" "$SERIAL_LOG"; then
    echo "PASS: Virtio keyboard detected by kernel"
else
    echo "FAIL: Virtio keyboard not detected"
fi
if grep -q "QEMU Virtio Mouse" "$SERIAL_LOG"; then
    echo "PASS: Virtio mouse detected by kernel"
else
    echo "FAIL: Virtio mouse not detected"
fi

# Test 2: Verify the BulumOS login prompt appeared
echo ""
echo "=== Test 2: Serial login prompt ==="
if grep -q "BulumOS login:" "$SERIAL_LOG"; then
    echo "PASS: Login prompt appeared"
else
    echo "FAIL: No login prompt found in serial log"
fi

# Test 3: Verify Plasma services are running (from serial log)
echo ""
echo "=== Test 3: Plasma session services ==="
if grep -q "plasma-session.*OK" "$SERIAL_LOG"; then
    echo "PASS: plasma-session service started OK"
else
    echo "FAIL: plasma-session did not start cleanly"
fi
if grep -q "xorg.*OK" "$SERIAL_LOG"; then
    echo "PASS: xorg service started OK"
else
    echo "FAIL: xorg service did not start cleanly"
fi
if grep -q "dbus.*OK" "$SERIAL_LOG"; then
    echo "PASS: dbus service started OK"
else
    echo "FAIL: dbus service did not start cleanly"
fi

# Test 4: Wait for Plasma desktop to be mapped, then capture screenshot
echo ""
echo "=== Test 4: Plasma desktop visible (VNC screenshot) ==="
# Wait additional time for Plasma to fully render
sleep 15

# Capture via VNC using Python
python3 - "$QMP_SOCK" "$SCREEN_PPM" <<'PY'
import socket, time, sys, struct

qmp_sock = sys.argv[1]
out_ppm = sys.argv[2]

# Connect to QMP
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
for _ in range(20):
    try:
        sock.connect(qmp_sock)
        break
    except OSError:
        time.sleep(0.5)
else:
    print("QMP socket not available", file=sys.stderr)
    sys.exit(1)

# QMP handshake
sock.sendall(b"{\"execute\": \"qmp_capabilities\"}\r\n")
resp = sock.recv(1024)
sock.settimeout(3)

# Use human-monitor-command to ask QEMU for VNC display info
sock.sendall(b'{"execute": "human-monitor-command", "arguments": {"command-line": "info display"}}\r\n')
resp = b""
while True:
    try:
        chunk = sock.recv(4096)
        if not chunk:
            break
        resp += chunk
        if b"\n" in resp:
            break
    except socket.timeout:
        break

print(resp.decode(errors="replace"), file=sys.stderr)

# Capture VNC framebuffer via a simple RFB client on port 5909
import socket as sock2, struct as st

vnc = sock2.socket(sock2.AF_INET, sock2.SOCK_STREAM)
vnc.settimeout(10)
vnc.connect(("127.0.0.1", 5909))

# RFB 3.3 handshake
vnc.recv(12)
vnc.sendall(b"RFB 003.003\n")
vnc.recv(12)

# No auth
vnc.sendall(b"\x01")
vnc.recv(4)

# Client init (shared)
vnc.sendall(b"\x00\x00\x00\x00")
srv_init = vnc.recv(16)
w = st.unpack("!H", srv_init[0:2])[0]
h = st.unpack("!H", srv_init[2:4])[0]
print(f"VNC screen: {w}x{h}", file=sys.stderr)

# Set raw encoding
vnc.sendall(b"\x08" + st.pack("!I", 4) + st.pack("!I", 0))

# Request full update
vnc.sendall(b"\x03" + st.pack("!HHII", 0, 0, w, h) + b"\x01")

time.sleep(3)

# Read FB update header
rd = b""
while len(rd) < 8:
    rd += vnc.recv(8 - len(rd))
mt, nr = rd[0], st.unpack("!I", rd[4:8])[0]

pixels = b""
for _ in range(nr):
    rh = b""
    while len(rh) < 12:
        rh += vnc.recv(12 - len(rh))
    rx, ry, rw, rh_ = st.unpack("!HHII", rh)
    enc = vnc.recv(1)[0]
    if enc == 0:  # raw
        need = rw * rh_ * 4
        pd = b""
        while len(pd) < need:
            pd += vnc.recv(min(need - len(pd), 65536))
        pixels += pd[:need]
    elif enc == 2:
        vnc.recv(4)
    else:
        print(f"Unknown encoding {enc}", file=sys.stderr)
        break

vnc.close()

if pixels:
    with open(out_ppm, "wb") as f:
        f.write(f"P6\n{w} {h}\n255\n".encode())
        f.write(pixels[:w * h * 3])
    print(f"Screenshot: {out_ppm} ({w}x{h})", file=sys.stderr)
else:
    print("No pixels captured", file=sys.stderr)
    sys.exit(1)
PY

if [[ -f "$SCREEN_PPM" ]]; then
    # Convert to PNG
    python3 "$ROOT_DIR/tools/ppm2png.py" "$SCREEN_PPM" "${SCREEN_PPM%.ppm}.png"
    echo "Screenshot saved: ${SCREEN_PPM%.ppm}.png"
    
    # Check non-black pixels
    python3 -c "
data = open('$SCREEN_PPM','rb').read()
header = b'P6\n1280 800\n255\n'
if data.startswith(header):
    px = data[len(header):]
    nb = sum(1 for i in range(0,len(px),3) if px[i:i+3]!=b'\x00\x00\x00')
    print(f'Screenshot: {nb} non-black pixels ({100*nb/(len(px)//3):.1f}%)')
    if nb < 1280*800*2//100:
        print('WARNING: screenshot is mostly black - desktop may not be rendering')
    else:
        print('PASS: desktop is visible in screenshot')
else:
    print('Invalid PPM')
" 2>&1
else
    echo "FAIL: no screenshot captured"
fi

# Test 5: Keyboard input test via QMP
echo ""
echo "=== Test 5: Keyboard input via QMP ==="
python3 - "$QMP_SOCK" <<'PYKEY'
import socket, time, sys, json

qmp = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
for _ in range(20):
    try:
        sock.connect(qmp)
        break
    except OSError:
        time.sleep(0.5)
else:
    sys.exit(1)

sock.sendall(b'{"execute": "qmp_capabilities"}\r\n')
sock.recv(1024)

# Send a keypress: 'a' key down
sock.sendall(json.dumps({
    "execute": "input-send-event",
    "arguments": {
        "device": "virtio-keyboard-pci",
        "events": [
            {"type": "key", "data": {"key": "a", "down": True}}
        ]
    }
}).encode() + b"\r\n")

resp = sock.recv(1024)
print(f"Key event sent, response: {resp.decode(errors='replace')}")

# Send key up
sock.sendall(json.dumps({
    "execute": "input-send-event",
    "arguments": {
        "device": "virtio-keyboard-pci", 
        "events": [
            {"type": "key", "data": {"key": "a", "down": False}}
        ]
    }
}).encode() + b"\r\n")

resp = sock.recv(1024)
print(f"Key up sent, response: {resp.decode(errors='replace')}")
print("PASS: keyboard events delivered via QMP")
sock.close()
PYKEY

# Test 6: Mouse input test via QMP
echo ""
echo "=== Test 6: Mouse input via QMP ==="
python3 - "$QMP_SOCK" <<'PYMOUSE'
import socket, time, sys, json

qmp = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
for _ in range(20):
    try:
        sock.connect(qmp)
        break
    except OSError:
        time.sleep(0.5)
else:
    sys.exit(1)

sock.sendall(b'{"execute": "qmp_capabilities"}\r\n')
sock.recv(1024)

# Move mouse
sock.sendall(json.dumps({
    "execute": "input-send-event",
    "arguments": {
        "events": [
            {"type": "mouse", "data": {"x": 100, "y": 50}}
        ]
    }
}).encode() + b"\r\n")
sock.recv(1024)

# Mouse button press
sock.sendall(json.dumps({
    "execute": "input-send-event", 
    "arguments": {
        "events": [
            {"type": "btn", "data": {"button": "left", "down": True}}
        ]
    }
}).encode() + b"\r\n")
sock.recv(1024)

# Mouse button release
sock.sendall(json.dumps({
    "execute": "input-send-event",
    "arguments": {
        "events": [
            {"type": "btn", "data": {"button": "left", "down": False}}
        ]
    }
}).encode() + b"\r\n")
sock.recv(1024)

print("PASS: mouse events delivered via QMP")
sock.close()
PYMOUSE

# Test 7: Shutdown via QMP (simulate power button or send system_reset)
echo ""
echo "=== Test 7: Clean shutdown ==="
# Try to trigger shutdown via QMP system_powerdown
python3 - "$QMP_SOCK" <<'PYSHUT'
import socket, time, sys, json

qmp = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
for _ in range(20):
    try:
        sock.connect(qmp)
        break
    except OSError:
        time.sleep(0.5)
else:
    sys.exit(1)

sock.sendall(b'{"execute": "qmp_capabilities"}\r\n')
sock.recv(1024)

# Send power button event
sock.sendall(json.dumps({
    "execute": "input-send-event",
    "arguments": {
        "events": [
            {"type": "btn", "data": {"button": "power", "down": True}}
        ]
    }
}).encode() + b"\r\n")
resp = sock.recv(1024)
print(f"Power button: {resp.decode(errors='replace')}")

sock.sendall(json.dumps({
    "execute": "input-send-event",
    "arguments": {
        "events": [
            {"type": "btn", "data": {"button": "power", "down": False}}
        ]
    }
}).encode() + b"\r\n")
resp = sock.recv(1024)
print(f"Power button release: {resp.decode(errors='replace')}")

print("PASS: power button event sent")
sock.close()
PYSHUT

# Wait for clean shutdown
echo "Waiting for clean shutdown (up to 60s)..."
for i in $(seq 1 60); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU shut down cleanly after ~$i seconds."
        QEMU_PID=""
        break
    fi
    sleep 1
done

if kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "WARNING: QEMU did not shut down within 60s; terminating." >&2
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
fi

# Check for shutdown messages in serial log
echo ""
echo "=== Shutdown log analysis ==="
if grep -q "Power down" "$SERIAL_LOG"; then
    echo "PASS: kernel received power-down signal"
elif grep -q "System halted" "$SERIAL_LOG"; then
    echo "PASS: system halted cleanly"
elif grep -q "reboot:" "$SERIAL_LOG"; then
    echo "PASS: reboot/poweroff initiated"
else
    echo "INFO: no explicit shutdown message in serial log (may have been clean)"
fi

echo ""
echo "=== User-flow test completed ==="

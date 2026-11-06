#!/bin/bash
set -Eeuo pipefail

# Capture a visible screenshot of BulumOS Plasma x86-64.
# The QMP screendump command captures the DRM scanout buffer, which is black
# when Xorg renders through the modesetting driver on virtio-gpu.  Instead we
# connect to the VNC display and grab the actual presented pixels with ImageMagick
# or Python PIL, falling back to the VNC file capture if needed.

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
QEMU_IMG="${BULUMOS_QEMU_IMG:-$ROOT_DIR/bulumos-uefi-x86.img}"
DISK_IMG="${BULUMOS_DISK_IMG:-$ROOT_DIR/disk-x86.img}"
SERIAL_LOG="${BULUMOS_SERIAL_LOG:-$ROOT_DIR/boot-gui-x86.log}"
QEMU_MON="${BULUMOS_QEMU_MON:-/tmp/qemu-mon-x86.sock}"
SCREENSHOT_PPM="${BULUMOS_SCREENSHOT_PPM:-$ROOT_DIR/shot-x86-ppm.ppm}"
SCREENSHOT_PNG="${BULUMOS_SCREENSHOT_PNG:-$ROOT_DIR/shot-x86.png}"
VNC_PORT="${BULUMOS_VNC_PORT:-5903}"
QEMU_PID=""

cleanup() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$QEMU_MON"
}
trap cleanup EXIT

if [[ "$EUID" -ne 0 ]]; then
    echo "Run as root for QEMU device access." >&2
    exit 1
fi

for tool in qemu-system-x86_64 python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done

[[ -f "$QEMU_IMG" ]] || { echo "EFI image not found: $QEMU_IMG" >&2; exit 1; }
[[ -f "$DISK_IMG" ]] || { echo "Disk image not found: $DISK_IMG" >&2; exit 1; }

pkill -f qemu-system-x86_64 2>/dev/null || true
sleep 1
rm -f "$SERIAL_LOG" "$SCREENSHOT_PPM" "$SCREENSHOT_PNG"
rm -f /tmp/bulumos-vars4m.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/bulumos-vars4m.fd 2>/dev/null || true

echo "== Booting BulumOS x86-64 Plasma (VNC on :$VNC_PORT) =="
qemu-system-x86_64 -M q35 -cpu max -m 512M -no-reboot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars4m.fd \
  -drive file="$QEMU_IMG",format=raw,if=virtio \
  -drive file="$DISK_IMG",format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2223-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci,xres=1280,yres=800 \
  -device virtio-keyboard-pci -device virtio-mouse-pci \
  -monitor unix:"$QEMU_MON",server,nowait \
  -serial file:"$SERIAL_LOG" \
  -display vnc=127.0.0.1:"$VNC_PORT" &
QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

# Wait for the desktop to be ready (serial login prompt or "BulumOS is ready")
echo "Waiting for boot (up to 120s)..."
for i in $(seq 1 120); do
    if grep -aq "BulumOS is ready" "$SERIAL_LOG" 2>/dev/null; then
        echo "Desktop ready after ~$i seconds."
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU exited early." >&2
        tail -50 "$SERIAL_LOG" >&2
        exit 1
    fi
    sleep 1
done

# Extra wait for Plasma to fully start
sleep 10

echo "== Capturing screenshot via VNC =="

# Try Python + PIL first (most reliable for VNC)
if python3 -c "import PIL; print('PIL available')" 2>/dev/null; then
    echo "Using Python PIL + VNC capture..."
    python3 - "$VNC_PORT" "$SCREENSHOT_PNG" <<'PY'
import socket, time, sys, struct
from PIL import Image
from io import BytesIO

vnc_port = int(sys.argv[1])
out_png = sys.argv[2]

# RFB protocol: connect, negotiate, wait for frames
def vnc_capture(port, output, wait_seconds=5):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", port))
    
    # Read server initialization
    version = s.recv(12)
    s.sendall(b"RFB 003.003\n")
    
    # Server version response
    ver_resp = s.recv(12)
    
    # Authentication: no auth (type 1)
    s.sendall(b"\x01")
    
    # Client initialisation (shared desktop)
    s.sendall(b"\x00\x00\x00\x00")
    
    # Server sets pixel format (assume we accept defaults)
    # Read server initialisation (width, height, etc.)
    header = s.recv(16)
    width = struct.unpack("!H", header[0:2])[0]
    height = struct.unpack("!H", header[2:4])[0]
    print(f"VNC desktop: {width}x{height}")
    
    # Wait for a frame update
    time.sleep(wait_seconds)
    
    # Send a SetEncodings request (raw encoding)
    encodings = [0]  # Raw encoding
    msg = b"\x08" + struct.pack("!I", len(encodings) * 4) + b""
    for enc in encodings:
        msg += struct.pack("!I", enc)
    s.sendall(msg)
    
    # FramebufferUpdate request
    time.sleep(2)
    
    # Read FramebufferUpdate
    fb_header = s.recv(8)
    fb_type = fb_header[0]
    if fb_type != 0:
        print(f"Unexpected VNC message type: {fb_type}")
    
    num_rects = struct.unpack("!I", fb_header[4:8])[0]
    print(f"FramebufferUpdate: {num_rects} rectangles")
    
    raw_data = b""
    for _ in range(num_rects):
        rect_header = s.recv(12)
        x, y, w, h = struct.unpack("!HHII", rect_header[:12])
        rect_type = rect_header[12] if len(rect_header) > 12 else 0
        
        # Read pixel data for this rectangle
        if rect_type == 0:  # Raw
            pixel_bytes = w * h * 4  # Assume 32-bit
            chunk = b""
            while len(chunk) < pixel_bytes:
                chunk += s.recv(pixel_bytes - len(chunk))
            raw_data += chunk[:pixel_bytes]
    
    s.close()
    
    if raw_data:
        # Create image from raw BGRA data
        img = Image.frombytes("BGRA", (width, height), raw_data, "raw", "BGRA")
        img.save(output, "PNG")
        print(f"Saved VNC screenshot to {output}: {width}x{height}")
        return True
    
    return False

success = vnc_capture(vnc_port, out_png)
sys.exit(0 if success else 1)
PY
    if [[ -f "$SCREENSHOT_PNG" ]] && [[ $(stat -c%s "$SCREENSHOT_PNG" 2>/dev/null || echo 0) -gt 10000 ]]; then
        echo "VNC screenshot captured successfully: $SCREENSHOT_PNG"
        exit 0
    fi
fi

# Fallback: use ImageMagick to capture from a VNC display
if command -v convert >/dev/null 2>&1; then
    echo "Using ImageMagick to capture VNC screenshot..."
    convert "vcn://127.0.0.1:$VNC_PORT" "$SCREENSHOT_PNG" 2>/dev/null && {
        echo "Screenshot captured: $SCREENSHOT_PNG"
        exit 0
    }
fi

# Fallback: use x11vnc + import (if we had X11 access, but we don't in headless)
# Last resort: try to get the framebuffer content via the QEMU monitor's
# 'screendump' but that's what fails. Instead, use the VNC raw socket capture.

echo "Building a minimal VNC client to capture the framebuffer..."
python3 - "$VNC_PORT" "$SCREENSHOT_PNG" <<'PYFALLBACK'
import socket, time, sys, struct

port = int(sys.argv[1])
out = sys.argv[2]

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(("127.0.0.1", port))

# RFB 3.3 handshake
s.recv(12)  # server version
s.sendall(b"RFB 003.003\n")
s.recv(12)  # server version response

# No authentication
s.sendall(b"\x01")  # bytes read: 1
s.recv(4)   # auth result

# Client init: shared
s.sendall(b"\x00\x00\x00\x00")
server_init = s.recv(16)
width = struct.unpack("!H", server_init[0:2])[0]
height = struct.unpack("!H", server_init[2:4])[0]
print(f"VNC server: {width}x{height}", file=sys.stderr)

# Select raw encoding
enc_msg = b"\x08" + struct.pack("!I", 4) + struct.pack("!I", 0)  # raw = 0
s.sendall(enc_msg)

# Trigger a full framebuffer update
# Send FramebufferUpdateRequest (incremental=False, x=0, y=0, w, h, and-more=1)
msg = b"\x03" + struct.pack("!HHII", 0, 0, width, height) + b"\x01"
s.sendall(msg)

# Wait for the update
time.sleep(3)

# Read FramebufferUpdate header
data = b""
while len(data) < 8:
    chunk = s.recv(8 - len(data))
    if not chunk:
        break
    data += chunk

if len(data) < 8:
    print("Failed to read FB update header", file=sys.stderr)
    sys.exit(1)

msg_type, = struct.unpack("!B", data[0:1])
num_rects, = struct.unpack("!I", data[4:8])

all_pixels = b""
remaining = num_rects
while remaining > 0:
    rect_hdr = b""
    while len(rect_hdr) < 12:
        chunk = s.recv(12 - len(rect_hdr))
        if not chunk:
            break
        rect_hdr += chunk
    if len(rect_hdr) < 12:
        break
    rx, ry, rw, rh = struct.unpack("!HHII", rect_hdr)
    enc_byte = s.recv(1)
    if not enc_byte:
        break
    enc = enc_byte[0]
    
    if enc == 0:  # Raw
        bytes_needed = rw * rh * 4  # BGRA
        pixels = b""
        while len(pixels) < bytes_needed:
            chunk = s.recv(min(bytes_needed - len(pixels), 65536))
            if not chunk:
                break
            pixels += chunk
        all_pixels += pixels[:bytes_needed]
    elif enc == 2:  # CopyRect
        # Skip copyrect data (destination rect + src rect)
        s.recv(4)
    else:
        print(f"Unknown encoding: {enc}", file=sys.stderr)
    
    remaining -= 1

s.close()

if all_pixels:
    # Write PPM
    with open(out, "wb") as f:
        f.write(f"P6\n{width} {height}\n255\n".encode())
        f.write(all_pixels[:width * height * 3])
    print(f"Wrote {out}: {width}x{height}, {len(all_pixels)} bytes of pixel data", file=sys.stderr)
else:
    print("No pixel data captured", file=sys.stderr)
    sys.exit(1)
PYFALLBACK

if [[ -f "$SCREENSHOT_PPM" ]]; then
    python3 "$ROOT_DIR/tools/ppm2png.py" "$SCREENSHOT_PPM" "$SCREENSHOT_PNG"
    echo "Screenshot saved: $SCREENSHOT_PNG"
else
    echo "Failed to capture screenshot." >&2
    exit 1
fi

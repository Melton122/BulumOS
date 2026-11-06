#!/bin/bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE="${BULUMOS_BUILD_DIR:-$ROOT/out/plasma-x86}"
KERNEL="$ROOT/kernel/arch/x86/boot/bzImage"
INITRD="$ROOT/initramfs-switch-x86.cpio.gz"
OUT_PREFIX="${BULUMOS_TEST_PREFIX:-$ROOT/out/plasma-x86-qemu}"
TEST_ROOT_DISK="${BULUMOS_TEST_ROOT_DISK:-}"
if [[ -n "$TEST_ROOT_DISK" ]]; then
    DISK="$TEST_ROOT_DISK"
    OWN_DISK=0
else
    DISK="${OUT_PREFIX}-root.img"
    OWN_DISK=1
fi
DUMMY_DISK="${OUT_PREFIX}-dummy.img"
SERIAL_LOG="${OUT_PREFIX}-serial.log"
QEMU_LOG="${OUT_PREFIX}.log"
QMP="${OUT_PREFIX}-qmp.sock"
SCREEN="${OUT_PREFIX}-screen.ppm"
AUTOTEST="$STAGE/etc/bulum/autotest.sh"
QEMU_PID=
TEST_INJECTED=0

if [[ "$EUID" -ne 0 ]]; then
    echo "Run the QEMU image test as root." >&2
    exit 1
fi
for f in "$KERNEL" "$INITRD" "$STAGE/sbin/init" "$STAGE/usr/bin/plasmashell"; do
    [[ -e "$f" ]] || { echo "Required file is missing: $f" >&2; exit 1; }
done
for tool in qemu-system-x86_64 mkfs.ext4 python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done
for output in "$DUMMY_DISK" "$SERIAL_LOG" "$QEMU_LOG" "$QMP" "$SCREEN"; do
    if [[ -e "$output" ]]; then
        echo "Refusing to overwrite existing test artifact: $output" >&2
        exit 1
    fi
done
if [[ "$OWN_DISK" -eq 1 && -e "$DISK" ]]; then
    echo "Refusing to overwrite existing test artifact: $DISK" >&2
    exit 1
fi
if [[ "$OWN_DISK" -eq 0 && ! -f "$DISK" ]]; then
    echo "Test root disk does not exist: $DISK" >&2
    exit 1
fi
if [[ -e "$AUTOTEST" ]]; then
    echo "Refusing to replace existing staged autotest: $AUTOTEST" >&2
    exit 1
fi

cleanup() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [[ "$TEST_INJECTED" -eq 1 ]]; then
        debugfs -w -R "rm /etc/bulum/autotest.sh" "$DISK" >/dev/null 2>&1 || true
    fi
    rm -f "$QMP" "$AUTOTEST"
}
trap cleanup EXIT

cat > "$AUTOTEST" <<'EOF'
#!/bin/sh
LOG=/var/log/bulum/plasma-boot-test.log
mkdir -p /var/log/bulum
exec > "$LOG" 2>&1
echo PLASMA_BOOT_CHECK_BEGIN
echo PLASMA_BOOT_CHECK_BEGIN > /dev/ttyS0

pass=0
fail=0
check() {
    name=$1
    shift
    if "$@"; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name"
        fail=$((fail + 1))
    fi
}

tries=0
while [ "$tries" -lt 150 ]; do
    if [ -S /tmp/.X11-unix/X0 ] &&
       /bin/busybox pidof plasmashell >/dev/null 2>&1 &&
       /bin/busybox pidof kwin_x11 >/dev/null 2>&1; then
        break
    fi
    sleep 1
    tries=$((tries + 1))
done

su -l -s /bin/sh -c \
   'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 15 xset s off -dpms' \
   melton

ps -eo pid,uid,comm,args |
    grep -E '(^ *PID|plasma|kwin|systemd|Xorg|dbus|kdeinit|klauncher|ksmserver)'
plasmashell_pid=$(/bin/busybox pidof plasmashell 2>/dev/null)
plasmashell_pid=${plasmashell_pid%% *}
session_bus=
if [ -n "$plasmashell_pid" ]; then
    tr '\000' '\n' < "/proc/$plasmashell_pid/environ" |
        grep -E '^(DISPLAY|DBUS_SESSION_BUS_ADDRESS|HOME|KDE_|XDG_)'
    session_bus=$(tr '\000' '\n' < "/proc/$plasmashell_pid/environ" |
        sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')
fi
if [ -n "$session_bus" ]; then
    tries=0
    while [ "$tries" -lt 180 ]; do
        if su -l -s /bin/sh -c \
            "DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS='$session_bus' timeout -k 2 8 dbus-send --session --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:org.kde.plasmashell" \
            melton >/dev/null 2>&1 &&
           su -l -s /bin/sh -c \
            'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 8 xwininfo -root -tree' \
            melton 2>/dev/null |
            grep -Eq '1280x[0-9]{1,2}\+0\+[1-9][0-9]+'; then
            break
        fi
        sleep 1
        tries=$((tries + 1))
    done
    check "Plasma session D-Bus responds" \
        su -l -s /bin/sh -c \
        "DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS='$session_bus' timeout -k 2 8 dbus-send --session --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames" \
        melton
    check "Plasma shell owns its D-Bus service" \
        su -l -s /bin/sh -c \
        "DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS='$session_bus' timeout -k 2 8 dbus-send --session --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner string:org.kde.plasmashell" \
        melton
    check "AccountsService responds on the system bus" \
        su -l -s /bin/sh -c \
        "DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS='$session_bus' timeout -k 2 15 dbus-send --system --dest=org.freedesktop.Accounts --type=method_call --print-reply /org/freedesktop/Accounts org.freedesktop.Accounts.ListCachedUsers" \
        melton
    check "BulumOS account-management authorization rule is installed" \
        test -f /etc/polkit-1/rules.d/50-bulum-user-management.rules
    check "BulumOS desktop user is an account administrator" \
        su -l -s /bin/sh -c 'id -nG | grep -qw bulum-admin' melton
    test_user="bulumtest$(date +%s)"
    create_status=0
    su -l -s /bin/sh -c \
        "timeout -k 2 20 dbus-send --system --dest=org.freedesktop.Accounts --type=method_call --print-reply /org/freedesktop/Accounts org.freedesktop.Accounts.CreateUser string:$test_user string:BulumOS_Test int32:0" \
        melton || create_status=$?
    if getent passwd "$test_user" >/dev/null; then
        echo "PASS: Authorized account creation works through AccountsService"
        pass=$((pass + 1))
        if [ "$create_status" -ne 0 ]; then
            echo "AccountsService created the account but did not return a successful D-Bus reply."
        fi
        test_uid=$(id -u "$test_user")
        check "Temporary test account can be removed" \
            su -l -s /bin/sh -c \
            "timeout -k 2 20 dbus-send --system --dest=org.freedesktop.Accounts --type=method_call --print-reply /org/freedesktop/Accounts org.freedesktop.Accounts.DeleteUser int64:$test_uid boolean:true" \
            melton
    else
        echo "FAIL: Authorized account creation works through AccountsService"
        fail=$((fail + 1))
    fi
else
    echo "FAIL: could not read Plasma session D-Bus address"
    fail=$((fail + 1))
fi
sleep 5
check "BulumOS volatile runtime directory is tmpfs" \
    grep -q '^tmpfs /run tmpfs ' /proc/mounts
check "D-Bus system bus is running" /bin/busybox pidof dbus-daemon
check "Xorg display socket exists" test -S /tmp/.X11-unix/X0
check "Xorg process is running" /bin/busybox pidof Xorg
check "Plasma shell process is running" /bin/busybox pidof plasmashell
check "KWin X11 process is running" /bin/busybox pidof kwin_x11
check "Konsole terminal is installed" test -x /usr/bin/konsole
check "Konsole terminal responds for the BulumOS user" \
    su -l -s /bin/sh -c \
    'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 15 konsole --version' \
    melton
check "KDE System Settings is installed" test -x /usr/bin/systemsettings
check "Plasma Users module is installed" \
    test -f /usr/lib/x86_64-linux-gnu/qt5/plugins/plasma/kcms/systemsettings/kcm_users.so
check "AccountsService D-Bus activation is installed" \
    test -f /usr/share/dbus-1/system-services/org.freedesktop.Accounts.service
check "Polkit D-Bus activation is installed" \
    test -f /usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service
check "X11 accepts client queries" \
    timeout -k 2 15 env DISPLAY=:0 xdpyinfo
check "BulumOS account can connect to X11" \
    su -l -s /bin/sh -c \
    'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 15 xdpyinfo' melton
su -l -s /bin/sh -c \
    'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 15 xmessage -center -timeout 120 -title "BulumOS Plasma" "Plasma graphics check"' \
    melton >/dev/null 2>&1 &
tries=0
while [ "$tries" -lt 10 ] && ! /bin/busybox pidof xmessage >/dev/null 2>&1; do
    sleep 1
    tries=$((tries + 1))
done
check "X11 graphical test window is running" /bin/busybox pidof xmessage
su -l -s /bin/sh -c \
    "DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS='$session_bus' timeout -k 2 30 systemsettings kcm_users" \
    melton >/dev/null 2>&1 &
tries=0
while [ "$tries" -lt 10 ] && ! /bin/busybox pidof systemsettings >/dev/null 2>&1; do
    sleep 1
    tries=$((tries + 1))
done
check "KDE Users settings module launches" /bin/busybox pidof systemsettings
window_tree=
tries=0
while [ "$tries" -lt 60 ]; do
    window_tree=$(su -l -s /bin/sh -c \
        'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 15 xwininfo -root -tree' \
        melton)
    if printf '%s\n' "$window_tree" | grep -qi systemsettings &&
       printf '%s\n' "$window_tree" |
        grep -Eq '1280x[0-9]{1,2}\+0\+[1-9][0-9]+'; then
        break
    fi
    sleep 1
    tries=$((tries + 1))
done
printf '%s\n' "$window_tree"
check "Plasma desktop surface is mapped" \
    sh -c 'printf "%s\n" "$1" | grep -Eq "1280x800"' sh "$window_tree"
check "Plasma panel is mapped" \
    sh -c 'printf "%s\n" "$1" | grep -Eq "1280x[0-9]{1,2}\\+0\\+[1-9][0-9]+"' sh "$window_tree"
check "System Settings window is visible" \
    sh -c 'printf "%s\n" "$1" | grep -qi systemsettings' sh "$window_tree"
su -l -s /bin/sh -c \
    'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 15 xprop -root _NET_CLIENT_LIST _NET_SUPPORTING_WM_CHECK' \
    melton
check "KWin advertises its managed X11 root window" \
    sh -c 'DISPLAY=:0 xprop -root _NET_SUPPORTING_WM_CHECK | grep -q "window id"'
check "BulumOS account can execute session commands" \
    su -l -s /bin/sh -c 'test "$(id -u)" = 1000' melton
check "BulumOS buser is installed" test -x /usr/bin/buser
check "BulumOS service manager is installed" test -x /sbin/bctl

# ---- interactive input: is this a real, usable desktop? ----------
check "udev device manager is running" \
    sh -c 'for p in /proc/[0-9]*; do [ "$(cat "$p/comm" 2>/dev/null)" = systemd-udevd ] && exit 0; done; exit 1'
check "Kernel exposes input event devices" \
    sh -c 'ls /dev/input/event* >/dev/null 2>&1'
input_list=$(su -l -s /bin/sh -c \
    'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 timeout -k 2 20 xinput list --short 2>&1' \
    melton 2>&1)
printf '%s\n' "xinput devices:" "$input_list"
check "Xorg exposes a real keyboard device" \
    sh -c 'printf "%s\n" "$1" | grep -vi "xtest\|virtual core" | grep -qi keyboard' sh "$input_list"
check "Xorg exposes a real pointer device" \
    sh -c 'printf "%s\n" "$1" | grep -vi "xtest\|virtual core" | grep -qiE "mouse|pointer|touchpad|tablet"' sh "$input_list"
echo "--- udev start log ---"
tail -25 /var/log/bulum/udev-start.log 2>/dev/null || true
echo "--- kernel input devices ---"
grep -E '^(I|H|N):' /proc/bus/input/devices 2>/dev/null | head -18 || true
echo "--- udev properties for the first event device ---"
udevadm info --query=property --name=/dev/input/event0 2>&1 | head -15 || true
check "udev tagged the input devices for the X server" \
    sh -c 'udevadm info --query=property --name=/dev/input/event0 2>/dev/null | grep -q "^ID_INPUT"'
echo "--- Xorg input log ---"
grep -iE 'libinput|input driver|keyboard|mouse' /var/log/Xorg.0.log 2>/dev/null | tail -12 || true
check "Xorg loaded the libinput driver" \
    sh -c 'grep -qi libinput /var/log/Xorg.0.log'
su -l -s /bin/sh -c \
    'DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 exec timeout -k 2 60 xinput test-xi2 --root' \
    melton > /tmp/xinput-events.log 2>&1 &
sleep 3
echo PLASMA_BOOT_INPUT_READY > /dev/ttyS0
tries=0
while [ "$tries" -lt 40 ]; do
    if grep -q KeyPress /tmp/xinput-events.log 2>/dev/null; then
        break
    fi
    sleep 1
    tries=$((tries + 1))
done
grep -m 6 'EVENT\|device:' /tmp/xinput-events.log 2>/dev/null || true
check "Injected keyboard events reach the X server" \
    grep -q KeyPress /tmp/xinput-events.log

# ---- desktop-initiated power: the Leave menu path --------------------
check "logind power service is installed" test -x /usr/sbin/blogind
tries=0
while [ "$tries" -lt 60 ]; do
    if dbus-send --system --print-reply --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner \
        string:org.freedesktop.login1 >/dev/null 2>&1; then
        break
    fi
    sleep 1
    tries=$((tries + 1))
done
echo "logind bus name appeared after ${tries}s"
logind_user_reply=$(su -l -s /bin/sh -c \
    'timeout -k 2 10 dbus-send --system --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager.CanPowerOff' \
    melton 2>&1)
printf '%s\n' "--- logind as desktop user ---" "$logind_user_reply"
check "logind power service owns its system bus name" \
    sh -c 'printf "%s\n" "$1" | grep -q yes' sh "$logind_user_reply"
check "logind reports that power off is available" \
    sh -c 'dbus-send --system --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager.CanPowerOff 2>/dev/null | grep -q yes'

echo PLASMA_BOOT_SCREEN_READY
echo PLASMA_BOOT_SCREEN_READY > /dev/ttyS0
echo "PLASMA_BOOT_CHECK_RESULT pass=$pass fail=$fail"
echo PLASMA_BOOT_CHECK_END
grep -E '^(PASS|FAIL):' "$LOG" > /dev/ttyS0
echo "PLASMA_BOOT_CHECK_RESULT pass=$pass fail=$fail" > /dev/ttyS0
echo PLASMA_BOOT_CHECK_END > /dev/ttyS0
# Ask the session to shut down exactly like the Leave menu does. If the
# machine is still alive after 45 seconds, the request never reached BulumOS.
echo PLASMA_BOOT_SHUTDOWN_TRIGGER > /dev/ttyS0
su -l -s /bin/sh -c \
    "DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS='$session_bus' timeout -k 2 20 qdbus org.kde.ksmserver /KSMServer org.kde.KSMServerInterface.logout 0 2 1" \
    melton >> /var/log/bulum/plasma-boot-test.log 2>&1
tries=0
while [ "$tries" -lt 45 ]; do
    sleep 1
    tries=$((tries + 1))
done
echo PLASMA_BOOT_SHUTDOWN_TIMEOUT > /dev/ttyS0
/bin/busybox kill -TERM 1
EOF
chmod 755 "$AUTOTEST"

if [[ "$OWN_DISK" -eq 1 ]]; then
    echo "== Creating isolated 3 GiB ext4 test disk from $STAGE =="
    truncate -s 3G "$DISK"
    mkfs.ext4 -F -m 0 -L BULUMROOT -d "$STAGE" "$DISK"
else
    command -v debugfs >/dev/null 2>&1 || {
        echo "Missing required tool: debugfs" >&2
        exit 1
    }
    echo "== Adding temporary guest checks to $DISK =="
    debugfs -w -R "write $AUTOTEST /etc/bulum/autotest.sh" "$DISK" >/dev/null
    debugfs -w -R "set_inode_field /etc/bulum/autotest.sh mode 0x81ed" \
        "$DISK" >/dev/null
    TEST_INJECTED=1
fi
truncate -s 1M "$DUMMY_DISK"
mkfs.ext4 -F -m 0 -L BOOTDUMMY "$DUMMY_DISK" >/dev/null

ACCEL=(-accel tcg)

echo "== Booting BulumOS/Plasma in QEMU (serial: $SERIAL_LOG) =="
qemu-system-x86_64 \
    -M q35 -cpu max -m 4096 "${ACCEL[@]}" -no-reboot \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -append "console=ttyS0,115200n8 root=/dev/vdb rw rootfstype=ext4 init=/sbin/init" \
    -drive "file=$DUMMY_DISK,format=raw,if=virtio" \
    -drive "file=$DISK,format=raw,if=virtio" \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0 \
    -device virtio-vga,xres=1280,yres=800 \
    -device virtio-keyboard-pci \
    -device virtio-mouse-pci \
    -display vnc=127.0.0.1:9 \
    -serial "file:$SERIAL_LOG" \
    -monitor none \
    -qmp "unix:$QMP,server=on,wait=off" \
    > "$QEMU_LOG" 2>&1 &
QEMU_PID=$!

echo "== Waiting for the guest keyboard listener =="
for _ in $(seq 1 900); do
    if grep -q "PLASMA_BOOT_INPUT_READY" "$SERIAL_LOG" 2>/dev/null ||
       grep -q "PLASMA_BOOT_SCREEN_READY" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU exited before the guest keyboard listener was ready." >&2
        tail -100 "$SERIAL_LOG" >&2 || true
        exit 1
    fi
    sleep 1
done
if grep -q "PLASMA_BOOT_INPUT_READY" "$SERIAL_LOG" 2>/dev/null; then
    echo "== Injecting keyboard input through QMP =="
    python3 - "$QMP" <<'PY'
import json
import socket
import sys
import time

path = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
for _ in range(50):
    try:
        sock.connect(path)
        break
    except OSError:
        time.sleep(0.1)
else:
    raise SystemExit("QMP socket did not become available")
stream = sock.makefile("rwb", buffering=0)
stream.readline()


def command(payload):
    stream.write(json.dumps(payload).encode() + b"\r\n")
    while True:
        response = json.loads(stream.readline())
        if "return" in response or "error" in response:
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["return"]


command({"execute": "qmp_capabilities"})
time.sleep(2)
for key in ("a", "b", "c", "ret"):
    command({"execute": "send-key",
             "arguments": {"keys": [{"type": "qcode", "data": key}],
                           "hold-time": 100}})
    time.sleep(0.2)
sock.close()
PY
fi

for _ in $(seq 1 900); do
    if grep -q "PLASMA_BOOT_SCREEN_READY" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU exited before the framebuffer capture was ready." >&2
        cat "$QEMU_LOG" >&2
        [[ ! -f "$SERIAL_LOG" ]] || tail -100 "$SERIAL_LOG" >&2
        exit 1
    fi
    sleep 1
done
if ! grep -q "PLASMA_BOOT_SCREEN_READY" "$SERIAL_LOG" 2>/dev/null; then
    echo "Timed out waiting for the desktop framebuffer capture." >&2
    tail -120 "$SERIAL_LOG" >&2 || true
    tail -40 "$QEMU_LOG" >&2 || true
    exit 1
fi

python3 - "$QMP" "$SCREEN" <<'PY'
import json
import socket
import sys
import time

path, screenshot = sys.argv[1:]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
for _ in range(50):
    try:
        sock.connect(path)
        break
    except OSError:
        time.sleep(0.1)
else:
    raise SystemExit("QMP socket did not become available")

stream = sock.makefile("rwb", buffering=0)
stream.readline()

def command(payload):
    stream.write(json.dumps(payload).encode() + b"\r\n")
    while True:
        response = json.loads(stream.readline())
        if "return" in response or "error" in response:
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["return"]

command({"execute": "qmp_capabilities"})
time.sleep(6)
command({
    "execute": "human-monitor-command",
    "arguments": {"command-line": "screendump " + screenshot},
})
sock.close()
PY
for _ in $(seq 1 60); do
    if grep -q "PLASMA_BOOT_CHECK_END" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "QEMU exited before the guest boot checks completed." >&2
        tail -100 "$SERIAL_LOG" >&2
        exit 1
    fi
    sleep 1
done
if ! grep -q "PLASMA_BOOT_CHECK_END" "$SERIAL_LOG" 2>/dev/null; then
    echo "Timed out waiting for the guest boot checks." >&2
    tail -120 "$SERIAL_LOG" >&2
    exit 1
fi
for _ in $(seq 1 60); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 1
done
if kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "Guest did not shut down cleanly after the boot test." >&2
    tail -100 "$SERIAL_LOG" >&2
    exit 1
fi
wait "$QEMU_PID" || true
QEMU_PID=
if [[ "$TEST_INJECTED" -eq 1 ]]; then
    debugfs -w -R "rm /etc/bulum/autotest.sh" "$DISK" >/dev/null
    TEST_INJECTED=0
fi
rm -f "$QMP" "$AUTOTEST"
trap - EXIT

if e2fsck -fy "$DISK"; then
    :
else
    fsck_status=$?
    if [[ "$fsck_status" -gt 1 ]]; then
        echo "Test disk filesystem recovery failed (e2fsck status $fsck_status)." >&2
        exit "$fsck_status"
    fi
fi
e2fsck -fn "$DISK"

python3 - "$SCREEN" <<'PY'
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
header = b"P6\n1280 800\n255\n"
if not data.startswith(header) or len(data) != len(header) + 1280 * 800 * 3:
    raise SystemExit("Framebuffer capture has an unexpected PPM format or size.")
pixels = data[len(header):]
nonblack = sum(
    pixels[index:index + 3] != b"\0\0\0"
    for index in range(0, len(pixels), 3)
)
minimum = 1280 * 800 * 2 // 100
if nonblack < minimum:
    raise SystemExit(
        f"Framebuffer capture has only {nonblack} nonblack pixels; "
        f"expected at least {minimum}."
    )
PY

echo "== Guest boot checks =="
grep -E 'PLASMA_BOOT_CHECK_|^(PASS|FAIL):' "$SERIAL_LOG" || true
if ! grep -q 'PLASMA_BOOT_CHECK_RESULT pass=[0-9][0-9]* fail=0' "$SERIAL_LOG"; then
    echo "One or more Plasma boot checks failed. Guest log: $SERIAL_LOG" >&2
    echo "Service log is in the guest disk at /var/log/bulum/plasma-boot-test.log" >&2
    exit 1
fi
if grep -q "PLASMA_BOOT_SHUTDOWN_TIMEOUT" "$SERIAL_LOG" 2>/dev/null; then
    echo "Plasma's Leave-menu shutdown request did not power the machine off." >&2
    exit 1
fi
if grep -q "PLASMA_BOOT_SHUTDOWN_TRIGGER" "$SERIAL_LOG" 2>/dev/null; then
    echo "Plasma Leave-menu shutdown request powered the machine off."
fi
echo "QEMU boot test passed; framebuffer capture: $SCREEN"
if [[ "$OWN_DISK" -eq 1 && "${BULUMOS_TEST_KEEP_DISK:-0}" != "1" ]]; then
    rm -f "$DISK" "$DUMMY_DISK"
    echo "Removed the temporary test disk (set BULUMOS_TEST_KEEP_DISK=1 to keep it)."
fi

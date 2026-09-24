#!/bin/bash
# boot-bpk-vm.sh - boot the OS and attach an interactive console (tmux).
# usage: tools/boot-bpk-vm.sh [arm64|x86_64]
set -e
cd /home/melton/BulumOS
ARCH=${1:-arm64}
PORT_MAIN=17474
PORT_OFF=17475

if [ "$ARCH" = arm64 ]; then
    UEFI=bulumos-uefi.img; DISK=disk.img
    UEFI_CODE=/usr/share/AAVMF/AAVMF_CODE.fd
    VARS=/usr/share/AAVMF/AAVMF_VARS.fd
    CPU=cortex-a72; MACH="-M virt"; QEMU=qemu-system-aarch64
else
    UEFI=bulumos-uefi-x86.img; DISK=disk-x86.img
    UEFI_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
    VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
    CPU=max; MACH="-M q35"; QEMU=qemu-system-x86_64
fi

bash tools/make-test-repo.sh >/dev/null 2>&1 || { echo "make-test-repo failed"; exit 1; }
[ -f out/repo/Packages ] || bash tools/build-official-repo.sh >/dev/null
pkill -f "http.server $PORT_MAIN" 2>/dev/null || true
pkill -f "http.server $PORT_OFF" 2>/dev/null || true
python3 -m http.server $PORT_MAIN --directory /tmp/bulum-repo > /tmp/bpk-main-httpd.log 2>&1 &
python3 -m http.server $PORT_OFF --directory /home/melton/BulumOS/out/repo > /tmp/bpk-off-httpd.log 2>&1 &
sleep 1

cp "$VARS" /tmp/bulumos-bpk-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
pkill -f qemu-system 2>/dev/null || true
sleep 1

echo "=== booting $ARCH (login: root / root) ==="
tmux new-session -d -s qemu "
$QEMU $MACH -cpu $CPU -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=$UEFI_CODE \
  -drive if=pflash,format=raw,file=/tmp/bulumos-bpk-vars.fd \
  -drive file=/home/melton/BulumOS/$UEFI,format=raw,if=virtio \
  -drive file=/home/melton/BulumOS/$DISK,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -nographic 2>&1 | tee /home/melton/BulumOS/boot-bpk-interactive.log"
echo "waiting 75s for boot... then attach with:  tmux attach -t qemu"
sleep 75
tmux attach -t qemu

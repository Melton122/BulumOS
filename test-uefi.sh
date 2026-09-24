#!/bin/bash
set -e
cd /home/melton/myos
echo "=== rebuilding ISO (with embedded-memdisk EFI app) ==="
bash build-iso.sh 2>&1 | grep -E '===|EFI|boot|Image|initramfs|Written|sectors|ISO|BOOTAA64'
echo "=== UEFI boot test ==="
rm -f /home/melton/melton 2>/dev/null || true
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null
pkill -f qemu-system 2>/dev/null
sleep 1
rm -f boot-uefi.log
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=/home/melton/myos/bulumos.iso,format=raw,if=none,id=cd0 \
  -device virtio-scsi-pci -device scsi-cd,drive=cd0,bootindex=0 \
  -drive file=/home/melton/myos/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -device virtio-gpu-pci \
  -nographic > /home/melton/myos/boot-uefi.log 2>&1"
echo "started; waiting..."
sleep 45
echo "=== log tail ==="
tail -c 2500 /home/melton/myos/boot-uefi.log | tr -d '\033' | tail -40

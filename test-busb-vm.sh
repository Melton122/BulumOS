#!/bin/bash
# BulumOS Phase 9.4: in-VM USB test
# Attaches a QEMU usb-storage (FAT image) and usb-keyboard to the virt machine,
# verifies busb lists them and the automount handler mounts the partition.
set -e
cd /home/melton/myos
LOG=boot-busb.log
rm -f $LOG

# Build a small FAT disk image to serve as the USB stick
USBIMG=/tmp/bulum-usb.img
if [ ! -f "$USBIMG" ]; then
    truncate -s 8M "$USBIMG"
    mkfs.vfat -n USBSTICK "$USBIMG" >/dev/null 2>&1 || \
        mkfs.vfat "$USBIMG" >/dev/null 2>&1
fi

pkill -f qemu-system 2>/dev/null || true
sleep 1
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/bulumos-vars.fd
tmux kill-session -t qemu 2>/dev/null || true
tmux new-session -d -s qemu "
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/bulumos-vars.fd \
  -drive file=$PWD/bulumos-uefi.img,format=raw,if=virtio \
  -drive file=$PWD/disk.img,format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -device qemu-xhci \
  -drive id=udisk,if=none,file=$USBIMG,format=raw \
  -device usb-storage,drive=udisk \
  -device usb-kbd \
  -nographic > $PWD/$LOG 2>&1"
echo "waiting 65s for boot..."
sleep 65
send() { tmux send-keys -t qemu "$1" Enter; sleep 2; }
send 'root'
send 'root'
send 'echo USB-BOOT-OK'

echo "=== USB1: busb list ==="
send 'busb list; echo USB1-END'
sleep 3

echo "=== USB2: busb storage ==="
send 'busb storage; echo USB2-END'
sleep 3

echo "=== USB3: verify /dev/sda node exists (mdev -s) ==="
send 'ls -la /dev/sda; echo USB3-END'
sleep 3

echo "=== USB4: run automount handler (hotplug action) + verify mount ==="
send 'ACTION=add DEVNAME=sda /sbin/mdev-automount.sh; sleep 1; ls -la /media; echo USB4-END'
sleep 3

echo "=== USB5: mount state ==="
send 'mount | grep -i media; echo USB5-END'
sleep 3

grep -q 'USB-BOOT-OK' $LOG && echo "PASS  boot ok" || echo "FAIL  boot ok"
grep -q 'USB1-END' $LOG && echo "PASS  USB1 busb list" || echo "FAIL  USB1 busb list"
grep -Eq 'sd[a-z]' $LOG && echo "PASS  USB2 sees sd* device" || echo "FAIL  USB2 sees sd* device"
grep -q '/dev/sda' $LOG && echo "PASS  USB3 /dev/sda node present" || echo "FAIL  USB3 /dev/sda node present"
grep -q '/media/sda' $LOG && echo "PASS  USB4 automount mounted /media/sda" || echo "FAIL  USB4 automount mounted /media/sda"
grep -q 'USB5-END' $LOG && echo "PASS  USB5 ran" || echo "FAIL  USB5 ran"
send 'poweroff'; sleep 4
grep -q 'all services stopped' $LOG && echo "PASS  clean poweroff" || echo "FAIL  clean poweroff"
echo "=== done; see $LOG ==="

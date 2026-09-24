#!/bin/bash
cd /home/melton/ksrc
grep -E "(EFI_STUB|VIRTIO_PCI|DRM_VIRTIO_GPU|DRM |VIRTIO_BLK|VIRTIO_NET|INPUT_EVDEV|SERIAL_8250_CONSOLE|VT |VT=|DEVTMPFS|BLK_DEV_INITRD|EXT4_FS|TMPFS=|SYSVIPC|FB_EFI|PCI_MSI)" .config | head -40
echo "===="
head -3 .config
wc -l .config
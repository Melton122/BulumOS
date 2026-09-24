#!/bin/bash
# Convert current arm64 .config to x86_64 and build bzImage.
set -e
cd /home/melton/ksrc
cp /home/melton/myos/kernel/.config .config
make ARCH=x86_64 olddefconfig
echo "=== key options on x86_64 ==="
for o in CONFIG_EFI_STUB CONFIG_VIRTIO_PCI CONFIG_DRM_VIRTIO_GPU CONFIG_VIRTIO_BLK CONFIG_VIRTIO_NET CONFIG_INPUT_EVDEV CONFIG_SERIAL_8250_CONSOLE CONFIG_VT CONFIG_DEVTMPFS_MOUNT CONFIG_BLK_DEV_INITRD CONFIG_RD_GZIP CONFIG_EXT4_FS CONFIG_TMPFS CONFIG_SYSVIPC CONFIG_FRAMEBUFFER_CONSOLE CONFIG_FB_EFI CONFIG_DRM CONFIG_PCI_MSI; do
  grep -q "^CONFIG_${o}=y" .config && echo "$o OK" || echo "$o --CHECK--"
done
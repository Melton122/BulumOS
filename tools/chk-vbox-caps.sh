#!/bin/bash
K=/home/melton/BulumOS/kernel/.config.x86_64
echo "== core ATA/SATA/SCSI/NVMe/E1000 =="
grep -E '^CONFIG_(ATA|ATA_PIIX|ATA_SFF|AHCI|SATA_AHCI|LIBATA|SCSI|BLK_DEV_SD|USB_STORAGE|NVME|E1000|RTL8139|AAHCI|PATA_)=' "$K"
echo "== other NICs vt8201/intel =="
grep -E '^CONFIG_(E100|R8169|TULIP|8139)=' "$K"
echo "== virtio current =="
grep -E '^CONFIG_VIRTIO_BLK=*|^CONFIG_VIRTIO_PCI=*' "$K"
echo "== efifb =="
grep -E '^CONFIG_FB_EFI=' "$K"
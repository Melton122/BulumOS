#!/bin/bash
M=~/grub-arm64-ubuntu/extracted/usr/lib/grub/arm64-efi
ls "$M" | grep -iE 'virtio|ata|usb|pci|scsi|mmc|nvme'
echo "---"
ls "$M" | grep -E 'efinet|net\.mod|http|tftp'

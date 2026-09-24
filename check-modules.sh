#!/bin/bash
MODS=~/grub-arm64/extracted/usr/lib/grub/arm64-efi
for m in normal linux iso9660 ext2 part_gpt part_msdos gzio search configfile serial terminal efi_gop echo ls cat test all_video boot videotest minicmd; do
  if [ -f "$MODS/$m.mod" ]; then echo "$m OK"; else echo "$m MISSING"; fi
done

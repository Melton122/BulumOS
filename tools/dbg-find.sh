#!/bin/bash
R="/home/melton/Bulum OS/rootfs"
for b in bctl bstatus blog bulum; do
  for d in sbin usr/sbin usr/bin bin; do
    [ -e "$R/$d/$b" ] && echo "$d/$b"
  done
done
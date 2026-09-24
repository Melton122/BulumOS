#!/bin/bash
cd /home/melton/BulumOS
miss=0
for m in packages/*.pk; do
  awk 'f&&NF{print} /^files[[:space:]]*$/{f=1}' "$m" | while read -r p; do
    for r in rootfs rootfs-x86; do
      if [ ! -f "$r/$p" ]; then echo "MISSING $r/$p for $m"; fi
    done
  done
done
echo "check done"
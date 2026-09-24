#!/bin/bash
for t in convert magick ffmpeg python3 socat nc ncat; do
  if command -v "$t" >/dev/null 2>&1; then echo "$t: yes"; else echo "$t: no"; fi
done
python3 -c "import PIL; print('pil: yes')" 2>/dev/null || echo "pil: no"
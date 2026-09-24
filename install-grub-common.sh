#!/bin/bash
echo '6374' | sudo -S apt-get install -y grub-common 2>&1 | tail -1
which grub-mkimage
grub-mkimage --version | head -1

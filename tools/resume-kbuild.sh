#!/bin/bash
cd /home/melton/ksrc
nohup make -j4 ARCH=x86_64 bzImage > /home/melton/BulumOS/tools/kbuild-x86.log 2>&1 &
echo "resumed make pid $!"
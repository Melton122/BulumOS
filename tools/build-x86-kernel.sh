#!/bin/bash
cd /home/melton/ksrc
make -j8 ARCH=x86_64 bzImage 2>&1 | tee /home/melton/BulumOS/tools/kbuild-x86.log | tail -3
echo "exit: ${PIPESTATUS[0]}"
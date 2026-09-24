#!/bin/bash
cd /home/melton/BulumOS
gcc -O2 -Wall -Wextra -o /tmp/bulum-check src/bulum.c src/bulum_crypto.c 2>&1 | head -60
echo "EXIT: $?"
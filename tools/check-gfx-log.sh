#!/bin/bash
cd /home/melton/BulumOS
LOG=boot-dbg.log
echo "=== state/log file outputs ==="
grep -aE 'Graphics: available|Framebuffer: /dev/fb0|deferred|running|not started' "$LOG" | grep -av 'send-keys\|\[K' | tail -30
echo "=== desktop.log content ==="
grep -aA3 'desktop.log' "$LOG" | grep -av 'send-keys\|\[K' | tail -10
echo "=== ps / bdesktop ==="
grep -aE 'bdesktop' "$LOG" | grep -av 'send-keys\|bdesktop -\|bdesktop --\|\[K\|=>' | tail -10
echo "=== .bapp files ==="
grep -aE '\.bapp' "$LOG" | grep -av 'send-keys\|\[K' | tail -6
echo "DONE"

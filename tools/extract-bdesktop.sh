#!/bin/bash
cd /home/melton/BulumOS
LOG=boot-bdesktop.log
echo "=== D6-RC (bdesktop --frames 30 exit code) ==="
grep -a 'D6-RC=' "$LOG" | tail -2
echo "=== any bdesktop self-test output around D6 ==="
awk '/D6-RC=/{exit} /bdesktop --frames 30/ {f=1} f' "$LOG" | grep -av '\[K\|send-keys\|^bdesktop\|--f' | grep -aE 'D6|desktop|framebuffer|fb|open|error|fail|Backend|window|BDESKTOP|Frames|init' | tail -20
echo "=== desktop.log content (bdesktop stderr) ==="
grep -a 'desktop.log' "$LOG" | grep -av 'send-keys\|\[K\|cat ' | head -2
awk '/desktop.log/ {f=1} f' "$LOG" | grep -av '\[K\|send-keys\|^root\|^cat\|desktop\.log' | tail -15
echo "=== desktop.state value ==="
grep -a 'deferred\|running\|STOPPED' "$LOG" | grep -av 'send-keys\|\[K\|supervise' | tail -6
echo "DONE"
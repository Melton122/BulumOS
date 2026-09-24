#!/bin/sh
# BulumOS log rotation: gzip old syslog, keep last one
LOG=/var/log/messages
if [ ! -f "$LOG" ]; then
    exit 0
fi
if [ -f "$LOG.1.gz" ]; then
    rm -f "$LOG.1.gz"
fi
mv "$LOG" "$LOG.tmp"
: > "$LOG"
gzip -9 "$LOG.tmp"
mv "$LOG.tmp.gz" "$LOG.1.gz" 2>/dev/null || mv "$LOG.tmp" "$LOG.1"
sync > /dev/null 2>&1
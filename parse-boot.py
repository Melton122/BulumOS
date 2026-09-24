#!/usr/bin/env python3
import re, sys
data = open('/home/melton/myos/boot.log','rb').read().decode('utf-8','replace')
data = re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',data)
data = re.sub(r'\x1b\][^\x07]*\x07','',data)
for line in data.split('\r'):
    line = line.strip('\n')
    if line.startswith('  [') or 'ready' in line or 'hotplug' in line or 'devpts' in line:
        print(line)

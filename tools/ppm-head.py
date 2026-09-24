#!/usr/bin/env python3
import re, sys
data = open(sys.argv[1], "rb").read()
print("head:", data[:20])
m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)", data)
print("dims:", m.groups())
a = m.end()
print("data offset:", a)
print("w*h*3:", int(m.group(1)) * int(m.group(2)) * 3)
print("avail after header:", len(data) - a)
print("byte right after header:", data[a:a + 12])
print("last 4 bytes:", data[-4:])
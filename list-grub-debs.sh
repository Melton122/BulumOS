#!/bin/bash
curl -s http://ftp.debian.org/debian/pool/main/g/grub/ | grep -oE 'href="[^"]+"' | head -20

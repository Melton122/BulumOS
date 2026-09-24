#!/bin/bash
# debug key export steps
cd /home/melton/Bulum\ OS
K=tools/testrepo.key
[ -f $K ] || openssl genrsa -out $K 2048 2>/dev/null
TXT=$(openssl rsa -in $K -text -noout)
MOD=$(echo "$TXT" | awk 'BEGIN{IGNORECASE=1} /modulus:/{f=1;next} /exponent/{f=0} f{printf "%s",$0}' | tr -cd '0-9a-fA-F')
EXP=$(echo "$TXT" | awk '/publicExponent/{print $2; exit}')
echo "MOD chars: $(printf '%s' "$MOD" | wc -c) (want 512)"
echo "EXP: $EXP (want 65537)"
mkdir -p rootfs/etc/bulum/keys
{ echo "name=main"; echo "e=$EXP"; echo "n=$MOD"; } > rootfs/etc/bulum/keys/main.pub
head -c 120 rootfs/etc/bulum/keys/main.pub; echo
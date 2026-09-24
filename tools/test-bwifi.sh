#!/bin/bash
# BulumOS Phase 9.5: WiFi command layer tests (bnet wifi)
set -e
cd /home/melton/myos
BIN=/tmp/bnet-wifi-host
mkdir -p "$BIN"
gcc -O2 -Wall -o "$BIN/bnet" src/bnet.c || { echo "build failed"; exit 1; }
BNET="$BIN/bnet"
PASS=0 FAIL=0
ok() { [ "$DW" != 1 ] && echo "PASS  $1"; PASS=$((PASS+1)); }
no() { [ "$DW" != 1 ] && echo "FAIL  $1"; FAIL=$((FAIL+1)); }

export BNET_WPA_CONF=/tmp/bwifi-test.conf
WC=/tmp/bwifi-test.conf

echo "=== status / discovery (no wireless adapter) ==="
"$BNET" wifi >/dev/null 2>&1 && ok "bnet wifi exits 0 (no adapter reported)" || no "bnet wifi exits 0"
"$BNET" wifi status >/dev/null 2>&1 && ok "wifi status exits 0" || no "wifi status exits 0"
"$BNET" wifi 2>&1 | grep -q 'none - no wireless adapter' && ok "reports no wireless adapter" || no "reports no wireless adapter"
"$BNET" wifi 2>&1 | grep -qi 'wpa_supplicant' && ok "reports wpa_supplicant status" || no "reports wpa_supplicant status"

echo "=== connect: WPA2-PSK config ==="
rm -f "$WC"
BNET_WLAN=wlan0 "$BNET" wifi connect CafeNet hunter2host >/dev/null 2>&1 && ok "connect WPA2 exits 0" || no "connect WPA2 exits 0"
grep -q 'ssid="CafeNet"' "$WC" && ok "conf has ssid" || no "conf has ssid"
grep -q 'psk="hunter2host"' "$WC" && ok "conf has psk" || no "conf has psk"
grep -q 'key_mgmt=NONE' "$WC" && no "WPA2 conf should NOT set key_mgmt=NONE" || ok "WPA2 conf omits key_mgmt=NONE"
grep -q 'update_config=1' "$WC" && ok "conf has update_config" || no "conf has update_config"
grep -q 'ctrl_interface' "$WC" && ok "conf has ctrl_interface" || no "conf has ctrl_interface"

echo "=== connect: open network ==="
rm -f "$WC"
BNET_WLAN=wlan0 "$BNET" wifi connect OpenCoffee >/dev/null 2>&1 && ok "connect open exits 0" || no "connect open exits 0"
grep -q 'ssid="OpenCoffee"' "$WC" && ok "open conf has ssid" || no "open conf has ssid"
grep -q 'key_mgmt=NONE' "$WC" && ok "open conf sets key_mgmt=NONE" || no "open conf sets key_mgmt=NONE"
grep -q 'psk=' "$WC" && no "open conf should not have psk" || ok "open conf omits psk"

echo "=== connect: no wireless adapter ==="
rm -f "$WC"
"$BNET" wifi connect NoHwNet 2>&1 | grep -q 'no wireless interface' && ok "connect without adapter reports error" || no "connect without adapter reports error"

echo "=== argument / help handling ==="
"$BNET" wifi connect >/dev/null 2>&1 && no "connect with no ssid exits nonzero" || ok "connect with no ssid exits nonzero"
"$BNET" wifi bogus >/dev/null 2>&1 && no "unknown wifi subcmd exits nonzero" || ok "unknown wifi subcmd exits nonzero"
"$BNET" --help 2>&1 | grep -q 'wifi connect' && ok "help documents wifi connect" || no "help documents wifi connect"
"$BNET" --help 2>&1 | grep -q 'wifi scan' && ok "help documents wifi scan" || no "help documents wifi scan"

echo "=== scan without wpa_supplicant ==="
BNET_WLAN=wlan0 "$BNET" wifi scan 2>&1 | grep -qi 'not installed\|scan' && ok "scan handles missing tools gracefully" || no "scan handles missing tools gracefully"

echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]

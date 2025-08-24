#!/bin/sh
# sta_connect.sh -- Connect MTK ApCli as STA (prefers 5 GHz if available)
# Usage:
#   sta_connect.sh "<SSID>" "<PASS>" [--bssid aa:bb:cc:dd:ee:ff] [--band 5|2] [--mixed]
# Examples:
#   sta_connect.sh "Trollvinter" "Mayonaise" --band 5
#   sta_connect.sh "MyAP" "secret" --bssid 70:3a:cb:f3:11:91
#   sta_connect.sh "Legacy" "pass" --mixed    # WPA1/2 + TKIP/AES

set -e

SSID="$1"
PSK="$2"
shift 2 || true

BSSID=""
BAND="5"            # prefer 5 GHz by default
MIXED=0             # WPA1WPA2 + TKIPAES if set

while [ $# -gt 0 ]; do
  case "$1" in
    --bssid) BSSID="$2"; shift 2 ;;
    --band)  BAND="$2";  shift 2 ;;
    --mixed) MIXED=1;    shift 1 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

[ -n "$SSID" ] || { echo "SSID required"; exit 2; }
[ -n "$PSK"  ] || { echo "Password required"; exit 2; }

# Pick STA iface (5 GHz first)
IF_5G=""
IF_24G=""
ip link show apclii0 >/dev/null 2>&1 && IF_5G="apclii0"
ip link show apcli0   >/dev/null 2>&1 && IF_24G="apcli0"

IF=""
if [ "$BAND" = "5" ] && [ -n "$IF_5G" ]; then
  IF="$IF_5G"
elif [ "$BAND" = "2" ] && [ -n "$IF_24G" ]; then
  IF="$IF_24G"
elif [ -n "$IF_5G" ]; then
  IF="$IF_5G"
elif [ -n "$IF_24G" ]; then
  IF="$IF_24G"
else
  echo "No ApCli interface found (apclii0/apcli0)." >&2
  exit 3
fi

echo "[sta] Using STA interface: $IF  (band pref=$BAND)"

# Some firmwares dislike quoted values; use unquoted sets.
# Reset + config
ifconfig "$IF" down 2>/dev/null || true
iwpriv "$IF" set ApCliEnable=0

if [ $MIXED -eq 1 ]; then
  iwpriv "$IF" set ApCliAuthMode=WPA1WPA2
  iwpriv "$IF" set ApCliEncrypType=TKIPAES
else
  iwpriv "$IF" set ApCliAuthMode=WPA2PSK
  iwpriv "$IF" set ApCliEncrypType=AES
fi

# SSID/PSK (unquoted; MTK parser reads up to EOL)
iwpriv "$IF" set ApCliSsid=$SSID
iwpriv "$IF" set ApCliWPAPSK=$PSK

# Optionally lock to a BSSID
if [ -n "$BSSID" ]; then
  iwpriv "$IF" set ApCliBssid=$BSSID
fi

# Bring up & enable; this triggers association on most builds
ifconfig "$IF" up
iwpriv "$IF" set ApCliEnable=1

# Give firmware time to associate
sleep 2

# DHCP (optional; comment out if you do static addressing)
if command -v udhcpc >/dev/null 2>&1; then
  udhcpc -i "$IF" -n -q -t 3 || true
fi

echo "[sta] Link status:"
iwpriv "$IF" stat | sed -n 's/.*Last TX Rate[[:space:]]*=[[:space:]]*//p; s/.*Last RX Rate[[:space:]]*=[[:space:]]*//p; s/^RSSI.*/&/p' | head -n 3

# Print IP if obtained
IP_NOW="$(ip -4 -o addr show "$IF" 2>/dev/null | awk '{print $4}' | cut -d/ -f1)"
[ -n "$IP_NOW" ] && echo "[sta] IP: $IP_NOW"

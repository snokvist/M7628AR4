#!/bin/sh
# usage: wifi-ssid on|off <ssid-or-section>
act="$1"; key="$2"
[ -n "$act" ] && [ -n "$key" ] || { echo "usage: wifi-ssid on|off <ssid-or-section>"; exit 2; }

# 1) exact section name?
if uci -q get "wireless.$key" >/dev/null 2>&1; then
  sec="$key"
else
  # 2) find by SSID exact match
  sec="$(uci -q show wireless | awk -F'[.=]' -v k="$key" '
    $3=="ssid" {
      m=index($0,"=");
      v=substr($0,m+1); gsub(/^'\''|'\''$/,"",v);
      if (v==k) {print $2; exit}
    }')"
fi

[ -n "$sec" ] || { echo "No matching wifi-iface for '$key'"; exit 1; }

case "$act" in
  on)  uci set "wireless.$sec.disabled=0" ;;
  off) uci set "wireless.$sec.disabled=1" ;;
  *)   echo "unknown action: $act"; exit 2 ;;
esac

uci commit wireless
wifi reload

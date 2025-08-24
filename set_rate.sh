#!/bin/sh
# set_rate.sh <if> <mcs> [bw_mhz] [sgi] [wcid]
#   <if>      : ra0 | apcli0 | ...
#   <mcs>     : HT 0..32
#   [bw_mhz]  : 20 | 40        (default 20)
#   [sgi]     : 0=LGI | 1=SGI  (default 0)
#   [wcid]    : peer index     (default 1)
#
# Uses Mediatek FixedRate tuple:
#   FixedRate=[WCID]-[Mode]-[BW]-[MCS]-[VhtNss]-[SGI]-[Preamble]-[STBC]-[LDPC]-[SPE_EN]
#     Mode=2 (HT), BW:0=20/1=40, VhtNss=0 for HT, SGI:0/1, Preamble=1

set -e

err(){ echo "[set_rate] ERROR: $*" >&2; exit 1; }
usage(){ echo "Usage: $0 <if> <mcs 0..32> [20|40] [sgi 0|1] [wcid]" >&2; exit 1; }

IF="$1"; MCS="$2"; BWmhz="${3:-20}"; SGI="${4:-0}"; WCID="${5:-1}"
[ -n "$IF" ] && [ -n "$MCS" ] || usage
case "$MCS" in *[!0-9]*|"") err "invalid MCS '$MCS'";; esac
[ "$MCS" -le 32 ] || err "HT MCS must be 0..32"

# BW map
case "$BWmhz" in
  40) BW=1 ;;
  20|"") BW=0; BWmhz=20 ;;
  *) echo "[set_rate] WARN: unsupported bw '$BWmhz', using 20"; BW=0; BWmhz=20 ;;
esac

# SGI
case "$SGI" in 0|1) ;; *) err "sgi must be 0 or 1";; esac

MODE=2      # HT
VHTNSS=0    # not used in HT
PREAMBLE=1
STBC=0
LDPC=0
SPE=0

TUPLE="$WCID-$MODE-$BW-$MCS-$VHTNSS-$SGI-$PREAMBLE-$STBC-$LDPC-$SPE"

echo "[set_rate] iface=$IF mode=ht mcs=$MCS bw=${BWmhz}MHz sgi=$SGI wcid=$WCID"
echo "[set_rate] applying FixedRate=$TUPLE"
if ! iwpriv "$IF" set FixedRate="$TUPLE" >/dev/null 2>&1; then
  err "iwpriv rejected FixedRate on $IF (tuple=$TUPLE)"
fi

# Give firmware time to commit (some SDKs need ~1s)
sleep 1

# Verify
STAT_OUT="$(iwpriv "$IF" stat 2>/dev/null || true)"
TX_LINE="$(echo "$STAT_OUT" | grep -m1 'Last TX Rate' | sed 's/.*Last TX Rate[[:space:]]*=[[:space:]]*//')"
RX_LINE="$(echo "$STAT_OUT" | grep -m1 'Last RX Rate' | sed 's/.*Last RX Rate[[:space:]]*=[[:space:]]*//')"
RSSI_LINE="$(echo "$STAT_OUT" | grep -m1 '^RSSI' | sed 's/^[^=]*=[[:space:]]*//')"

[ -n "$TX_LINE" ] || TX_LINE="(n/a)"
[ -n "$RX_LINE" ] || RX_LINE="(n/a)"
[ -n "$RSSI_LINE" ] || RSSI_LINE="(n/a)"

echo "[verify] applied tuple: $TUPLE"
echo "[verify] TX=$TX_LINE"
echo "[verify] RX=$RX_LINE"
echo "[verify] RSSI=$RSSI_LINE"

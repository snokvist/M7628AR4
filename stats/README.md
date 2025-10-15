# OpenWrt Wireless Diagnostics Cheatsheet

## Environment Basics
- Router: `ssh root@192.168.2.1`
- Radios: `phy0-sta0` (2.4 GHz client, SSID `Trollvinter`), `phy1-sta0` (5 GHz client, SSID `Drone`)
- DebugFS is mounted at `/sys/kernel/debug`

## Interactive CLI Snapshots
- Link summary (quality, RSSI, SSID):
  ```sh
  iwinfo phy1-sta0 info
  iwinfo phy0-sta0 info
  ```
- Per-station counters (RSSI history, retries, throughput):
  ```sh
  iw dev phy1-sta0 station dump
  iw dev phy0-sta0 station dump
  ```
- Channel airtime and noise survey:
  ```sh
  iw dev phy1-sta0 survey dump
  iw dev phy0-sta0 survey dump
  ```

Example (captured 2025-02-14):
```
Station 98:03:cf:cf:a4:28 (on phy1-sta0)
    tx retries: 84
    signal: -14 [-16, -17] dBm
    signal avg: -14 [-16, -17] dBm
```

## JSON via `ubus`
All of the above data is also exposed over `ubus`, which is faster to parse and script.

- Current link metadata (SSID, RSSI, noise):
  ```sh
  ubus call iwinfo info '{"device":"phy1-sta0"}'
  ```
- Associated station stats (signal, retries, bytes):
  ```sh
  ubus call iwinfo assoclist '{"device":"phy1-sta0"}'
  ubus call iwinfo assoclist '{"device":"phy0-sta0"}'
  ```
- Noise floor / airtime per channel:
  ```sh
  ubus call iwinfo survey '{"device":"phy1-sta0"}'
  ```

### Quick field extraction with `jsonfilter`
`jsonfilter` is included on OpenWrt. Examples verified on the router:

- Grab RSSI and noise:
  ```sh
  ubus call iwinfo assoclist '{"device":"phy1-sta0"}' \
    | jsonfilter -l 1 -e '@.results[0].signal' -e '@.results[0].noise'
  # -> -14 (signal), -90 (noise)
  ```
- Grab retries, failed frames, and RX packets:
  ```sh
  ubus call iwinfo assoclist '{"device":"phy1-sta0"}' \
    | jsonfilter -l 1 -e '@.results[0].tx.retries' \
                        -e '@.results[0].tx.failed' \
                        -e '@.results[0].rx.packets'
  # -> 84, 0, 1194584
  ```
- Derive SNR in shell (noise may be `0` on some radios; fall back to survey data if so):
  ```sh
  read signal noise <<"EOF"
  $(ubus call iwinfo assoclist '{"device":"phy1-sta0"}' \
     | jsonfilter -l 1 -e '@.results[0].signal' -e '@.results[0].noise')
  EOF
  snr=$((signal - noise))
  echo "SNR: ${snr} dB"
  ```

## Driver DebugFS Data
Low-level counters live under `/sys/kernel/debug/ieee80211`. Paths below were confirmed on the router.

### mac80211 statistics
- Global 802.11 counters (5 GHz radio only; 2.4 GHz reports "Not supported"):
  ```sh
  for f in dot11ACKFailureCount dot11FCSErrorCount dot11RTSFailureCount dot11RTSSuccessCount; do
    printf "%s: " "$f"
    cat /sys/kernel/debug/ieee80211/phy1/statistics/$f
  done
  # -> dot11ACKFailureCount: 100, dot11FCSErrorCount: 2528, dot11RTSFailureCount: 2, dot11RTSSuccessCount: 8
  ```

### Per-station driver metrics
Each associated peer has its own directory:
```
/sys/kernel/debug/ieee80211/phy1/netdev:phy1-sta0/stations/98:03:cf:cf:a4:28/
```
Useful files:
- `aqm` – queue backlog and drops per TID (drops/marks highlight congestion)
  ```sh
  cat /sys/kernel/debug/ieee80211/phy1/netdev:phy1-sta0/stations/98:03:cf:cf:a4:28/aqm
  ```
- `airtime` – RX/TX airtime totals (`cat .../airtime`)
- `rc_stats` / `rc_stats_csv` – per-MCS success/attempt counts
  ```sh
  head -n 20 /sys/kernel/debug/ieee80211/phy1/netdev:phy1-sta0/stations/98:03:cf:cf:a4:28/rc_stats
  ```
- `agg_status` – AMPDU window {dialog tokens, pending}
- `tx_filtered` – frames dropped after aggregation

### MT76 driver-specific counters
Located under `/sys/kernel/debug/ieee80211/phyX/mt76/` (available on both radios).
- AMPDU distribution and BlockAck misses:
  ```sh
  cat /sys/kernel/debug/ieee80211/phy1/mt76/ampdu_stat
  # -> shows AMPDU length histogram, BA miss count: 95, PER: 0.0%
  ```
- Radio sensitivity and false CCA counts:
  ```sh
  cat /sys/kernel/debug/ieee80211/phy1/mt76/radio
  ```
- Queue depth snapshots:
  ```sh
  head -n 40 /sys/kernel/debug/ieee80211/phy1/mt76/xmit-queues
  head -n 40 /sys/kernel/debug/ieee80211/phy1/mt76/rx-queues
  ```

## Spotting Unstable Links
Use the metrics below to identify a link that is unstable or about to degrade. Commands are verified above; combine them as needed.

| Symptom | Evidence | Command(s) |
| --- | --- | --- |
| RSSI / SNR dropping | Rapid falls in `signal` or growing gap between `signal` and `noise` | `ubus call iwinfo assoclist '{"device":"phy1-sta0"}' \| jsonfilter ...`; `iw dev phyX-staY station dump` |
| Rising retries / failed TX | `tx.retries`, `tx.failed`, `dot11ACKFailureCount` climbing faster than RX packet growth | `ubus call iwinfo assoclist ...`; mac80211 `dot11*` counters; `iw dev ... station dump` |
| AMPDU reordering issues | `BA miss count` rising, `agg_status` showing many pending entries | `cat /sys/kernel/debug/ieee80211/phy1/mt76/ampdu_stat`; `cat .../agg_status` |
| Queue congestion | Non-zero `drops`, `overlimit`, or large backlog in `aqm`; high `busy_time` in surveys | `cat .../aqm`; `ubus call iwinfo survey ...` |
| Noise / interference | `busy_time` and `rx_time` in surveys or rising false CCA counts | `ubus call iwinfo survey ...`; `cat /sys/kernel/debug/ieee80211/phy1/mt76/radio` |

### Suggested Polling Loops
- Continuous RSSI/SNR check every 5 seconds:
  ```sh
  while sleep 5; do
    ubus call iwinfo assoclist '{"device":"phy1-sta0"}' \
      | jsonfilter -l 1 -e '@.results[0].signal' -e '@.results[0].noise'
  done
  ```
- Retry rate vs throughput (5 GHz radio):
  ```sh
  while sleep 10; do
    ubus call iwinfo assoclist '{"device":"phy1-sta0"}' \
      | jsonfilter -l 1 -e '@.results[0].tx.retries' -e '@.results[0].tx.packets'
  done
  ```
  A sharp rise in the ratio `retries / packets` (>5-8%) signals trouble.
- Track BlockAck misses:
  ```sh
  while sleep 5; do
    cat /sys/kernel/debug/ieee80211/phy1/mt76/ampdu_stat | grep "BA miss"
  done
  ```
- Monitor channel congestion alongside retries:
  ```sh
  # Identify the index of the active channel (left column is the array index)
  ubus call iwinfo survey '{"device":"phy1-sta0"}' \
    | jsonfilter -e '@.results[*].mhz' \
    | awk '{printf "%02d %s\n", NR-1, $0}'

  # Once you know the index (e.g. 24 for 5825 MHz), pull busy and RX airtime
  while sleep 10; do
    ubus call iwinfo survey '{"device":"phy1-sta0"}' \
      | jsonfilter -l 1 -e '@.results[24].busy_time' -e '@.results[24].rx_time'
  done
  ```

## Forcing TX Rate (MCS Locks)
- Lock 5 GHz traffic (phy1) to HT/VHT MCS 2. The VHT mask prevents 802.11ac from selecting higher rates when the AP supports them.
  ```sh
  sshpass -p 'Mayonaise' ssh -o StrictHostKeyChecking=no root@192.168.2.1 \
    "iw dev phy1-sta0 set bitrates ht-mcs-5 2 vht-mcs-5 1:2"
  ```
- Lock 2.4 GHz traffic (phy0) to MCS 2 (legacy-N only on this radio, so HT mask is enough):
  ```sh
  sshpass -p 'Mayonaise' ssh -o StrictHostKeyChecking=no root@192.168.2.1 \
    "iw dev phy0-sta0 set bitrates ht-mcs-2.4 2"
  ```
- Clear either lock with an empty `set bitrates` invocation:
  ```sh
  sshpass -p 'Mayonaise' ssh -o StrictHostKeyChecking=no root@192.168.2.1 \
    "iw dev phy1-sta0 set bitrates"
  ```
- Confirm the negotiated rate after locking:
  ```sh
  sshpass -p 'Mayonaise' ssh -o StrictHostKeyChecking=no root@192.168.2.1 \
    "iw dev phy1-sta0 link"
  ```
- Locks disappear after `wifi reload` or reboots; sample hotplug scripts for reapplying the settings live in `docs/mcs-startup-scripts.md`.
- Current setup: `/etc/hotplug.d/iface/99-force-mcs-phy1` is installed on the router and logs `user.notice force-mcs` when it reapplies the mask during `ifup drone_5g`. Validate with:
  ```sh
  sshpass -p 'Mayonaise' ssh -o StrictHostKeyChecking=no root@192.168.2.1 \
    "logread | grep force-mcs | tail -1"
  sshpass -p 'Mayonaise' ssh -o StrictHostKeyChecking=no root@192.168.2.1 \
    "iw dev phy1-sta0 link"
  ```

## Automation Hints
- Lightweight logger (append RSSI, noise, retries):
  ```sh
  while sleep 10; do
    read signal noise retries <<"EOF"
    $(ubus call iwinfo assoclist '{"device":"phy1-sta0"}' \
       | jsonfilter -l 1 -e '@.results[0].signal' -e '@.results[0].noise' -e '@.results[0].tx.retries')
    EOF
    printf '%(%Y-%m-%dT%H:%M:%S%z)T %s %s %s\n' -1 "$signal" "$noise" "$retries" >> /tmp/phy1_rssi.log
  done
  ```
- Combine 2.4 GHz metrics with the same commands by swapping `phy1-sta0` for `phy0-sta0`. Remember that the MT7628 driver reports `noise: 0`; rely on survey data or external spectrum checks for SNR on that radio.

## UDP Link Telemetry Pipeline

- Build the sender (`wifi_metrics_sender.c`) and receiver (`osd_feed.c`):
  ```sh
  gcc -Wall -Wextra -std=c11 wifi_metrics_sender.c -o wifi_metrics_sender
  gcc -Wall -Wextra -std=c11 osd_feed.c -o osd_feed
  ```
- For OpenWrt targets use the staged cross toolchain:
  ```sh
  /home/snokvist/dev/openwrt/staging_dir/toolchain-mipsel_24kc_gcc-14.3.0_musl/bin/mipsel-openwrt-linux-musl-gcc \
      -O2 -pipe -mno-branch-likely -mips32r2 -EL -std=c11 wifi_metrics_sender.c -o wifi_metrics_sender
  /home/snokvist/dev/openwrt/staging_dir/toolchain-mipsel_24kc_gcc-14.3.0_musl/bin/mipsel-openwrt-linux-musl-gcc \
      -O2 -pipe -mno-branch-likely -mips32r2 -EL -std=c11 osd_feed.c -o osd_feed
  ```
- On the router run the sender, locking to the live peer and pointing at the OSD host:
- On the router run the sender, locking to the live peer and pointing at the OSD host:
  ```sh
  ./wifi_metrics_sender -m 98:03:cf:cf:a4:28 -H 192.168.2.20 -i 250 -v
  # or discover peers first
  ./wifi_metrics_sender -L
  ```
  The sender polls `iw dev <iface> station get <MAC>` plus `/sys/kernel/debug/ieee80211/<phy>/statistics`, normalises RSSI, derives TX/RX health scores (smoothed with an EMA), and emits JSON:
  ```
  {
    "rssi": <0-100>,
    "link": <alias of link_all>,
    "link_tx": <0-100>,
    "link_rx": <0-100>,
    "link_all": <0-100>,
    "text": ["RSSI","Link TX","Link RX","Link ALL"],
    "value": [<rssi>,<link_tx>,<link_rx>,<link_all>],
    "raw": {
      "signal": dBm,
      "tx_retry_ratio": …,
      "tx_retry_rate": …/s,
      "tx_fail_rate": …/s,
      "tx_beacon_rate": …/s,
      "tx_packet_rate": …/s,
      "rx_retry_ratio": …,
      "rx_retry_rate": …/s,
      "rx_drop_rate": …/s
    }
  }
  ```
- Run `osd_feed` on the host to bridge the UDP payload into the UNIX socket (`/run/pixelpilot/osd.sock` by default). It keeps the latest RSSI/Link, publishes `text/value` updates at ~1 Hz even when the UDP feed stalls, and reconnects to the socket if needed.
- For quick sanity checks use verbose mode on the sender (shows refresh Hz and raw rates) and watch the receiver logs—both should show two entries (`RSSI`, `Link`) with the same update counter/Hz.

All commands above were executed against the router (kernel 6.6.102, OpenWrt build 2025-08-28) and produced the noted sample values.

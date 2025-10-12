# MCS Lock Startup Scripts

These hotplug scripts reapply the `iw … set bitrates` locks whenever the given
client interface finishes coming up. Drop each snippet into
`/etc/hotplug.d/iface/`, mark it executable (`chmod +x`), and adjust the
filename to match your ordering preferences.

## Deployment Notes
- The production router currently uses the phy1 script below. It emits a `user.notice force-mcs`
  line in `logread` every time it runs, which is useful for confirming that the lock was reapplied
  after `ifup drone_5g` or `wifi reload`.
- Use `ifdown drone_5g && ifup drone_5g` followed by `iw dev phy1-sta0 link` to verify the negotiated
  TX rate drops to MCS2 (`21.7 MBit/s` with short GI on this hardware).

### `/etc/hotplug.d/iface/99-force-mcs-phy1`
```sh
#!/bin/sh
# Lock phy1-sta0 (5 GHz) to HT/VHT MCS 2 on interface bring-up. Applies when either the device
# or logical interface (`drone_5g`) comes up.

TARGET_DEV=phy1-sta0
TARGET_IF=drone_5g

[ "$ACTION" = "ifup" ] || exit 0
case "$DEVICE" in
  "$TARGET_DEV") match=1 ;;
esac
case "$INTERFACE" in
  "$TARGET_IF") match=1 ;;
esac
[ -n "$match" ] || exit 0
command -v iw >/dev/null 2>&1 || exit 0

logger -t force-mcs "Applying MCS2 lock on $TARGET_DEV (ACTION=$ACTION DEVICE=$DEVICE INTERFACE=$INTERFACE)"
iw dev $TARGET_DEV set bitrates ht-mcs-5 2 vht-mcs-5 1:2
```

### `/etc/hotplug.d/iface/99-force-mcs-phy0`
```sh
#!/bin/sh
# Lock phy0-sta0 (2.4 GHz) to HT MCS 2 on interface bring-up.

[ "$ACTION" = "ifup" ] || exit 0
[ "$DEVICE" = "phy0-sta0" ] || exit 0
command -v iw >/dev/null 2>&1 || exit 0

iw dev phy0-sta0 set bitrates ht-mcs-2.4 2
```

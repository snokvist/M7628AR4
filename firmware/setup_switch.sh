#!/bin/sh

# Only run on first boot / if no LAN set yet
uci -q get network.lan && exit 0

uci -q batch <<'EOF'
delete network.lan
set network.lan=interface
set network.lan.proto='static'
set network.lan.ipaddr='192.168.1.1'
set network.lan.netmask='255.255.255.0'
set network.lan.device='eth0.1'

delete network.wan
set network.wan=interface
set network.wan.proto='dhcp'
set network.wan.device='eth0.2'

delete network.switch
set network.switch='switch'
set network.switch.name='switch0'
set network.switch.reset='1'
set network.switch.enable_vlan='1'

delete network.vlan1
set network.vlan1='switch_vlan'
set network.vlan1.device='switch0'
set network.vlan1.vlan='1'
set network.vlan1.ports='0 1 2 3 6t'   # LAN: ports 0-3 + CPU tagged

delete network.vlan2
set network.vlan2='switch_vlan'
set network.vlan2.device='switch0'
set network.vlan2.vlan='2'
set network.vlan2.ports='4 6t'         # WAN: port 4 + CPU tagged
commit network
EOF

# No restart here; uci-defaults runs before services come up.
exit 0

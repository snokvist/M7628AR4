root@OpenWrt:~# cat /usr/bin/luci-o*
#!/bin/sh
/etc/init.d/uhttpd stop
/etc/init.d/rpcd stop
#!/bin/sh
/etc/init.d/rpcd start
/etc/init.d/uhttpd start
root@OpenWrt:~# 

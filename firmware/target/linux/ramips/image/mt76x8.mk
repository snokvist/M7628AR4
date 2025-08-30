define Device/custom_m7628ar4
  SOC := mt7628an
  DEVICE_DTS := mt7628an-m7628ar4
  IMAGE_SIZE := 16064k
  DEVICE_VENDOR := Custom
  DEVICE_MODEL := M7628AR4
  DEVICE_PACKAGES := \
        -luci-ssl \
        luci-light uhttpd uhttpd-mod-ubus \
        kmod-mt76 kmod-mt7603 kmod-mt7615e kmod-mt7663-firmware-ap \
        wpad-basic-mbedtls wireless-regdb \
        luci-app-package-manager \
        nano-tiny coreutils-timeout kmod-zram zram-swap kmod-lib-lz4 \
        wfb-ng wfb-ng-tun kmod-usb2 kmod-usb-ohci kmod-usb-ehci kmod-usb-storage block-mount bash
endef
TARGET_DEVICES += custom_m7628ar4




light version?

define Device/custom_m7628ar4
  SOC := mt7628an
  DEVICE_DTS := mt7628an-m7628ar4
  IMAGE_SIZE := 16064k
  DEVICE_VENDOR := Custom
  DEVICE_MODEL := M7628AR4
  DEVICE_PACKAGES := \
    luci-light uhttpd uhttpd-mod-ubus \
    kmod-mt7603 kmod-mt7615e kmod-mt7615-common kmod-mt76-core \
    wpad-basic-wolfssl wireless-regdb \
    kmod-zram zram-swap \
    -kmod-mt76 -kmod-mt76x02-common -kmod-mt76x2 -kmod-mt76x2-common \
    -ppp -ppp-mod-pppoe -luci-proto-ppp \
    -odhcpd-ipv6only -odhcp6c -luci-proto-ipv6
endef
TARGET_DEVICES += custom_m7628ar4

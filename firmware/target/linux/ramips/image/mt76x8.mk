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
        nano-tiny coreutils-timeout \
        kmod-zram zram-swap \
        wfb-ng wfb-ng-tun kmod-usb2 kmod-usb-ohci kmod-usb-ehci kmod-usb-storage block-mount bash
endef
TARGET_DEVICES += custom_m7628ar4

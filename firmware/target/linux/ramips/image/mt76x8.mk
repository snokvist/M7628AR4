define Device/custom_m7628ar4
  SOC := mt7628an
  IMAGE_SIZE := 16064k
  DEVICE_VENDOR := Custom
  DEVICE_MODEL := M7628AR4
  DEVICE_PACKAGES := \
        kmod-mt76 wpad-basic-mbedtls \
        kmod-mt7615e mt7663-firmware-ap mt7663-firmware-sta
endef
TARGET_DEVICES += custom_m7628ar4

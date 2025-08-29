define Device/custom_m7628ar4
  SOC := mt7628an
  DEVICE_DTS := mt7628an-m7628ar4   # <— tell it the real DTS filename
  IMAGE_SIZE := 16064k
  DEVICE_VENDOR := Custom
  DEVICE_MODEL := M7628AR4
  DEVICE_PACKAGES := \
        kmod-mt76 kmod-mt7615e mt7663-firmware-ap mt7663-firmware-sta \
        wpad-basic-mbedtls
endef
TARGET_DEVICES += custom_m7628ar4

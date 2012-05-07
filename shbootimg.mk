LOCAL_PATH := $(call my-dir)

INSTALLED_BOOTIMAGE_TARGET := $(PRODUCT_OUT)/boot.img

uncompressed_ramdisk := $(PRODUCT_OUT)/ramdisk.cpio
$(uncompressed_ramdisk): $(INSTALLED_RAMDISK_TARGET)
	zcat $< > $@
recovery_ramdisk := $(PRODUCT_OUT)/ramdisk-recovery.cpio

$(INSTALLED_BOOTIMAGE_TARGET): $(INSTALLED_KERNEL_TARGET) $(recovery_ramdisk) $(INSTALLED_RAMDISK_TARGET) ${uncompressed_ramdisk} ${recovery_uncompressed_ramdisk}
	$(call pretty,"Boot image: $@")
	$(hide) ./device/samsung/apollo/mkshbootimg.py $@ $(INSTALLED_KERNEL_TARGET) $(INSTALLED_RAMDISK_TARGET) $(recovery_ramdisk)

$(INSTALLED_RECOVERYIMAGE_TARGET): $(INSTALLED_BOOTIMAGE_TARGET)
	$(ACP) $(INSTALLED_BOOTIMAGE_TARGET) $@

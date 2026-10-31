include $(sort $(wildcard $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/package/*/*.mk))

# Cross-toolchain GDB helpers contain host paths and have no use on the board.
define ESP32S31_INK_LINUX_REMOVE_TARGET_DEBUG_FILES
	find $(TARGET_DIR) -name '*-gdb.py' -delete
endef
TARGET_FINALIZE_HOOKS += ESP32S31_INK_LINUX_REMOVE_TARGET_DEBUG_FILES

ifeq ($(BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT),y)
include $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/platform/esp32s31/linux.mk
endif

ifeq ($(BR2_PACKAGE_METALIO_EPD),y)
define METALIO_INSTALL_KERNEL_DRIVER
	mkdir -p $(@D)/drivers/misc/metalio
	cp $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/kernel/*.c $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/kernel/*.h \
		$(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/kernel/Kconfig $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/kernel/Makefile \
		$(@D)/drivers/misc/metalio/
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/include/uapi/linux/metalio_epd*.h \
		$(@D)/include/uapi/linux/
	grep -q 'source "drivers/misc/metalio/Kconfig"' $(@D)/drivers/misc/Kconfig || \
		printf '\nsource "drivers/misc/metalio/Kconfig"\n' >> $(@D)/drivers/misc/Kconfig
	grep -q 'CONFIG_METALIO_EPD.*metalio/' $(@D)/drivers/misc/Makefile || \
		printf '\nobj-$$(CONFIG_METALIO_EPD) += metalio/\n' >> $(@D)/drivers/misc/Makefile
endef
LINUX_POST_PATCH_HOOKS += METALIO_INSTALL_KERNEL_DRIVER

LINUX_POST_RSYNC_HOOKS += METALIO_INSTALL_KERNEL_DRIVER
endif

ifeq ($(BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT)$(BR2_PACKAGE_ESP32S31_LINUX_COMPANION),yy)
include $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/platform/esp32s31/companion.mk
endif

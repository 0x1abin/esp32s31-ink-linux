################################################################################
# ESP32-S31 CPU0 Linux companion; built separately from the Linux toolchain.
################################################################################

ESP32S31_LINUX_COMPANION_VERSION = 1
ESP32S31_LINUX_COMPANION_SITE = $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/firmware/esp32s31-linux-companion
ESP32S31_LINUX_COMPANION_SITE_METHOD = local
ESP32S31_LINUX_COMPANION_LICENSE = BSD-2-Clause, Apache-2.0
ESP32S31_LINUX_COMPANION_DEPENDENCIES = linux opensbi
ESP32S31_LINUX_COMPANION_INSTALL_TARGET = NO
ESP32S31_LINUX_COMPANION_INSTALL_IMAGES = YES

define ESP32S31_LINUX_COMPANION_BUILD_CMDS
	ESP32S31_INK_LINUX_PROJECT_DIR=$(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH) \
		$(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/scripts/build-wifi-firmware.sh $(@D) $(@D)/build \
		"$(call qstrip,$(BR2_PACKAGE_ESP32S31_LINUX_COMPANION_BOARD_DIR))"
endef

define ESP32S31_LINUX_COMPANION_INSTALL_IMAGES_CMDS
	$(INSTALL) -m 0644 $(@D)/build/bootloader/bootloader.bin $(BINARIES_DIR)/wifi-bootloader.bin
	$(INSTALL) -m 0644 $(@D)/build/partition_table/partition-table.bin $(BINARIES_DIR)/wifi-partition-table.bin
	$(INSTALL) -m 0644 $(@D)/build/esp32s31-linux-companion.bin $(BINARIES_DIR)/esp32s31-linux-companion.bin
	cp $(@D)/build/sdkconfig $(BINARIES_DIR)/wifi-sdkconfig
endef

$(eval $(generic-package))

ESP_LINUX_BSP_VERSION = a427cfbf5cccebd4f4e571db8a111ff1f5d31d71
ESP_LINUX_BSP_SITE = https://github.com/espressif/esp-linux-bsp.git
ESP_LINUX_BSP_SITE_METHOD = git
ESP_LINUX_BSP_INSTALL_TARGET = NO
HOST_ESP_LINUX_BSP_DEPENDENCIES = host-zstd

define HOST_ESP_LINUX_BSP_INSTALL_CMDS
	mkdir -p $(HOST_DIR)/share/esp-linux-bsp/tools $(HOST_DIR)/share/esp-linux-bsp/configs
	$(INSTALL) -m 0755 $(@D)/tools/*.sh $(HOST_DIR)/share/esp-linux-bsp/tools/
	$(INSTALL) -m 0644 $(@D)/configs/*.cfg $(HOST_DIR)/share/esp-linux-bsp/configs/
endef

$(eval $(host-generic-package))

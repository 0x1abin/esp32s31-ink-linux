METALIO_EPD_VERSION = 1
METALIO_EPD_SITE = $(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/apps
METALIO_EPD_SITE_METHOD = local
METALIO_EPD_DEPENDENCIES = libvterm
METALIO_EPD_LICENSE = MIT AND GPL-2.0-only
METALIO_EPD_LICENSE_FILES = LICENSES/MIT.txt LICENSES/GPL-2.0-only.txt

define METALIO_EPD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/include/uapi \
		$(@D)/epd-demo.c $(TARGET_LDFLAGS) -o $(@D)/metalio-epd-demo
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/include/uapi \
		$(@D)/terminal.c $(@D)/terminal-ui.c $(TARGET_LDFLAGS) -lvterm \
		-o $(@D)/metalio-terminal
endef

define METALIO_EPD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/metalio-epd-demo $(TARGET_DIR)/usr/bin/metalio-epd-demo
	$(INSTALL) -D -m 0755 $(@D)/metalio-terminal $(TARGET_DIR)/usr/bin/metalio-terminal
endef

$(eval $(generic-package))

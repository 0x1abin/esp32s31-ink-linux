LIBVTERM_VERSION = 0.3.3
LIBVTERM_SITE = https://www.leonerd.org.uk/code/libvterm
LIBVTERM_LICENSE = MIT
LIBVTERM_LICENSE_FILES = LICENSE
LIBVTERM_INSTALL_STAGING = YES

define LIBVTERM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c99 -fPIC -shared -I$(@D)/include \
		$(@D)/src/*.c $(TARGET_LDFLAGS) -Wl,-soname,libvterm.so.0 \
		-o $(@D)/libvterm.so.0
endef

define LIBVTERM_INSTALL_STAGING_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) PREFIX=/usr DESTDIR=$(STAGING_DIR) install-inc
	$(INSTALL) -D -m 0755 $(@D)/libvterm.so.0 $(STAGING_DIR)/usr/lib/libvterm.so.0
	ln -sf libvterm.so.0 $(STAGING_DIR)/usr/lib/libvterm.so
endef

define LIBVTERM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libvterm.so.0 $(TARGET_DIR)/usr/lib/libvterm.so.0
endef

$(eval $(generic-package))

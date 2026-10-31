################################################################################
#
# dropbear-client-only
#
################################################################################

DROPBEAR_CLIENT_ONLY_VERSION = 2026.94
DROPBEAR_CLIENT_ONLY_SITE = https://matt.ucc.asn.au/dropbear/releases
DROPBEAR_CLIENT_ONLY_SOURCE = dropbear-$(DROPBEAR_CLIENT_ONLY_VERSION).tar.bz2
DROPBEAR_CLIENT_ONLY_LICENSE = MIT, BSD-2-Clause, Public domain, Unlicense, WTFPL
DROPBEAR_CLIENT_ONLY_LICENSE_FILES = LICENSE libtommath/LICENSE libtomcrypt/LICENSE
DROPBEAR_CLIENT_ONLY_CONF_OPTS = --disable-harden --disable-zlib --enable-bundled-libtom
DROPBEAR_CLIENT_ONLY_MAKE_OPTS = PROGRAMS=dbclient

define DROPBEAR_CLIENT_ONLY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dbclient $(TARGET_DIR)/usr/bin/dbclient
	ln -sf dbclient $(TARGET_DIR)/usr/bin/ssh
endef

$(eval $(autotools-package))

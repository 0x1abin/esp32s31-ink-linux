# ESP32-S31 serial fixes also apply to an rsynced local kernel override.
define ESP32S31_PATCH_LOCAL_KERNEL
	$(BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH)/platform/esp32s31/patch-local-kernel.sh $(@D)
endef
LINUX_POST_RSYNC_HOOKS += ESP32S31_PATCH_LOCAL_KERNEL
LINUX_POST_PATCH_HOOKS += ESP32S31_PATCH_LOCAL_KERNEL

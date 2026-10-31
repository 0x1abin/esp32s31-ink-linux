# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_ESP32S31_HOSTED) += esp32s31_hosted.o
esp32s31_hosted-y := esp_shm.o esp_shm_flash.o esp_rcp.o main.o esp_cmd.o \
	esp_utils.o esp_cfg80211.o esp_stats.o esp_debugfs.o esp_log.o
esp32s31_hosted-$(CONFIG_BT) += esp_bt.o
ccflags-y += -I$(src) -I$(src)/include
CFLAGS_esp_log.o = -DDEBUG

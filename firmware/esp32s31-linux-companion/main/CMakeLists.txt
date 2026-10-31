# SPDX-License-Identifier: BSD-2-Clause
idf_component_register(SRCS main.c
    REQUIRES freertos esp_partition esp_psram esp_hw_support esp_rom hal esp_hal_wdt heap esp_hosted bootloader_support esp_mm
    INCLUDE_DIRS ".")

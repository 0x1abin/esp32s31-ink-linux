/* SPDX-License-Identifier: GPL-2.0-only */
/* The data window of the firmware as an MTD device. */
#ifndef _ESP_SHM_FLASH_H
#define _ESP_SHM_FLASH_H

#include <linux/device.h>
#include <linux/io.h>

struct esp_shm_flash;

struct esp_shm_flash *esp_shm_flash_probe(struct device *dev, void __iomem *shm,
					  void __iomem *doorbell);
void esp_shm_flash_wake(struct esp_shm_flash *flash);
void esp_shm_flash_remove(struct esp_shm_flash *flash);

#endif /* _ESP_SHM_FLASH_H */

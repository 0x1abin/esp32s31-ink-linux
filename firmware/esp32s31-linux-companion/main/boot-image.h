/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef METALIO_BOOT_IMAGE_H
#define METALIO_BOOT_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define METALIO_DTB_MAX_SIZE 0x10000U

static inline uint32_t metalio_boot_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static inline uint32_t metalio_dtb_size(const uint8_t header[8], size_t partition_size)
{
    uint32_t size = metalio_boot_be32(header + 4);

    if (metalio_boot_be32(header) != 0xD00DFEEDU || size < 40 ||
        size > METALIO_DTB_MAX_SIZE || size > partition_size)
        return 0;
    return size;
}

#endif

/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include "../firmware/esp32s31-linux-companion/main/boot-image.h"

int main(void)
{
    uint8_t header[] = {0xd0, 0x0d, 0xfe, 0xed, 0, 0, 0x10, 0};

    assert(metalio_dtb_size(header, 0x100000) == 4096);
    assert(!metalio_dtb_size(header, 4095));
    header[0] = 0;
    assert(!metalio_dtb_size(header, 0x100000));
    header[0] = 0xd0;
    header[6] = 0;
    header[7] = 39;
    assert(!metalio_dtb_size(header, 0x100000));
    header[5] = 1;
    header[6] = 0;
    header[7] = 0;
    assert(metalio_dtb_size(header, 0x100000) == METALIO_DTB_MAX_SIZE);
    header[7] = 1;
    assert(!metalio_dtb_size(header, 0x100000));
    return 0;
}

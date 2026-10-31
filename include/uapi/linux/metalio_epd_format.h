/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_METALIO_EPD_FORMAT_H
#define _UAPI_LINUX_METALIO_EPD_FORMAT_H

#define METALIO_EPD_WIDTH 1216U
#define METALIO_EPD_HEIGHT 684U
#define METALIO_EPD_STRIDE (METALIO_EPD_WIDTH / 8U)
#define METALIO_EPD_FRAME_BYTES (METALIO_EPD_STRIDE * METALIO_EPD_HEIGHT)
#define METALIO_EPD_ABI_VERSION 1U

/* Native landscape, row-major, most significant bit first: 1 black, 0 white. */
enum metalio_epd_mode {
	METALIO_EPD_DU = 0,
	METALIO_EPD_GC = 1,
	METALIO_EPD_FEEDBACK = 2,
};

#endif

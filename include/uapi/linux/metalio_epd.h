/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_METALIO_EPD_H
#define _UAPI_LINUX_METALIO_EPD_H

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/metalio_epd_format.h>

/* write() replaces a complete pending frame. One exclusive opener is allowed. */
struct metalio_epd_info {
	__u32 abi_version;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 frame_bytes;
	/* Zero on old kernels; DU and GC remain supported in that case. */
	__u32 supported_modes;
	__u32 reserved[2];
};

/* REFRESH starts asynchronously; write/REFRESH return EBUSY until it finishes. */
struct metalio_epd_refresh {
	__u32 mode;
	__u32 reserved[3];
};

/* poll() reports POLLIN on completion and POLLERR when last_error is nonzero. */
/* GET_STATUS acknowledges the completion. last_error is a negative errno. */
struct metalio_epd_status {
	__u32 sequence;
	__u32 busy;
	__s32 last_error;
	__u32 elapsed_ms;
};

#define METALIO_EPD_GET_INFO _IOR('M', 0, struct metalio_epd_info)
#define METALIO_EPD_REFRESH _IOW('M', 1, struct metalio_epd_refresh)
#define METALIO_EPD_GET_STATUS _IOR('M', 2, struct metalio_epd_status)

#endif

/* SPDX-License-Identifier: MIT */
/* Binary endpoints of E0470A14-AF MY_WAVEFORM; see LICENSES/Metalio-waveform.md. */
#ifndef METALIO_EPD_WAVEFORM_H
#define METALIO_EPD_WAVEFORM_H

/* Transition index: (old_black << 1) | new_black; actions: 0 hold, 1 black, 2 white. */
#if !defined(CONFIG_METALIO_EPD_FAST_REFRESH) || !CONFIG_METALIO_EPD_FAST_REFRESH
static const unsigned char metalio_epd_du_actions[20][4] = {
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 0 },
	{ 0, 1, 2, 1 },
	{ 0, 1, 2, 1 },
	{ 0, 0, 0, 1 },
	{ 0, 0, 0, 1 },
};
#endif

static const unsigned char metalio_epd_gc_actions[48][4] = {
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 1, 0, 0, 2 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 2, 1, 2, 1 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 }, /* The 48th source row is implicitly zero-initialized. */
};

#endif

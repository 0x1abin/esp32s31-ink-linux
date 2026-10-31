/* SPDX-License-Identifier: MIT */
#ifndef METALIO_EPD_PHASE_H
#define METALIO_EPD_PHASE_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stddef.h>
#include <string.h>
#endif
#include <linux/metalio_epd_format.h>
#include "metalio-epd-waveform.h"

#define METALIO_EPD_PHASE_STRIDE (METALIO_EPD_WIDTH / 4U + 1U)
#define METALIO_EPD_PHASE_ROWS ((METALIO_EPD_HEIGHT + 7U) & ~7U)
#define METALIO_EPD_PHASE_BYTES (METALIO_EPD_PHASE_STRIDE * METALIO_EPD_PHASE_ROWS)

static inline unsigned int metalio_epd_phase_count(unsigned int mode)
{
#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	if (mode == METALIO_EPD_DU)
		return 14U;
	if (mode == METALIO_EPD_FEEDBACK)
		return 8U;
#else
	if (mode == METALIO_EPD_DU)
		return sizeof(metalio_epd_du_actions) / sizeof(metalio_epd_du_actions[0]);
#endif
	if (mode == METALIO_EPD_GC)
		return sizeof(metalio_epd_gc_actions) / sizeof(metalio_epd_gc_actions[0]);
	return 0;
}

static inline unsigned int metalio_epd_supported_modes(void)
{
	unsigned int modes = (1U << METALIO_EPD_DU) | (1U << METALIO_EPD_GC);

#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	modes |= 1U << METALIO_EPD_FEEDBACK;
#endif
	return modes;
}

static inline unsigned int metalio_epd_phase_time_us(unsigned int mode,
						    unsigned int phase)
{
	return phase < metalio_epd_phase_count(mode) ? 12000U : 0U;
}

static inline const unsigned char *metalio_epd_phase_actions(unsigned int mode,
							   unsigned int phase)
{
#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	/* User-specified pulse budgets: DU 13 black/14 white, feedback 7 black/8 white. */
	static const unsigned char binary_drive[2][4] = {
		{ 0, 1, 2, 0 }, { 0, 0, 2, 0 },
	};
#endif

	if (phase >= metalio_epd_phase_count(mode))
		return NULL;
#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	if (mode != METALIO_EPD_GC)
		return binary_drive[phase + 1U == metalio_epd_phase_count(mode)];
#else
	if (mode == METALIO_EPD_DU)
		return metalio_epd_du_actions[phase];
#endif
	return metalio_epd_gc_actions[phase];
}

static inline int metalio_epd_phase_needs_encoding(unsigned int mode, unsigned int phase)
{
	const unsigned char *actions = metalio_epd_phase_actions(mode, phase);

	if (!actions)
		return 0;
#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	if (phase && mode != METALIO_EPD_GC)
		return memcmp(actions, metalio_epd_phase_actions(mode, phase - 1U), 4U) != 0;
#endif
	return 1;
}

static inline int metalio_epd_encode_uniform_phase(unsigned char *out,
						 size_t out_bytes, unsigned int drive)
{
	unsigned int y;

	if (!out || out_bytes < METALIO_EPD_PHASE_BYTES || drive > 2U)
		return -EINVAL;
	memset(out, 0, METALIO_EPD_PHASE_BYTES);
	for (y = 0; y < METALIO_EPD_HEIGHT; ++y)
		memset(out + y * METALIO_EPD_PHASE_STRIDE + 1U, drive * 0x55U,
		       METALIO_EPD_WIDTH / 4U);
	return 0;
}

/* out must be distinct from the exact-size native old/new 1 bpp frames. */
static inline int metalio_epd_encode_phase(unsigned char *out, size_t out_bytes,
		const unsigned char *old, const unsigned char *new,
		unsigned int mode, unsigned int phase, unsigned int border)
{
	unsigned char lut[256];
	const unsigned char *actions;
	unsigned int index, pixel, y, group;

	if (!out || !old || !new || out_bytes < METALIO_EPD_PHASE_BYTES ||
	    phase >= metalio_epd_phase_count(mode) || border % 4U ||
	    border >= METALIO_EPD_HEIGHT / 2U)
		return -EINVAL;
	actions = metalio_epd_phase_actions(mode, phase);
	for (index = 0; index < sizeof(lut); ++index) {
		unsigned char value = 0;

		for (pixel = 0; pixel < 4; ++pixel) {
			unsigned int old_black = (index >> (7U - pixel)) & 1U;
			unsigned int new_black = (index >> (3U - pixel)) & 1U;

			value |= actions[(old_black << 1) | new_black] << (6U - 2U * pixel);
		}
		lut[index] = value;
	}
	memset(out, 0, METALIO_EPD_PHASE_BYTES);
	for (y = border; y < METALIO_EPD_HEIGHT - border; ++y) {
		unsigned char *line = out + y * METALIO_EPD_PHASE_STRIDE + 1U;

		for (group = border / 4U; group < (METALIO_EPD_WIDTH - border) / 4U; ++group) {
			unsigned int offset = y * METALIO_EPD_STRIDE + group / 2U;
			unsigned int shift = group % 2U ? 0U : 4U;

			index = (((old[offset] >> shift) & 15U) << 4) |
				((new[offset] >> shift) & 15U);
			/* S31 emits x0 in bits 7:6; no little-endian word swapping. */
			line[group] = lut[index];
		}
	}
	return 0;
}

#endif

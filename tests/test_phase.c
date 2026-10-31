/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/metalio_epd.h>
#include "../kernel/metalio-epd-phase.h"
#include "../kernel/metalio-epd-regs.h"

_Static_assert(sizeof(struct metalio_epd_info) == 32, "GET_INFO ABI size");
_Static_assert(offsetof(struct metalio_epd_info, supported_modes) == 20, "former reserved slot");
_Static_assert(sizeof(struct metalio_epd_refresh) == 16, "REFRESH ABI size");
_Static_assert(sizeof(struct metalio_epd_status) == 16, "GET_STATUS ABI size");
_Static_assert(METALIO_EPD_ABI_VERSION == 1, "additive mode keeps ABI version");
_Static_assert(METALIO_EPD_GET_INFO == 0x80204d00U, "GET_INFO ioctl number");
_Static_assert(METALIO_EPD_REFRESH == 0x40104d01U, "REFRESH ioctl number");
_Static_assert(METALIO_EPD_GET_STATUS == 0x80104d02U, "GET_STATUS ioctl number");

/* Independent source endpoint oracle; fast tables include equal-color holds. */
#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
#define LAST_MODE METALIO_EPD_FEEDBACK
static const char * const expected_du[4] = {
	"00000000000000", "11111111111110", "22222222222222", "00000000000000",
};
static const char * const expected_feedback[4] = {
	"00000000", "11111110", "22222222", "00000000",
};
#else
#define LAST_MODE METALIO_EPD_GC
static const char * const expected_du[4] = {
	"00000000000000000000",
	"11111111111111111100",
	"22222222222222222200",
	"00000000000000001111",
};
#endif
static const char * const expected_gc[4] = {
	"000000000011111111111111111122222222222222222000",
	"000000000000000000000000000011111111111111111000",
	"000000000000000000000000000022222222222222222000",
	"000000000022222222222222222211111111111111111000",
};

static unsigned int expected(unsigned int mode, unsigned int phase,
			     unsigned int old, unsigned int new)
{
	const char * const *table = mode == METALIO_EPD_DU ? expected_du : expected_gc;

#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	if (mode == METALIO_EPD_FEEDBACK)
		table = expected_feedback;
#endif
	return table[(old << 1) | new][phase] - '0';
}

static unsigned int bit(const unsigned char *image, unsigned int x, unsigned int y)
{
	return (image[y * METALIO_EPD_STRIDE + x / 8U] >> (7U - x % 8U)) & 1U;
}

static void check(const unsigned char *encoded, const unsigned char *old,
		  const unsigned char *new, unsigned int mode, unsigned int phase,
		  unsigned int border)
{
	unsigned int x, y;

	assert(encoded[-1] == 0xa5);
	assert(encoded[METALIO_EPD_PHASE_BYTES] == 0xa5);
	for (y = 0; y < METALIO_EPD_PHASE_ROWS; ++y) {
		const unsigned char *line = encoded + y * METALIO_EPD_PHASE_STRIDE;

		assert(line[0] == 0); /* S31 LCD's dummy clock precedes active data. */
		for (x = 0; x < METALIO_EPD_WIDTH; ++x) {
			unsigned int drive = (line[1U + x / 4U] >> (6U - 2U * (x % 4U))) & 3U;
			unsigned int want = 0;

			if (y < METALIO_EPD_HEIGHT && x >= border &&
			    x < METALIO_EPD_WIDTH - border && y >= border &&
			    y < METALIO_EPD_HEIGHT - border)
				want = expected(mode, phase, bit(old, x, y), bit(new, x, y));
			assert(drive == want);
		}
	}
}

static void check_profile(void)
{
	assert(EPD_PCLK_MHZ == 20U);
	assert(EPD_LINE_CLOCKS == EPD_PCLK_MHZ * EPD_LINE_US);
	assert(EPD_CKV_HIGH_TICKS + EPD_CKV_LOW_TICKS == 10U * EPD_LINE_US);
	assert(EPD_LINE_US == 17U && EPD_LINE_CLOCKS == 340U);
	assert(EPD_CKV_HIGH_TICKS == 159U && EPD_CKV_LOW_TICKS == 11U);
	assert(EPD_CKV_LAUNCH_US == 15U);
	assert(EPD_LINE_US + EPD_CKV_LAUNCH_US == 32U);
	assert(metalio_epd_phase_count(METALIO_EPD_GC) == 48U);
#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	assert(metalio_epd_phase_count(METALIO_EPD_DU) == 14U);
	assert(metalio_epd_phase_count(METALIO_EPD_FEEDBACK) == 8U);
	assert(metalio_epd_supported_modes() == 7U);
#else
	assert(metalio_epd_phase_count(METALIO_EPD_DU) == 20U);
	assert(metalio_epd_phase_count(METALIO_EPD_FEEDBACK) == 0U);
	assert(metalio_epd_supported_modes() == 3U);
#endif
}

static void check_reuse(unsigned char *encoded, const unsigned char *old,
			const unsigned char *new, unsigned int mode)
{
	unsigned int phase, encoded_phases = 0;

	for (phase = 0; phase < metalio_epd_phase_count(mode); ++phase) {
		if (metalio_epd_phase_needs_encoding(mode, phase)) {
			assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES,
				old, new, mode, phase, 0) == 0);
			++encoded_phases;
		}
		check(encoded, old, new, mode, phase, 0);
	}
	assert(metalio_epd_phase_needs_encoding(mode, 0));
#if defined(CONFIG_METALIO_EPD_FAST_REFRESH) && CONFIG_METALIO_EPD_FAST_REFRESH
	assert(encoded_phases == (mode == METALIO_EPD_GC ? 48U : 2U));
#else
	assert(encoded_phases == metalio_epd_phase_count(mode));
#endif
	assert(!metalio_epd_phase_needs_encoding(mode, phase));
}

int main(void)
{
	unsigned char *old = malloc(METALIO_EPD_FRAME_BYTES);
	unsigned char *new = malloc(METALIO_EPD_FRAME_BYTES);
	unsigned char *storage = malloc(METALIO_EPD_PHASE_BYTES + 2U);
	unsigned char *encoded;
	unsigned int mode, phase, transition, i, drive;

	assert(old && new && storage);
	encoded = storage + 1;
	assert(METALIO_EPD_FRAME_BYTES == 103968U);
	assert(METALIO_EPD_PHASE_STRIDE == 305U);
	assert(METALIO_EPD_PHASE_ROWS == 688U);
	assert(METALIO_EPD_PHASE_BYTES == 209840U);
	check_profile();
	assert(metalio_epd_phase_count(3) == 0);
	assert(!metalio_epd_phase_needs_encoding(3, 0));
	for (mode = METALIO_EPD_DU; mode <= LAST_MODE; ++mode) {
		unsigned int phases = metalio_epd_phase_count(mode);

		for (transition = 0; transition < 4; ++transition) {
			memset(old, transition & 2U ? 255 : 0, METALIO_EPD_FRAME_BYTES);
			memset(new, transition & 1U ? 255 : 0, METALIO_EPD_FRAME_BYTES);
			for (phase = 0; phase < phases; ++phase) {
				memset(storage, 0xa5, METALIO_EPD_PHASE_BYTES + 2U);
				assert(metalio_epd_phase_time_us(mode, phase) == 12000U);
				assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES,
					old, new, mode, phase, 0) == 0);
				check(encoded, old, new, mode, phase, 0);
			}
		}
		assert(metalio_epd_phase_time_us(mode, phases) == 0);
	}
	/* Mixed pixels catch byte/nibble reversal and x/y traversal errors. */
	for (i = 0; i < METALIO_EPD_FRAME_BYTES; ++i) {
		old[i] = (i * 37U + i / 11U) & 255U;
		new[i] = (i * 71U + i / 7U) & 255U;
	}
	for (mode = METALIO_EPD_DU; mode <= LAST_MODE; ++mode)
		for (phase = 0; phase < metalio_epd_phase_count(mode); ++phase) {
			unsigned int border = phase % 2U ? 4 : 0;

			memset(storage, 0xa5, METALIO_EPD_PHASE_BYTES + 2U);
			assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES,
				old, new, mode, phase, border) == 0);
			check(encoded, old, new, mode, phase, border);
		}
	/* Replay complete scans while encoding only when actions change. */
	for (mode = METALIO_EPD_DU; mode <= LAST_MODE; ++mode)
		check_reuse(encoded, old, new, mode);
	/* Every new refresh must replace the preceding frame's reusable phase. */
	memset(old, 0, METALIO_EPD_FRAME_BYTES);
	memset(new, 255, METALIO_EPD_FRAME_BYTES);
	for (mode = METALIO_EPD_DU; mode <= LAST_MODE; ++mode)
		check_reuse(encoded, old, new, mode);
	/* DU phase 0 drives white->black only: x0 must occupy bits 7:6. */
	memset(old, 0, METALIO_EPD_FRAME_BYTES);
	memset(new, 0, METALIO_EPD_FRAME_BYTES);
	new[0] = 0x81;
	new[METALIO_EPD_FRAME_BYTES - 1U] = 1;
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES,
		old, new, METALIO_EPD_DU, 0, 0) == 0);
	assert(encoded[1] == 0x40 && encoded[2] == 0x01);
	assert(encoded[(METALIO_EPD_HEIGHT - 1U) * METALIO_EPD_PHASE_STRIDE + 304U] == 1);
	for (drive = 0; drive <= 2; ++drive) {
		memset(storage, 0xa5, METALIO_EPD_PHASE_BYTES + 2U);
		assert(metalio_epd_encode_uniform_phase(encoded,
			METALIO_EPD_PHASE_BYTES, drive) == 0);
		assert(encoded[-1] == 0xa5 && encoded[METALIO_EPD_PHASE_BYTES] == 0xa5);
		for (i = 0; i < METALIO_EPD_PHASE_BYTES; ++i)
			assert(encoded[i] == (i / METALIO_EPD_PHASE_STRIDE < METALIO_EPD_HEIGHT &&
				i % METALIO_EPD_PHASE_STRIDE ? drive * 0x55U : 0U));
	}
	memset(encoded, 0xa5, METALIO_EPD_PHASE_BYTES);
	assert(metalio_epd_encode_phase(NULL, METALIO_EPD_PHASE_BYTES, old, new, 0, 0, 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES - 1U, old, new, 0, 0, 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, NULL, new, 0, 0, 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, NULL, 0, 0, 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, new, 3, 0, 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, new,
		METALIO_EPD_DU, metalio_epd_phase_count(METALIO_EPD_DU), 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, new, 1, 48, 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, new,
		METALIO_EPD_FEEDBACK, metalio_epd_phase_count(METALIO_EPD_FEEDBACK), 0) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, new, 0, 0, 1) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, new, 0, 0, 344) == -EINVAL);
	assert(metalio_epd_encode_phase(encoded, METALIO_EPD_PHASE_BYTES, old, new, 0, 0, UINT_MAX) == -EINVAL);
	assert(metalio_epd_encode_uniform_phase(NULL, METALIO_EPD_PHASE_BYTES, 0) == -EINVAL);
	assert(metalio_epd_encode_uniform_phase(encoded, METALIO_EPD_PHASE_BYTES - 1U, 0) == -EINVAL);
	assert(metalio_epd_encode_uniform_phase(encoded, METALIO_EPD_PHASE_BYTES, 3) == -EINVAL);
	for (i = 0; i < METALIO_EPD_PHASE_BYTES; ++i)
		assert(encoded[i] == 0xa5); /* Invalid requests cannot partially overwrite a phase. */
	free(storage);
	free(new);
	free(old);
	puts("phase encoder: all binary transitions, ordering, padding, border and bounds passed");
	return 0;
}

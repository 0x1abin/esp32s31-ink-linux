/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef METALIO_FT6336U_CORE_H
#define METALIO_FT6336U_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

#define METALIO_TOUCH_REPORT_SIZE 13
#define METALIO_TOUCH_CONTACTS 2
#define METALIO_TOUCH_IDS 16

struct metalio_touch_point {
	unsigned short x, y;
	unsigned char id, event;
};

struct metalio_touch_report {
	unsigned char count;
	struct metalio_touch_point points[METALIO_TOUCH_CONTACTS];
};

struct metalio_touch_frame {
	unsigned char count;
	struct metalio_touch_point points[METALIO_TOUCH_CONTACTS];
	bool enter, confirm;
};

enum metalio_touch_region {
	METALIO_TOUCH_NONE,
	METALIO_TOUCH_SCREEN,
	METALIO_TOUCH_ENTER,
	METALIO_TOUCH_CONFIRM,
	METALIO_TOUCH_IGNORED,
};

struct metalio_touch_capture {
	enum metalio_touch_region region;
	unsigned short x, y;
};

struct metalio_touch_state {
	struct metalio_touch_capture capture[METALIO_TOUCH_IDS];
	bool barrier;
};

static inline bool metalio_touch_decode(const unsigned char *raw,
				       unsigned int length,
				       struct metalio_touch_report *report)
{
	struct metalio_touch_report decoded = { 0 };
	unsigned int i;

	if (!raw || !report || length != METALIO_TOUCH_REPORT_SIZE ||
	    (raw[0] & 15) > METALIO_TOUCH_CONTACTS)
		return false;
	decoded.count = raw[0] & 15;
	for (i = 0; i < decoded.count; i++) {
		const unsigned char *p = raw + 1 + i * 6;
		struct metalio_touch_point *point = &decoded.points[i];

		point->event = p[0] >> 6;
		if (point->event == 3)
			return false;
		point->x = ((p[0] & 15) << 8) | p[1];
		point->y = ((p[2] & 15) << 8) | p[3];
		point->id = p[2] >> 4;
	}
	if (decoded.count == 2 && decoded.points[0].id == decoded.points[1].id)
		return false;
	*report = decoded;
	return true;
}

static inline bool metalio_touch_map(unsigned short raw_x, unsigned short raw_y,
				    unsigned short *x, unsigned short *y)
{
	if (!x || !y || raw_x >= 1216 || raw_y >= 1216 ||
	    (raw_x >= 684 && raw_y >= 684))
		return false;
	if (raw_x < 684) {
		*x = raw_x;
		*y = raw_y;
	} else {
		*x = 683 - raw_y;
		*y = raw_x;
	}
	return true;
}

static inline bool metalio_touch_key(unsigned short x, unsigned short y)
{
	return y >= 1570 && y <= 1630 &&
		((x >= 170 && x <= 230) || (x >= 270 && x <= 330));
}

static inline bool metalio_touch_confirm_key(unsigned short x, unsigned short y)
{
	return y >= 1570 && y <= 1630 && x >= 70 && x <= 130;
}

static inline void metalio_touch_init(struct metalio_touch_state *state)
{
	*state = (struct metalio_touch_state){ .barrier = true };
}

static inline void metalio_touch_fail(struct metalio_touch_state *state,
				     struct metalio_touch_frame *frame)
{
	metalio_touch_init(state);
	*frame = (struct metalio_touch_frame){ 0 };
}

static inline void metalio_touch_update(struct metalio_touch_state *state,
				       const struct metalio_touch_report *report,
				       struct metalio_touch_frame *frame)
{
	unsigned int i, active = 0;

	*frame = (struct metalio_touch_frame){ 0 };
	for (i = 0; i < report->count; i++) {
		const struct metalio_touch_point *point = &report->points[i];
		struct metalio_touch_capture *capture = &state->capture[point->id];
		unsigned short x, y;
		bool on_screen;

		if (point->event == 1)
			continue;
		active |= 1U << point->id;
		if (state->barrier)
			continue;
		on_screen = metalio_touch_map(point->x, point->y, &x, &y);
		if (capture->region == METALIO_TOUCH_NONE) {
			if (metalio_touch_confirm_key(point->x, point->y))
				capture->region = METALIO_TOUCH_CONFIRM;
			else if (metalio_touch_key(point->x, point->y))
				capture->region = METALIO_TOUCH_ENTER;
			else if (on_screen)
				capture->region = METALIO_TOUCH_SCREEN;
			else
				capture->region = METALIO_TOUCH_IGNORED;
		}
		if (capture->region == METALIO_TOUCH_SCREEN && on_screen) {
			capture->x = x;
			capture->y = y;
		}
	}
	for (i = 0; i < METALIO_TOUCH_IDS; i++) {
		struct metalio_touch_capture *capture = &state->capture[i];

		if (!(active & (1U << i))) {
			*capture = (struct metalio_touch_capture){ 0 };
			continue;
		}
		if (capture->region == METALIO_TOUCH_ENTER)
			frame->enter = true;
		else if (capture->region == METALIO_TOUCH_CONFIRM)
			frame->confirm = true;
		else if (capture->region == METALIO_TOUCH_SCREEN)
			frame->points[frame->count++] = (struct metalio_touch_point){
				.x = capture->x, .y = capture->y, .id = i,
			};
	}
	if (!active)
		state->barrier = false;
}

#endif

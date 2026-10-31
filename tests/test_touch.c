/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/metalio-ft6336u-core.h"

static void encode_point(unsigned char raw[13], unsigned int index,
			 unsigned int id, unsigned int event,
			 unsigned int x, unsigned int y)
{
	unsigned char *p = raw + 1 + index * 6;

	p[0] = (event << 6) | ((x >> 8) & 15);
	p[1] = x;
	p[2] = (id << 4) | ((y >> 8) & 15);
	p[3] = y;
	p[4] = 0xa5;
	p[5] = 0x5a;
}

static struct metalio_touch_report report(unsigned int count,
		unsigned int id0, unsigned int event0, unsigned int x0, unsigned int y0,
		unsigned int id1, unsigned int event1, unsigned int x1, unsigned int y1)
{
	unsigned char raw[13] = { 0 };
	struct metalio_touch_report result;

	raw[0] = count;
	encode_point(raw, 0, id0, event0, x0, y0);
	encode_point(raw, 1, id1, event1, x1, y1);
	assert(metalio_touch_decode(raw, sizeof(raw), &result));
	return result;
}

static struct metalio_touch_report one(unsigned int id, unsigned int event,
				      unsigned int x, unsigned int y)
{
	return report(1, id, event, x, y, 0, 0, 0, 0);
}

static void check_decode(void)
{
	unsigned char raw[13] = { 0xf2 };
	struct metalio_touch_report parsed, before;
	unsigned int count, event;

	encode_point(raw, 0, 15, 0, 4095, 4095);
	encode_point(raw, 1, 3, 2, 684, 1215);
	assert(metalio_touch_decode(raw, sizeof(raw), &parsed));
	assert(parsed.count == 2);
	assert(parsed.points[0].id == 15 && parsed.points[0].event == 0);
	assert(parsed.points[0].x == 4095 && parsed.points[0].y == 4095);
	assert(parsed.points[1].id == 3 && parsed.points[1].event == 2);
	assert(parsed.points[1].x == 684 && parsed.points[1].y == 1215);
	for (event = 0; event < 3; ++event) {
		encode_point(raw, 0, 15, event, 17, 39);
		assert(metalio_touch_decode(raw, sizeof(raw), &parsed));
		assert(parsed.points[0].event == event);
	}
	memset(&before, 0xa7, sizeof(before));
	for (count = 3; count < 16; ++count) {
		raw[0] = count;
		parsed = before;
		assert(!metalio_touch_decode(raw, sizeof(raw), &parsed));
		assert(!memcmp(&parsed, &before, sizeof(parsed)));
	}
	raw[0] = 2;
	encode_point(raw, 0, 1, 0, 10, 20);
	encode_point(raw, 1, 1, 1, 20, 30);
	parsed = before;
	assert(!metalio_touch_decode(raw, sizeof(raw), &parsed));
	assert(!memcmp(&parsed, &before, sizeof(parsed)));
	encode_point(raw, 1, 2, 3, 20, 30);
	assert(!metalio_touch_decode(raw, sizeof(raw), &parsed));
	assert(!memcmp(&parsed, &before, sizeof(parsed)));
	encode_point(raw, 0, 1, 3, 10, 20);
	encode_point(raw, 1, 2, 0, 20, 30);
	assert(!metalio_touch_decode(raw, sizeof(raw), &parsed));
	assert(!memcmp(&parsed, &before, sizeof(parsed)));
	assert(!metalio_touch_decode(NULL, 13, &parsed));
	assert(!metalio_touch_decode(raw, 13, NULL));
	assert(!metalio_touch_decode(raw, 12, &parsed));
	assert(!metalio_touch_decode(raw, 14, &parsed));
	/* Empty reports may contain old point bytes, including reserved events. */
	raw[0] = 0xa0;
	memset(raw + 1, 0xff, 12);
	assert(metalio_touch_decode(raw, sizeof(raw), &parsed));
	assert(parsed.count == 0);
}

static void check_coordinates(void)
{
	unsigned int raw_x, raw_y;
	unsigned short x = 0, y = 0;

	for (raw_x = 0; raw_x < 1216; ++raw_x) {
		for (raw_y = 0; raw_y < 1216; ++raw_y) {
			bool valid = raw_x < 684 || raw_y < 684;

			assert(metalio_touch_map(raw_x, raw_y, &x, &y) == valid);
			if (valid) {
				assert(x == (raw_x < 684 ? raw_x : 683 - raw_y));
				assert(y == (raw_x < 684 ? raw_y : raw_x));
				assert(x < 684 && y < 1216);
			}
		}
	}
	assert(!metalio_touch_map(1216, 0, &x, &y));
	assert(!metalio_touch_map(0, 1216, &x, &y));
	assert(!metalio_touch_map(4095, 4095, &x, &y));
	assert(!metalio_touch_map(0, 0, NULL, &y));
	assert(!metalio_touch_map(0, 0, &x, NULL));
	for (raw_x = 0; raw_x < 4096; ++raw_x) {
		bool in_confirm = raw_x >= 70 && raw_x <= 130;
		bool in_key = (raw_x >= 170 && raw_x <= 230) ||
			(raw_x >= 270 && raw_x <= 330);

		assert(metalio_touch_key(raw_x, 1570) == in_key);
		assert(metalio_touch_key(raw_x, 1600) == in_key);
		assert(metalio_touch_key(raw_x, 1630) == in_key);
		assert(!metalio_touch_key(raw_x, 1569));
		assert(!metalio_touch_key(raw_x, 1631));
		assert(metalio_touch_confirm_key(raw_x, 1570) == in_confirm);
		assert(metalio_touch_confirm_key(raw_x, 1600) == in_confirm);
		assert(metalio_touch_confirm_key(raw_x, 1630) == in_confirm);
		assert(!metalio_touch_confirm_key(raw_x, 1569));
		assert(!metalio_touch_confirm_key(raw_x, 1631));
	}
}

static void idle(struct metalio_touch_state *state)
{
	struct metalio_touch_frame frame;
	struct metalio_touch_report empty = report(0, 0, 0, 0, 0, 0, 0, 0, 0);

	metalio_touch_update(state, &empty, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
}

static void check_barrier_and_release(void)
{
	struct metalio_touch_state state;
	struct metalio_touch_frame frame;
	struct metalio_touch_report current = one(15, 0, 20, 30);

	metalio_touch_init(&state);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(15, 2, 4095, 4095);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(15, 2, 20, 30);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(15, 1, 20, 30);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(15, 0, 20, 30);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && frame.points[0].id == 15 && !frame.enter && !frame.confirm);
	metalio_touch_fail(&state, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(2, 0, 100, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(0, 2, 10, 20);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	idle(&state);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && frame.points[0].id == 0);
	idle(&state);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1); /* A disappeared ID is a fresh contact. */
}

static void check_capture(void)
{
	struct metalio_touch_state state;
	struct metalio_touch_frame frame;
	struct metalio_touch_report current;
	unsigned int key;

	metalio_touch_init(&state);
	idle(&state);
	for (key = 0; key < 3; ++key) {
		current = one(key, 0, (key + 1) * 100, 1600);
		metalio_touch_update(&state, &current, &frame);
		assert(!frame.count && frame.enter == (key != 0) && frame.confirm == (key == 0));
		current = one(key, 2, ((key + 1) % 3 + 1) * 100, 1600);
		metalio_touch_update(&state, &current, &frame);
		assert(!frame.count && frame.enter == (key != 0) && frame.confirm == (key == 0));
		current = one(key, 2, 11, 12);
		metalio_touch_update(&state, &current, &frame);
		assert(!frame.count && frame.enter == (key != 0) && frame.confirm == (key == 0));
		current = one(key, 1, 11, 12);
		metalio_touch_update(&state, &current, &frame);
		assert(!frame.count && !frame.enter && !frame.confirm);
	}
	current = one(7, 0, 11, 12);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && !frame.enter && !frame.confirm);
	current = one(7, 2, 200, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && !frame.enter && !frame.confirm);
	assert(frame.points[0].x == 11 && frame.points[0].y == 12);
	current = one(7, 2, 1000, 100);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && frame.points[0].x == 583 && frame.points[0].y == 1000);
	idle(&state);
	current = one(7, 0, 4095, 4095);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(7, 2, 200, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	current = one(7, 2, 50, 60);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	idle(&state);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && frame.points[0].x == 50 && frame.points[0].y == 60);
}

static void check_multitouch(void)
{
	struct metalio_touch_state state;
	struct metalio_touch_frame frame, before;
	struct metalio_touch_report current;
	unsigned int id;

	metalio_touch_init(&state);
	idle(&state);
	for (id = 0; id < 16; ++id) {
		current = one(id, 0, 10 + id, 20 + id);
		metalio_touch_update(&state, &current, &frame);
		assert(frame.count == 1 && frame.points[0].id == id);
		assert(frame.points[0].x == 10 + id && frame.points[0].y == 20 + id);
	}
	idle(&state);
	current = report(2, 15, 0, 30, 40, 2, 0, 10, 20);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 2 && !frame.enter && !frame.confirm);
	assert(frame.points[0].id == 2 && frame.points[1].id == 15);
	before = frame;
	current = report(2, 2, 2, 10, 20, 15, 2, 30, 40);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == before.count && frame.enter == before.enter && frame.confirm == before.confirm);
	for (id = 0; id < 2; ++id) {
		assert(frame.points[id].id == before.points[id].id);
		assert(frame.points[id].x == before.points[id].x);
		assert(frame.points[id].y == before.points[id].y);
	}
	current = report(2, 15, 2, 31, 41, 2, 1, 10, 20);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && frame.points[0].id == 15);
	current = report(2, 0, 0, 200, 1600, 15, 2, 32, 42);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && frame.points[0].id == 15 && frame.enter && !frame.confirm);
	current = one(15, 2, 33, 43);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && !frame.enter && !frame.confirm);
	idle(&state);
	current = report(2, 10, 0, 100, 1600, 11, 0, 300, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && frame.enter && frame.confirm);
	current = one(11, 2, 300, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && frame.enter && !frame.confirm);
	current = report(2, 11, 2, 300, 1600, 12, 0, 200, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && frame.enter && !frame.confirm);
	idle(&state);
	current = one(12, 0, 200, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && frame.enter && !frame.confirm);
	idle(&state);
	current = report(2, 3, 0, 100, 1600, 4, 0, 110, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && frame.confirm);
	current = one(4, 2, 300, 1600); /* Captured confirmation survives dragging. */
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && frame.confirm);
	current = report(2, 4, 2, 300, 1600, 5, 0, 200, 1600);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && frame.enter && frame.confirm);
	metalio_touch_fail(&state, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	metalio_touch_update(&state, &current, &frame);
	assert(!frame.count && !frame.enter && !frame.confirm);
	idle(&state);
	current = report(2, 4, 0, 100, 1600, 5, 0, 40, 50);
	metalio_touch_update(&state, &current, &frame);
	assert(frame.count == 1 && frame.points[0].id == 5 && frame.confirm && !frame.enter);
}

int main(void)
{
	check_decode();
	check_coordinates();
	check_barrier_and_release();
	check_capture();
	check_multitouch();
	puts("PASS: FT6336U protocol, coordinates, capture, confirm/Enter aggregation and recovery");
	return 0;
}

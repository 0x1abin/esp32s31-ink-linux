/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../apps/terminal-ui.h"

static unsigned int portrait_pixel(const struct terminal_ui *ui, int x, int y)
{
	unsigned int native_x = y, native_y = 683 - x;

	return (ui->frame[native_y * 152U + native_x / 8U] >> (7U - native_x % 8U)) & 1U;
}

static unsigned int ink(const struct terminal_ui *ui)
{
	unsigned int total = 0, i;

	for (i = 0; i < sizeof(ui->frame); ++i)
		total += __builtin_popcount(ui->frame[i]);
	return total;
}

static void check_safe_margin(const struct terminal_ui *ui)
{
	int x, y, margin;

	for (margin = 0; margin < 10; ++margin) {
		for (x = 0; x < TERMINAL_UI_WIDTH; ++x)
			assert(!portrait_pixel(ui, x, margin) &&
			       !portrait_pixel(ui, x, TERMINAL_UI_HEIGHT - 1 - margin));
		for (y = 10; y < TERMINAL_UI_HEIGHT - 10; ++y)
			assert(!portrait_pixel(ui, margin, y) &&
			       !portrait_pixel(ui, TERMINAL_UI_WIDTH - 1 - margin, y));
	}
}

static void check_glyph_and_rotation(struct terminal_ui *ui)
{
	/* Independently copied from the pinned Linux font's ASCII A glyph. */
	const unsigned char letter_a[16] = {
		0, 0, 0x10, 0x38, 0x6c, 0xc6, 0xc6, 0xfe,
		0xc6, 0xc6, 0xc6, 0xc6, 0, 0, 0, 0,
	};
	unsigned char uppercase[METALIO_EPD_FRAME_BYTES];
	unsigned int normal, x, y, c, dx, dy;

	terminal_ui_init(ui);
	assert(!ink(ui) && !ui->shift && !ui->symbols);
	assert(TERMINAL_UI_CELL_WIDTH == 16 && TERMINAL_UI_CELL_HEIGHT == 32);
	assert(TERMINAL_UI_SAFE_MARGIN == 10);
	assert(TERMINAL_UI_COLS == 41 && TERMINAL_UI_ROWS == 27);
	terminal_ui_put_cell(ui, 0, 0, 'A', false, false);
	for (y = 0; y < 16; ++y)
		for (x = 0; x < 8; ++x)
			for (dy = 0; dy < 2; ++dy)
				for (dx = 0; dx < 2; ++dx)
					assert(portrait_pixel(ui, 14 + 2 * x + dx, 10 + 2 * y + dy) ==
					       ((letter_a[y] >> (7U - x)) & 1U));
	memcpy(uppercase, ui->frame, sizeof(uppercase));
	normal = ink(ui);
	terminal_ui_put_cell(ui, 0, 0, 'a', false, false);
	assert(memcmp(uppercase, ui->frame, sizeof(uppercase)));
	for (c = 'A'; c <= 'Z'; ++c) {
		terminal_ui_put_cell(ui, 0, 0, c, false, false);
		memcpy(uppercase, ui->frame, sizeof(uppercase));
		terminal_ui_put_cell(ui, 0, 0, c - 'A' + 'a', false, false);
		assert(memcmp(uppercase, ui->frame, sizeof(uppercase)));
	}
	terminal_ui_put_cell(ui, 0, 0, 'A', true, false);
	assert(ink(ui) >= normal);
	terminal_ui_put_cell(ui, 0, 0, 'A', false, true);
	assert(ink(ui) == 512U - normal);
	terminal_ui_clear(ui);
	terminal_ui_put_cell(ui, 0, 0, ' ', false, true);
	assert(ink(ui) == 512);
	assert(ui->frame[669 * 152 + 1] & 0x20); /* Portrait (14,10) -> native (10,669). */
	assert(ui->frame[654 * 152 + 5] & 0x40); /* Portrait (29,41) -> native (41,654). */
	terminal_ui_clear(ui);
	terminal_ui_put_cell(ui, 26, 40, ' ', false, true);
	assert(ink(ui) == 512);
	assert(ui->frame[29 * 152 + 105] & 0x20); /* Portrait (654,842). */
	assert(ui->frame[14 * 152 + 109] & 0x40); /* Portrait (669,873). */
	check_safe_margin(ui);
	terminal_ui_clear(ui);
	for (c = 33; c <= 126; ++c) {
		terminal_ui_put_cell(ui, 0, 0, c, false, false);
		assert(ink(ui));
	}
	terminal_ui_put_cell(ui, 0, 0, '?', false, false);
	memcpy(uppercase, ui->frame, sizeof(uppercase));
	terminal_ui_put_cell(ui, 0, 0, 0x4e2d, false, false);
	assert(!memcmp(uppercase, ui->frame, sizeof(uppercase)));
	terminal_ui_clear(ui);
	terminal_ui_put_cell(ui, 0, 0, 0, false, false);
	assert(!ink(ui));
	terminal_ui_cursor(ui, 0, 0);
	assert(ink(ui) == 64);
	assert(portrait_pixel(ui, 14, 38) && portrait_pixel(ui, 29, 41));
	check_safe_margin(ui);
	terminal_ui_cursor(ui, 0, 0);
	assert(!ink(ui));
	terminal_ui_cursor(ui, -1, 0);
	terminal_ui_cursor(ui, 27, 0);
	terminal_ui_cursor(ui, 0, 41);
	terminal_ui_put_cell(ui, -1, 0, 'A', false, false);
	terminal_ui_put_cell(ui, 0, -1, 'A', false, false);
	terminal_ui_put_cell(ui, 27, 0, 'A', false, false);
	terminal_ui_put_cell(ui, 0, 41, 'A', false, false);
	terminal_ui_put_cell(ui, INT_MIN, INT_MAX, 'A', false, false);
	assert(!ink(ui));
}

static void check_keyboard(struct terminal_ui *ui)
{
	bool characters[127] = { false };
	bool actions[11] = { false };
	unsigned int state, row, x, c;
	int key;

	terminal_ui_init(ui);
	assert(terminal_ui_key_at(ui, 39, 925) == '1');
	assert(terminal_ui_key_at(ui, 67, 1078) == TERMINAL_UI_KEY_MODIFIER);
	assert(ui->shift);
	assert(terminal_ui_key_at(ui, 39, 925) == '!');
	assert(terminal_ui_key_at(ui, 94, 976) == 'Q');
	assert(terminal_ui_key_at(ui, 67, 1078) == TERMINAL_UI_KEY_MODIFIER);
	assert(!ui->shift);
	assert(terminal_ui_key_at(ui, 94, 976) == 'q');
	assert(terminal_ui_key_at(ui, 67, 1129) == TERMINAL_UI_KEY_MODIFIER);
	assert(ui->symbols);
	assert(terminal_ui_key_at(ui, 94, 976) == '`');
	terminal_ui_clear(ui);
	assert(ui->symbols);
	for (state = 0; state < 4; ++state)
		for (row = 0; row < 6; ++row)
			for (x = 0; x < TERMINAL_UI_WIDTH; ++x) {
				ui->shift = state & 1U;
				ui->symbols = state & 2U;
				key = terminal_ui_key_at(ui, x, 925 + row * 51);
				if (key > 0) {
					assert(key >= 32 && key <= 126);
					characters[key] = true;
				} else {
					assert(key >= TERMINAL_UI_KEY_MODIFIER);
					actions[-key] = true;
				}
			}
	for (c = 32; c <= 126; ++c)
		assert(characters[c]);
	for (c = 0; c < 11; ++c)
		assert(actions[c]);
	assert(!terminal_ui_key_at(ui, -1, 925));
	assert(!terminal_ui_key_at(ui, 684, 925));
	assert(!terminal_ui_key_at(ui, 39, 895));
	assert(!terminal_ui_key_at(ui, 39, 1216));
	assert(!terminal_ui_key_at(ui, INT_MAX, INT_MIN));
	assert(!terminal_ui_key_at(ui, 12, 902));
	assert(!terminal_ui_key_at(ui, 39, 900));
	assert(!terminal_ui_key_at(ui, 65, 925));
	assert(terminal_ui_key_at(ui, 14, 902) != TERMINAL_UI_KEY_NONE);
	assert(terminal_ui_key_at(ui, 64, 948) != TERMINAL_UI_KEY_NONE);
	assert(!terminal_ui_key_at(ui, 39, 949));
	assert(!terminal_ui_key_at(ui, 39, 1206));
	assert(!terminal_ui_key_at(ui, 9, 925));
	assert(!terminal_ui_key_at(ui, 674, 925));
}

static void check_keyboard_render(struct terminal_ui *ui)
{
	unsigned char snapshot[METALIO_EPD_FRAME_BYTES];
	unsigned int x, y;

	terminal_ui_init(ui);
	terminal_ui_put_cell(ui, 26, 40, 'a', false, false);
	memcpy(snapshot, ui->frame, sizeof(snapshot));
	terminal_ui_draw_keyboard(ui);
	assert(ink(ui) > 1000);
	for (y = 0; y < TERMINAL_UI_KEYBOARD_TOP; ++y)
		for (x = 0; x < TERMINAL_UI_WIDTH; ++x) {
			unsigned int offset = (683U - x) * 152U + y / 8U;
			unsigned int mask = 128U >> (y % 8U);

			assert((ui->frame[offset] & mask) == (snapshot[offset] & mask));
		}
	memcpy(snapshot, ui->frame, sizeof(snapshot));
	ui->shift = true;
	terminal_ui_draw_keyboard(ui);
	assert(memcmp(snapshot, ui->frame, sizeof(snapshot)));
	ui->symbols = true;
	terminal_ui_draw_keyboard(ui);
	assert(memcmp(snapshot, ui->frame, sizeof(snapshot)));
	check_safe_margin(ui);
}

static void check_no_press_feedback(struct terminal_ui *ui)
{
	unsigned char normal[METALIO_EPD_FRAME_BYTES];
	const int keys[][2] = { {39, 925}, {94, 976}, {620, 1180}, {34, 1180} };

	for (unsigned int state = 0; state < 4; ++state) {
		terminal_ui_init(ui);
		ui->shift = state & 1U;
		ui->symbols = state & 2U;
		terminal_ui_draw_keyboard(ui);
		memcpy(normal, ui->frame, sizeof(normal));
		for (unsigned int key = 0; key < sizeof(keys) / sizeof(keys[0]); ++key) {
			int value = terminal_ui_key_at(ui, keys[key][0], keys[key][1]);
			assert(value && value != TERMINAL_UI_KEY_MODIFIER);
			terminal_ui_draw_keyboard(ui);
			assert(!memcmp(normal, ui->frame, sizeof(normal)));
		}
		assert(!terminal_ui_key_at(ui, -1, 925));
		terminal_ui_clear(ui);
		terminal_ui_draw_keyboard(ui);
		assert(!memcmp(normal, ui->frame, sizeof(normal)));
		check_safe_margin(ui);
	}
}

static void check_safe_layout(struct terminal_ui *ui)
{
	int row, col;
	unsigned int state;

	terminal_ui_init(ui);
	for (row = 0; row < TERMINAL_UI_ROWS; ++row)
		for (col = 0; col < TERMINAL_UI_COLS; ++col)
			terminal_ui_put_cell(ui, row, col, ' ', false, true);
	/* Every cell fits completely, including the first and last rows/columns. */
	assert(ink(ui) == 41U * 27U * 16U * 32U);
	assert(portrait_pixel(ui, 14, 10) && portrait_pixel(ui, 669, 873));
	assert(!portrait_pixel(ui, 13, 10) && !portrait_pixel(ui, 670, 873));
	assert(!portrait_pixel(ui, 14, 9) && !portrait_pixel(ui, 669, 874));
	check_safe_margin(ui);
	for (row = 0; row < TERMINAL_UI_ROWS; ++row)
		for (col = 0; col < TERMINAL_UI_COLS; ++col)
			terminal_ui_put_cell(ui, row, col, 'A', true, true);
	terminal_ui_cursor(ui, 0, 0);
	terminal_ui_cursor(ui, 26, 40);
	for (state = 0; state < 4; ++state) {
		ui->shift = state & 1U;
		ui->symbols = state & 2U;
		terminal_ui_draw_keyboard(ui);
		check_safe_margin(ui);
	}
}

static void preview(struct terminal_ui *ui, const char *path)
{
	FILE *output = fopen(path, "wb");
	int row = 0, col, x, y, line, character;
	const char *lines[] = {
		"Metalio Linux 6.18 - native E0470A14-AF terminal",
		"metalio:~# uname -a",
		"Linux metalio 6.18 riscv32 GNU/Linux",
		"metalio:~# echo 'Hello, Linux!'",
		"Hello, Linux!",
		"0123456789 !@#$%^&*()_+{}|:\"<>?",
		"abcdefghijklmnopqrstuvwxyz []\\;',./`~-="
	};

	assert(output);
	terminal_ui_init(ui);
	for (line = 0; line < (int)(sizeof(lines) / sizeof(lines[0])); ++line) {
		col = 0;
		for (character = 0; lines[line][character]; ++character) {
			if (col == TERMINAL_UI_COLS) {
				col = 0;
				++row;
			}
			assert(row < TERMINAL_UI_ROWS);
			terminal_ui_put_cell(ui, row, col++, (unsigned char)lines[line][character],
					     false, false);
		}
		++row;
	}
	terminal_ui_cursor(ui, row, 0);
	terminal_ui_draw_keyboard(ui);
	check_safe_margin(ui);
	assert(fprintf(output, "P4\n684 1216\n") > 0);
	for (y = 0; y < 1216; ++y)
		for (x = 0; x < 688; x += 8) {
			unsigned char value = 0;
			int bit;

			for (bit = 0; bit < 8 && x + bit < 684; ++bit)
				value |= portrait_pixel(ui, x + bit, y) << (7 - bit);
			assert(fputc(value, output) != EOF);
		}
	assert(!fclose(output));
}

int main(int argc, char **argv)
{
	struct {
		unsigned char before[8];
		struct terminal_ui ui;
		unsigned char after[8];
	} *storage = malloc(sizeof(*storage));
	unsigned int i;

	assert(storage);
	memset(storage, 0xa5, sizeof(*storage));
	check_glyph_and_rotation(&storage->ui);
	check_keyboard(&storage->ui);
	check_keyboard_render(&storage->ui);
	check_no_press_feedback(&storage->ui);
	check_safe_layout(&storage->ui);
	for (i = 0; i < 8; ++i)
		assert(storage->before[i] == 0xa5 && storage->after[i] == 0xa5);
	if (argc == 2)
		preview(&storage->ui, argv[1]);
	free(storage);
	puts("terminal UI: ASCII glyphs, rotation, attributes, cursor, keyboard layers, no press redraws and 10px safe margins passed");
	return 0;
}

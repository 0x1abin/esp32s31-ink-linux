/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
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
	assert(TERMINAL_UI_COLS == 41 && TERMINAL_UI_ROWS == 22);
	assert(TERMINAL_UI_KEYBOARD_TOP == 728);
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
	terminal_ui_put_cell(ui, 21, 40, ' ', false, true);
	assert(ink(ui) == 512);
	assert(ui->frame[29 * 152 + 85] & 0x20); /* Portrait (654,682). */
	assert(ui->frame[14 * 152 + 89] & 0x40); /* Portrait (669,713). */
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
	terminal_ui_cursor(ui, 22, 0);
	terminal_ui_cursor(ui, 0, 41);
	terminal_ui_put_cell(ui, -1, 0, 'A', false, false);
	terminal_ui_put_cell(ui, 0, -1, 'A', false, false);
	terminal_ui_put_cell(ui, 22, 0, 'A', false, false);
	terminal_ui_put_cell(ui, 0, 41, 'A', false, false);
	terminal_ui_put_cell(ui, INT_MIN, INT_MAX, 'A', false, false);
	assert(!ink(ui));
	ui->shift = true;
	terminal_ui_draw_keyboard(ui);
	/* Keyboard A uses the same font at 24x48, centered in its 66x79 key. */
	for (y = 0; y < 16; ++y)
		for (x = 0; x < 8; ++x)
			for (dy = 0; dy < 3; ++dy)
				for (dx = 0; dx < 3; ++dx)
					assert(portrait_pixel(ui, 66 + 3 * x + dx, 905 + 3 * y + dy) ==
					       ((letter_a[y] >> (7U - x)) & 1U));
}

static void check_symbol(struct terminal_ui *ui, uint32_t codepoint,
			 const unsigned char *question)
{
	unsigned int normal;

	terminal_ui_clear(ui);
	terminal_ui_put_cell(ui, 0, 0, codepoint, false, false);
	assert(memcmp(question, ui->frame, sizeof(ui->frame)));
	normal = ink(ui);
	assert(normal);
	terminal_ui_put_cell(ui, 0, 0, codepoint, false, true);
	assert(ink(ui) == 512U - normal);
	terminal_ui_clear(ui);
	terminal_ui_put_cell(ui, TERMINAL_UI_ROWS - 1, TERMINAL_UI_COLS - 1,
			     codepoint, true, false);
	assert(ink(ui) >= normal);
	normal = ink(ui);
	terminal_ui_put_cell(ui, TERMINAL_UI_ROWS - 1, TERMINAL_UI_COLS - 1,
			     codepoint, true, true);
	assert(ink(ui) == 512U - normal);
	check_safe_margin(ui);
}

static void check_symbols(struct terminal_ui *ui)
{
	const unsigned int ranges[][2] = {
		{ 0x2190, 0x21ff }, { 0x2500, 0x259f }, { 0x2800, 0x28ff },
	};
	const char32_t common[] = U"·•‣…–—‘’“”‹›⋯∅−∙√∞≠≤≥⌘⏎⏺●○◉◦▪▫■□◆◇▶▷◀◁▲△▼▽✓✔✕✖✗✘❯❮⚠✦✧★☆";
	const uint32_t unsupported[] = {
		0x1f, 0x7f, 0x00b6, 0x00b8, 0x218f, 0x2200, 0x24ff,
		0x25a2, 0x27ff, 0x2900, 0x10000, UINT32_MAX,
	};
	const unsigned int dots[][3] = {
		{ 3, 4, 0 }, { 3, 7, 1 }, { 3, 10, 2 }, { 5, 4, 3 },
		{ 5, 7, 4 }, { 5, 10, 5 }, { 3, 13, 6 }, { 5, 13, 7 },
	};
	unsigned char question[METALIO_EPD_FRAME_BYTES];
	unsigned int x, y;

	terminal_ui_init(ui);
	terminal_ui_put_cell(ui, 0, 0, '?', false, false);
	memcpy(question, ui->frame, sizeof(question));
	for (unsigned int range = 0; range < sizeof(ranges) / sizeof(ranges[0]); ++range)
		for (unsigned int cp = ranges[range][0]; cp <= ranges[range][1]; ++cp)
			check_symbol(ui, cp, question);
	for (unsigned int i = 0; common[i]; ++i)
		check_symbol(ui, common[i], question);
	for (unsigned int pattern = 0; pattern < 256; ++pattern) {
		terminal_ui_clear(ui);
		terminal_ui_put_cell(ui, 0, 0, 0x2800 + pattern, false, false);
		for (unsigned int dot = 0; dot < 8; ++dot)
			assert(portrait_pixel(ui, 14 + 2 * dots[dot][0], 10 + 2 * dots[dot][1]) ==
			       !!(pattern & (1U << dots[dot][2])));
	}
	terminal_ui_clear(ui);
	terminal_ui_put_cell(ui, 0, 0, 0x256d, false, false);
	terminal_ui_put_cell(ui, 0, 1, 0x2500, false, false);
	terminal_ui_put_cell(ui, 0, 2, 0x256e, false, false);
	terminal_ui_put_cell(ui, 1, 0, 0x2502, false, false);
	terminal_ui_put_cell(ui, 1, 2, 0x2502, false, false);
	terminal_ui_put_cell(ui, 2, 0, 0x2570, false, false);
	terminal_ui_put_cell(ui, 2, 1, 0x2500, false, false);
	terminal_ui_put_cell(ui, 2, 2, 0x256f, false, false);
	for (x = 26; x < 52; ++x)
		assert(portrait_pixel(ui, x, 24) && portrait_pixel(ui, x, 88));
	for (y = 28; y < 86; ++y)
		assert(portrait_pixel(ui, 22, y) && portrait_pixel(ui, 54, y));
	check_safe_margin(ui);
	for (unsigned int block = 0; block < 2; ++block) {
		terminal_ui_clear(ui);
		terminal_ui_put_cell(ui, 0, 0, block ? 0x2588 : 0x2580, false, false);
		for (y = 0; y < 32; ++y)
			for (x = 0; x < 16; ++x)
				assert(portrait_pixel(ui, 14 + x, 10 + y) == (block || y < 16));
	}
	for (unsigned int i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
		terminal_ui_clear(ui);
		terminal_ui_put_cell(ui, 0, 0, unsupported[i], false, false);
		assert(!memcmp(question, ui->frame, sizeof(question)));
	}
}

static void check_keyboard(struct terminal_ui *ui)
{
	bool characters[127] = { false };
	bool actions[11] = { false };
	unsigned int state, row, x, c;
	int key;

	terminal_ui_init(ui);
	assert(terminal_ui_key_at(ui, 45, 771) == '1');
	assert(terminal_ui_key_at(ui, 61, 1008) == TERMINAL_UI_KEY_MODIFIER);
	assert(ui->shift);
	assert(terminal_ui_key_at(ui, 45, 771) == '!');
	assert(terminal_ui_key_at(ui, 45, 850) == 'Q');
	assert(terminal_ui_key_at(ui, 639, 1087) == '_');
	assert(terminal_ui_key_at(ui, 243, 1087) == '?');
	assert(terminal_ui_key_at(ui, 61, 1008) == TERMINAL_UI_KEY_MODIFIER);
	assert(!ui->shift);
	assert(terminal_ui_key_at(ui, 45, 850) == 'q');
	assert(terminal_ui_key_at(ui, 61, 1087) == TERMINAL_UI_KEY_MODIFIER);
	assert(ui->symbols);
	assert(terminal_ui_key_at(ui, 45, 850) == '!');
	assert(terminal_ui_key_at(ui, 61, 1008) == TERMINAL_UI_KEY_MODIFIER);
	assert(ui->shift && ui->symbols);
	assert(terminal_ui_key_at(ui, 144, 1008) == '~');
	terminal_ui_clear(ui);
	assert(ui->symbols);
	for (state = 0; state < 4; ++state)
		for (row = 0; row < 6; ++row)
			for (x = 0; x < TERMINAL_UI_WIDTH; ++x) {
				ui->shift = state & 1U;
				ui->symbols = state & 2U;
				key = terminal_ui_key_at(ui, x, 771 + row * 79);
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
	assert(!terminal_ui_key_at(ui, -1, 771));
	assert(!terminal_ui_key_at(ui, 684, 771));
	assert(!terminal_ui_key_at(ui, 45, 727));
	assert(!terminal_ui_key_at(ui, 39, 1216));
	assert(!terminal_ui_key_at(ui, INT_MAX, INT_MIN));
	assert(!terminal_ui_key_at(ui, 12, 735));
	assert(!terminal_ui_key_at(ui, 45, 732));
	assert(!terminal_ui_key_at(ui, 75, 771));
	assert(!terminal_ui_key_at(ui, 45, 808));
	assert(!terminal_ui_key_at(ui, 45, 1203));
	assert(!terminal_ui_key_at(ui, 9, 771));
	assert(!terminal_ui_key_at(ui, 674, 771));
}

static void check_key_box(struct terminal_ui *ui, int left, int top, int width,
			  int value, bool symbols)
{
	const int points[][2] = {
		{ left + width / 2, top + 39 },
		{ left + 3, top + 3 }, { left + width - 4, top + 3 },
		{ left + 3, top + 75 }, { left + width - 4, top + 75 },
	};

	for (unsigned int i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
		ui->shift = false;
		ui->symbols = symbols;
		assert(terminal_ui_key_at(ui, points[i][0], points[i][1]) == value);
	}
	assert(!terminal_ui_key_at(ui, left + 2, top + 39));
	assert(!terminal_ui_key_at(ui, left + width - 3, top + 39));
	assert(!terminal_ui_key_at(ui, left + width / 2, top + 2));
	assert(!terminal_ui_key_at(ui, left + width / 2, top + 76));
}

static void check_keyboard_layout(struct terminal_ui *ui)
{
	const char *letters[2][4] = {
		{ "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" },
		{ "1234567890", "!@#$%^&*()", "[]{}<>=+|", "`\\;:'\"," },
	};
	const struct { int left, top, width, value; } fixed[] = {
		{ 12, 969, 99, TERMINAL_UI_KEY_MODIFIER },
		{ 573, 969, 99, TERMINAL_UI_KEY_BACKSPACE },
		{ 12, 1048, 99, TERMINAL_UI_KEY_MODIFIER },
		{ 111, 1048, 99, TERMINAL_UI_KEY_TAB },
		{ 210, 1048, 66, '/' }, { 276, 1048, 264, ' ' },
		{ 540, 1048, 66, '.' }, { 606, 1048, 66, '-' },
		{ 12, 1127, 66, TERMINAL_UI_KEY_ESCAPE },
		{ 78, 1127, 132, TERMINAL_UI_KEY_CTRL_C },
		{ 210, 1127, 66, TERMINAL_UI_KEY_LEFT },
		{ 276, 1127, 66, TERMINAL_UI_KEY_DOWN },
		{ 342, 1127, 66, TERMINAL_UI_KEY_UP },
		{ 408, 1127, 66, TERMINAL_UI_KEY_RIGHT },
		{ 474, 1127, 198, TERMINAL_UI_KEY_ENTER },
	};

	terminal_ui_init(ui);
	for (unsigned int layer = 0; layer < 2; ++layer) {
		for (unsigned int row = 0; row < 4; ++row)
			for (unsigned int col = 0; letters[layer][row][col]; ++col) {
				int left = (row == 2 ? 45 : row == 3 ? 111 : 12) + col * 66;

				check_key_box(ui, left, 732 + row * 79, 66,
					      letters[layer][row][col], layer);
			}
		for (unsigned int i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
			check_key_box(ui, fixed[i].left, fixed[i].top, fixed[i].width,
				      fixed[i].value, layer);
		assert(!terminal_ui_key_at(ui, 47, 929));
		assert(!terminal_ui_key_at(ui, 636, 929));
	}
}

static void check_keyboard_render(struct terminal_ui *ui)
{
	unsigned char snapshot[METALIO_EPD_FRAME_BYTES];
	unsigned int x, y;

	terminal_ui_init(ui);
	terminal_ui_put_cell(ui, 21, 40, 'a', false, false);
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
	for (unsigned int state = 0; state < 4; ++state) {
		ui->shift = state & 1U;
		ui->symbols = state & 2U;
		terminal_ui_draw_keyboard(ui);
		/* Borders and labels must stay inside their touch targets in every layer. */
		for (y = TERMINAL_UI_KEYBOARD_TOP; y < TERMINAL_UI_HEIGHT; ++y)
			for (x = 0; x < TERMINAL_UI_WIDTH; ++x) {
				ui->shift = state & 1U;
				ui->symbols = state & 2U;
				if (!terminal_ui_key_at(ui, x, y))
					assert(!portrait_pixel(ui, x, y));
			}
	}
	/* The four arrows have a shaft and the appropriate directional head. */
	assert(portrait_pixel(ui, 243, 1166) && portrait_pixel(ui, 230, 1166));
	assert(portrait_pixel(ui, 235, 1161) && !portrait_pixel(ui, 251, 1161));
	assert(portrait_pixel(ui, 309, 1166) && portrait_pixel(ui, 309, 1179));
	assert(portrait_pixel(ui, 304, 1174) && !portrait_pixel(ui, 304, 1158));
	assert(portrait_pixel(ui, 375, 1166) && portrait_pixel(ui, 375, 1153));
	assert(portrait_pixel(ui, 370, 1158) && !portrait_pixel(ui, 370, 1174));
	assert(portrait_pixel(ui, 441, 1166) && portrait_pixel(ui, 454, 1166));
	assert(portrait_pixel(ui, 449, 1161) && !portrait_pixel(ui, 433, 1161));
}

static void check_no_press_feedback(struct terminal_ui *ui)
{
	unsigned char normal[METALIO_EPD_FRAME_BYTES];
	const int keys[][2] = { {45, 771}, {45, 850}, {573, 1166}, {144, 1166} };

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
		assert(!terminal_ui_key_at(ui, -1, 771));
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
	assert(ink(ui) == 41U * 22U * 16U * 32U);
	assert(portrait_pixel(ui, 14, 10) && portrait_pixel(ui, 669, 713));
	assert(!portrait_pixel(ui, 13, 10) && !portrait_pixel(ui, 670, 713));
	assert(!portrait_pixel(ui, 14, 9) && !portrait_pixel(ui, 669, 714));
	check_safe_margin(ui);
	for (row = 0; row < TERMINAL_UI_ROWS; ++row)
		for (col = 0; col < TERMINAL_UI_COLS; ++col)
			terminal_ui_put_cell(ui, row, col, 'A', true, true);
	terminal_ui_cursor(ui, 0, 0);
	terminal_ui_cursor(ui, 21, 40);
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
	const char32_t *lines[] = {
		U"Metalio Linux 6.18 - native E0470A14-AF terminal",
		U"metalio:~# uname -a",
		U"Linux metalio 6.18 riscv32 GNU/Linux",
		U"metalio:~# echo 'Hello, Linux!'",
		U"Hello, Linux!",
		U"0123456789 !@#$%^&*()_+{}|:\"<>?",
		U"abcdefghijklmnopqrstuvwxyz []\\;',./`~-=",
		U"",
		U"╭───────────────────────────────────────╮",
		U"│ › CLI symbols: ✓ ✔ ✗ ✘ ⚠ • … → ↵ ⏎ ⏺  │",
		U"│ ╭─╮ ╔═╗ ┌─┐ ┏━┓ ░▒▓█ ▀▄▌▐ ■□◆◇●○◉◦    │",
		U"│ ╰─╯ ╚═╝ └─┘ ┗━┛ ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏            │",
		U"│ ← ↑ → ↓ ↔ ↕ ↖ ↗ ↘ ↙ ▲△▼▽▶▷◀◁ ★☆ ✦✧    │",
		U"│ ‘quotes’ “text” ‹path› – — ⋯ ≠ ≤ ≥ √ ∞│",
		U"╰───────────────────────────────────────╯",
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
			terminal_ui_put_cell(ui, row, col++, lines[line][character],
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
	check_symbols(&storage->ui);
	check_keyboard(&storage->ui);
	check_keyboard_layout(&storage->ui);
	check_keyboard_render(&storage->ui);
	check_no_press_feedback(&storage->ui);
	check_safe_layout(&storage->ui);
	for (i = 0; i < 8; ++i)
		assert(storage->before[i] == 0xa5 && storage->after[i] == 0xa5);
	if (argc == 2)
		preview(&storage->ui, argv[1]);
	free(storage);
	puts("terminal UI: ASCII/Unicode glyphs, rotation, attributes, box joins, blocks, Braille dots, cursor, large key geometry, keyboard layers, arrows, no press redraws and 10px safe margins passed");
	return 0;
}

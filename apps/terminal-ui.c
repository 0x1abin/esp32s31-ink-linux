/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-ui.h"
#include "terminal-font.h"
#include <string.h>

/* Half-key units allow 1.5-key modifiers and a centered home row. */
#define KEY_UNIT ((TERMINAL_UI_WIDTH - 2 * TERMINAL_UI_SAFE_MARGIN) / 20)
#define KEY_Y (TERMINAL_UI_KEYBOARD_TOP + 4)
#define KEY_HEIGHT ((TERMINAL_UI_HEIGHT - TERMINAL_UI_SAFE_MARGIN - KEY_Y) / 6)
#define KEY_INSET 3
#define UI_SHIFT (-100)
#define UI_SYMBOLS (-101)
#define CELL_X ((TERMINAL_UI_WIDTH - TERMINAL_UI_COLS * TERMINAL_UI_CELL_WIDTH) / 2)
#define CELL_Y TERMINAL_UI_SAFE_MARGIN

struct key {
	int value, symbol;
	unsigned char units;
	const char *label;
};

#define CHAR(c, s) { c, s, 2, NULL }
#define ACTION(key, width, text) { key, key, width, text }
static const struct key keyboard[6][10] = {
	{ CHAR('1','1'), CHAR('2','2'), CHAR('3','3'), CHAR('4','4'),
	  CHAR('5','5'), CHAR('6','6'), CHAR('7','7'), CHAR('8','8'),
	  CHAR('9','9'), CHAR('0','0') },
	{ CHAR('q','!'), CHAR('w','@'), CHAR('e','#'), CHAR('r','$'),
	  CHAR('t','%'), CHAR('y','^'), CHAR('u','&'), CHAR('i','*'),
	  CHAR('o','('), CHAR('p',')') },
	{ CHAR('a','['), CHAR('s',']'), CHAR('d','{'), CHAR('f','}'),
	  CHAR('g','<'), CHAR('h','>'), CHAR('j','='), CHAR('k','+'), CHAR('l','|') },
	{ ACTION(UI_SHIFT,3,"Shift"),
	  CHAR('z','`'), CHAR('x','\\'), CHAR('c',';'), CHAR('v',':'),
	  CHAR('b','\''), CHAR('n','"'), CHAR('m',','),
	  ACTION(TERMINAL_UI_KEY_BACKSPACE,3,"Back") },
	{ ACTION(UI_SYMBOLS,3,"Sym"), ACTION(TERMINAL_UI_KEY_TAB,3,"Tab"),
	  CHAR('/','/'), ACTION(' ',8,"Space"), CHAR('.','.'), CHAR('-','-') },
	{ ACTION(TERMINAL_UI_KEY_ESCAPE,2,"Esc"),
	  ACTION(TERMINAL_UI_KEY_CTRL_C,4,"Ctrl-C"),
	  ACTION(TERMINAL_UI_KEY_LEFT,2,"Left"),
	  ACTION(TERMINAL_UI_KEY_DOWN,2,"Down"),
	  ACTION(TERMINAL_UI_KEY_UP,2,"Up"),
	  ACTION(TERMINAL_UI_KEY_RIGHT,2,"Right"),
	  ACTION(TERMINAL_UI_KEY_ENTER,6,"Enter") },
};
#undef CHAR
#undef ACTION

static int row_left(unsigned int row)
{
	unsigned int index;
	int units = 0;

	for (index = 0; index < 10; ++index)
		units += keyboard[row][index].units;
	return (TERMINAL_UI_WIDTH - units * KEY_UNIT) / 2;
}

static void pixel(struct terminal_ui *ui, int x, int y, bool black)
{
	unsigned int offset, mask;

	if (x < TERMINAL_UI_SAFE_MARGIN || x >= TERMINAL_UI_WIDTH - TERMINAL_UI_SAFE_MARGIN ||
	    y < TERMINAL_UI_SAFE_MARGIN || y >= TERMINAL_UI_HEIGHT - TERMINAL_UI_SAFE_MARGIN)
		return;
	/* Same landscape rotation as the reference reader: (xP,yP)=(yL,683-xL). */
	offset = (TERMINAL_UI_WIDTH - 1 - x) * METALIO_EPD_STRIDE + y / 8;
	mask = 128U >> (y % 8);
	if (black)
		ui->frame[offset] |= mask;
	else
		ui->frame[offset] &= ~mask;
}

static void rectangle(struct terminal_ui *ui, int x, int y, int width, int height,
		      bool black)
{
	int dx, dy;

	for (dy = 0; dy < height; ++dy)
		for (dx = 0; dx < width; ++dx)
			pixel(ui, x + dx, y + dy, black);
}

static const unsigned char *font_bitmap(uint32_t codepoint)
{
	size_t low = 0, high = sizeof(terminal_symbols_8x16) / sizeof(terminal_symbols_8x16[0]);

	if (!codepoint)
		codepoint = ' ';
	if (codepoint >= 32 && codepoint <= 126)
		return terminal_font_8x16[codepoint - 32];
	while (low < high) {
		size_t mid = low + (high - low) / 2;

		if (codepoint == terminal_symbols_8x16[mid].codepoint)
			return terminal_symbols_8x16[mid].bitmap;
		if (codepoint < terminal_symbols_8x16[mid].codepoint)
			high = mid;
		else
			low = mid + 1;
	}
	return terminal_font_8x16['?' - 32];
}

static void glyph(struct terminal_ui *ui, int x, int y, uint32_t codepoint,
		  bool bold, bool inverse, int scale)
{
	const unsigned char *bitmap = font_bitmap(codepoint);
	int row, col, dx, dy;

	for (row = 0; row < 16; ++row) {
		unsigned char bits = bitmap[row];

		if (bold)
			bits |= bits >> 1;
		for (col = 0; col < 8; ++col)
			for (dy = 0; dy < scale; ++dy)
				for (dx = 0; dx < scale; ++dx)
					pixel(ui, x + col * scale + dx, y + row * scale + dy,
					      !!(bits & (128U >> col)) != inverse);
	}
}

static void arrow(struct terminal_ui *ui, int x, int y, int value)
{
	bool horizontal = value == TERMINAL_UI_KEY_LEFT || value == TERMINAL_UI_KEY_RIGHT;
	int direction = value == TERMINAL_UI_KEY_LEFT || value == TERMINAL_UI_KEY_UP ? -1 : 1;
	int i;

	if (horizontal)
		rectangle(ui, x - 12, y - 1, 25, 3, true);
	else
		rectangle(ui, x - 1, y - 12, 3, 25, true);
	for (i = 0; i <= 10; ++i) {
		int along = direction * (12 - i);

		if (horizontal) {
			rectangle(ui, x + along - 1, y - i - 1, 3, 3, true);
			rectangle(ui, x + along - 1, y + i - 1, 3, 3, true);
		} else {
			rectangle(ui, x - i - 1, y + along - 1, 3, 3, true);
			rectangle(ui, x + i - 1, y + along - 1, 3, 3, true);
		}
	}
}

static int shifted(int character)
{
	static const char plain[] = "`1234567890-=[]\\;',./";
	static const char shift[] = "~!@#$%^&*()_+{}|:\"<>?";
	const char *position;

	if (character >= 'a' && character <= 'z')
		return character - 'a' + 'A';
	position = strchr(plain, character);
	return position ? shift[position - plain] : character;
}

static int key_value(const struct terminal_ui *ui, const struct key *key)
{
	int value = ui->symbols ? key->symbol : key->value;

	return ui->shift && value > 0 ? shifted(value) : value;
}

void terminal_ui_init(struct terminal_ui *ui)
{
	memset(ui, 0, sizeof(*ui));
}

void terminal_ui_clear(struct terminal_ui *ui)
{
	memset(ui->frame, 0, sizeof(ui->frame));
}

void terminal_ui_put_cell(struct terminal_ui *ui, int row, int col,
			  uint32_t codepoint, bool bold, bool inverse)
{
	if (row < 0 || row >= TERMINAL_UI_ROWS || col < 0 || col >= TERMINAL_UI_COLS)
		return;
	glyph(ui, CELL_X + col * TERMINAL_UI_CELL_WIDTH, CELL_Y + row * TERMINAL_UI_CELL_HEIGHT,
	      codepoint, bold, inverse, TERMINAL_UI_CELL_WIDTH / 8);
}

void terminal_ui_cursor(struct terminal_ui *ui, int row, int col)
{
	int x, y;

	if (row < 0 || row >= TERMINAL_UI_ROWS || col < 0 || col >= TERMINAL_UI_COLS)
		return;
	for (y = CELL_Y + (row + 1) * TERMINAL_UI_CELL_HEIGHT - TERMINAL_UI_CELL_HEIGHT / 8;
	     y < CELL_Y + (row + 1) * TERMINAL_UI_CELL_HEIGHT; ++y)
		for (x = CELL_X + col * TERMINAL_UI_CELL_WIDTH;
		     x < CELL_X + (col + 1) * TERMINAL_UI_CELL_WIDTH; ++x) {
			unsigned int offset = (TERMINAL_UI_WIDTH - 1 - x) * METALIO_EPD_STRIDE + y / 8;

			pixel(ui, x, y, !(ui->frame[offset] & (128U >> (y % 8))));
		}
}

void terminal_ui_draw_keyboard(struct terminal_ui *ui)
{
	unsigned int row, index;

	rectangle(ui, 0, TERMINAL_UI_KEYBOARD_TOP, TERMINAL_UI_WIDTH,
		  TERMINAL_UI_HEIGHT - TERMINAL_UI_KEYBOARD_TOP, false);
	for (row = 0; row < 6; ++row) {
		int left = row_left(row);

		for (index = 0; index < 10 && keyboard[row][index].units; ++index) {
			const struct key *key = &keyboard[row][index];
			int width = key->units * KEY_UNIT;
			int top = KEY_Y + row * KEY_HEIGHT;
			bool active = (key->value == UI_SHIFT && ui->shift) ||
				      (key->value == UI_SYMBOLS && ui->symbols);
			char character[2] = { 0, 0 };
			const char *label = key->label;
			int x, scale = label ? 2 : 3;

			rectangle(ui, left + KEY_INSET, top + KEY_INSET,
				  width - 2 * KEY_INSET, KEY_HEIGHT - 2 * KEY_INSET, true);
			if (!active)
				rectangle(ui, left + KEY_INSET + 1, top + KEY_INSET + 1,
					  width - 2 * KEY_INSET - 2, KEY_HEIGHT - 2 * KEY_INSET - 2, false);
			if (key->value == TERMINAL_UI_KEY_LEFT || key->value == TERMINAL_UI_KEY_DOWN ||
			    key->value == TERMINAL_UI_KEY_UP || key->value == TERMINAL_UI_KEY_RIGHT) {
				arrow(ui, left + width / 2, top + KEY_HEIGHT / 2, key->value);
				left += width;
				continue;
			}
			if (key->value == UI_SYMBOLS)
				label = ui->symbols ? "ABC" : "Sym";
			if (!label) {
				character[0] = key_value(ui, key);
				label = character;
			}
			x = left + (width - (int)strlen(label) * 8 * scale) / 2;
			for (; *label; ++label, x += 8 * scale)
				glyph(ui, x, top + (KEY_HEIGHT - 16 * scale) / 2,
				      (unsigned char)*label, false, active, scale);
			left += width;
		}
	}
}

int terminal_ui_key_at(struct terminal_ui *ui, int x, int y)
{
	const struct key *key;
	unsigned int row, index;
	int left, top;

	if (x < TERMINAL_UI_SAFE_MARGIN || x >= TERMINAL_UI_WIDTH - TERMINAL_UI_SAFE_MARGIN ||
	    y < KEY_Y || y >= KEY_Y + 6 * KEY_HEIGHT)
		return TERMINAL_UI_KEY_NONE;
	row = (y - KEY_Y) / KEY_HEIGHT;
	top = KEY_Y + row * KEY_HEIGHT;
	if (y < top + KEY_INSET || y >= top + KEY_HEIGHT - KEY_INSET)
		return TERMINAL_UI_KEY_NONE;
	left = row_left(row);
	for (index = 0; index < 10 && keyboard[row][index].units; ++index) {
		key = &keyboard[row][index];
		if (x >= left + KEY_INSET && x < left + key->units * KEY_UNIT - KEY_INSET) {
			if (key->value == UI_SHIFT) {
				ui->shift = !ui->shift;
				return TERMINAL_UI_KEY_MODIFIER;
			}
			if (key->value == UI_SYMBOLS) {
				ui->symbols = !ui->symbols;
				return TERMINAL_UI_KEY_MODIFIER;
			}
			return key_value(ui, key);
		}
		left += key->units * KEY_UNIT;
	}
	return TERMINAL_UI_KEY_NONE;
}

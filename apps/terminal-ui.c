/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-ui.h"
#include "terminal-font.h"
#include <string.h>

#define KEY_UNIT ((TERMINAL_UI_WIDTH - 2 * TERMINAL_UI_SAFE_MARGIN) / 12)
#define KEY_X ((TERMINAL_UI_WIDTH - 12 * KEY_UNIT) / 2)
#define KEY_Y (TERMINAL_UI_KEYBOARD_TOP + 4)
#define KEY_HEIGHT ((TERMINAL_UI_HEIGHT - TERMINAL_UI_SAFE_MARGIN - KEY_Y) / 6)
#define UI_SHIFT (-100)
#define UI_SYMBOLS (-101)
#define CELL_X ((TERMINAL_UI_WIDTH - TERMINAL_UI_COLS * TERMINAL_UI_CELL_WIDTH) / 2)
#define CELL_Y TERMINAL_UI_SAFE_MARGIN

struct key {
	int value, symbol;
	unsigned char units;
	const char *label;
};

#define CHAR(c, s) { c, s, 1, NULL }
#define ACTION(key, width, text) { key, key, width, text }
static const struct key keyboard[6][12] = {
	{ CHAR('1','1'), CHAR('2','2'), CHAR('3','3'), CHAR('4','4'),
	  CHAR('5','5'), CHAR('6','6'), CHAR('7','7'), CHAR('8','8'),
	  CHAR('9','9'), CHAR('0','0'), CHAR('-','-'), CHAR('=','=') },
	{ ACTION(TERMINAL_UI_KEY_TAB,1,"Tab"),
	  CHAR('q','`'), CHAR('w','~'), CHAR('e','['), CHAR('r',']'),
	  CHAR('t','{'), CHAR('y','}'), CHAR('u','<'), CHAR('i','>'),
	  CHAR('o','\\'), CHAR('p','|'), CHAR('\\','/') },
	{ ACTION(TERMINAL_UI_KEY_ESCAPE,1,"Esc"),
	  CHAR('a','!'), CHAR('s','@'), CHAR('d','#'), CHAR('f','$'),
	  CHAR('g','%'), CHAR('h','^'), CHAR('j','&'), CHAR('k','*'),
	  CHAR('l','('), CHAR(';',')'), CHAR('\'','-') },
	{ ACTION(UI_SHIFT,2,"Shift"),
	  CHAR('z','='), CHAR('x','+'), CHAR('c','_'), CHAR('v',':'),
	  CHAR('b',';'), CHAR('n','"'), CHAR('m','\''), CHAR(',',','),
	  CHAR('.','.'), CHAR('/','?') },
	{ ACTION(UI_SYMBOLS,2,"Sym"), CHAR('`','`'), CHAR('[','['), CHAR(']',']'),
	  ACTION(' ',5,"Space"), ACTION(TERMINAL_UI_KEY_BACKSPACE,2,"Back") },
	{ ACTION(TERMINAL_UI_KEY_CTRL_C,2,"Ctrl-C"),
	  ACTION(TERMINAL_UI_KEY_LEFT,2,"Left"),
	  ACTION(TERMINAL_UI_KEY_DOWN,2,"Down"),
	  ACTION(TERMINAL_UI_KEY_UP,2,"Up"),
	  ACTION(TERMINAL_UI_KEY_RIGHT,2,"Right"),
	  ACTION(TERMINAL_UI_KEY_ENTER,2,"Enter") },
};
#undef CHAR
#undef ACTION

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

static void glyph(struct terminal_ui *ui, int x, int y, uint32_t codepoint,
		  bool bold, bool inverse, int scale)
{
	const unsigned char *bitmap;
	int row, col, dx, dy;

	if (!codepoint)
		codepoint = ' ';
	if (codepoint < 32 || codepoint > 126)
		codepoint = '?';
	bitmap = terminal_font_8x16[codepoint - 32];
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
		int left = KEY_X;

		for (index = 0; index < 12 && keyboard[row][index].units; ++index) {
			const struct key *key = &keyboard[row][index];
			int width = key->units * KEY_UNIT;
			int top = KEY_Y + row * KEY_HEIGHT;
			bool active = (key->value == UI_SHIFT && ui->shift) ||
				      (key->value == UI_SYMBOLS && ui->symbols);
			char character[2] = { 0, 0 };
			const char *label = key->label;
			int x;

			rectangle(ui, left + 2, top + 2, width - 4, KEY_HEIGHT - 4, true);
			if (!active)
				rectangle(ui, left + 3, top + 3, width - 6, KEY_HEIGHT - 6, false);
			if (key->value == UI_SYMBOLS)
				label = ui->symbols ? "ABC" : "Sym";
			if (!label) {
				character[0] = key_value(ui, key);
				label = character;
			}
			x = left + (width - (int)strlen(label) * 16) / 2;
			for (; *label; ++label, x += 16)
				glyph(ui, x, top + 10, (unsigned char)*label, false, active, 2);
			left += width;
		}
	}
}

int terminal_ui_key_at(struct terminal_ui *ui, int x, int y)
{
	const struct key *key;
	unsigned int row, index;
	int left = KEY_X, top;

	if (x < KEY_X || x >= KEY_X + 12 * KEY_UNIT || y < KEY_Y ||
	    y >= KEY_Y + 6 * KEY_HEIGHT)
		return TERMINAL_UI_KEY_NONE;
	row = (y - KEY_Y) / KEY_HEIGHT;
	top = KEY_Y + row * KEY_HEIGHT;
	if (y < top + 2 || y >= top + KEY_HEIGHT - 2)
		return TERMINAL_UI_KEY_NONE;
	for (index = 0; index < 12 && keyboard[row][index].units; ++index) {
		key = &keyboard[row][index];
		if (x >= left + 2 && x < left + key->units * KEY_UNIT - 2) {
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

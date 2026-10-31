/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef METALIO_TERMINAL_UI_H
#define METALIO_TERMINAL_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <linux/metalio_epd.h>

#define TERMINAL_UI_WIDTH 684
#define TERMINAL_UI_HEIGHT 1216
#define TERMINAL_UI_SAFE_MARGIN 10
#define TERMINAL_UI_CELL_WIDTH 16
#define TERMINAL_UI_CELL_HEIGHT 32
#define TERMINAL_UI_KEYBOARD_TOP 896
#define TERMINAL_UI_COLS ((TERMINAL_UI_WIDTH - 2 * TERMINAL_UI_SAFE_MARGIN) / TERMINAL_UI_CELL_WIDTH)
#define TERMINAL_UI_ROWS ((TERMINAL_UI_KEYBOARD_TOP - 2 * TERMINAL_UI_SAFE_MARGIN) / TERMINAL_UI_CELL_HEIGHT)

enum terminal_ui_key {
	TERMINAL_UI_KEY_NONE = 0,
	TERMINAL_UI_KEY_ENTER = -1,
	TERMINAL_UI_KEY_TAB = -2,
	TERMINAL_UI_KEY_BACKSPACE = -3,
	TERMINAL_UI_KEY_ESCAPE = -4,
	TERMINAL_UI_KEY_UP = -5,
	TERMINAL_UI_KEY_DOWN = -6,
	TERMINAL_UI_KEY_LEFT = -7,
	TERMINAL_UI_KEY_RIGHT = -8,
	TERMINAL_UI_KEY_CTRL_C = -9,
	TERMINAL_UI_KEY_MODIFIER = -10,
};

struct terminal_ui {
	/* Native landscape 1216x684, MSB first, black=1; write directly to EPD. */
	unsigned char frame[METALIO_EPD_FRAME_BYTES];
	bool shift;
	bool symbols;
};

void terminal_ui_init(struct terminal_ui *ui);
/* Clears the frame while preserving keyboard modifiers. */
void terminal_ui_clear(struct terminal_ui *ui);
void terminal_ui_put_cell(struct terminal_ui *ui, int row, int col,
			  uint32_t codepoint, bool bold, bool inverse);
/* Inverts a static underline; call once after repainting the terminal cells. */
void terminal_ui_cursor(struct terminal_ui *ui, int row, int col);
void terminal_ui_draw_keyboard(struct terminal_ui *ui);
/* Portrait touch pixels: ASCII>0, enum<0, 0 outside keys; modifiers toggle here. */
int terminal_ui_key_at(struct terminal_ui *ui, int x, int y);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <sys/ioctl.h>
#include <unistd.h>
#undef _DEFAULT_SOURCE /* terminal.c declares this again; libc may normalize it to 1. */

static ssize_t test_write(int fd, const void *bytes, size_t length);
static int test_ioctl(int fd, unsigned long request, void *argument);

#define write test_write
#define ioctl(fd, request, argument) test_ioctl((fd), (request), (void *)(argument))
#define main terminal_program_main
#include "../apps/terminal.c"
#undef main
#undef ioctl
#undef write

#define TEST_EPD_FD (-2)
#define TEST_INPUT_FD (-3)
static unsigned int epd_writes, epd_requests, epd_mode;
static unsigned char epd_frame[METALIO_EPD_FRAME_BYTES];
static int epd_failure;
static bool input_touching, input_enter, input_refresh;
static int input_x, input_y, input_failure;

static struct terminal *fixture;

static void destroy_fixture(void)
{
	if (!fixture)
		return;
	if (fixture->child > 0) {
		kill(-fixture->child, SIGKILL);
		kill(fixture->child, SIGKILL);
		while (waitpid(fixture->child, NULL, 0) < 0 && errno == EINTR) {}
	}
	if (fixture->master >= 0)
		close(fixture->master);
	if (fixture->input >= 0)
		close(fixture->input);
	if (fixture->vt)
		vterm_free(fixture->vt);
	free(fixture);
	fixture = NULL;
}

static void failed(const char *condition, int line)
{
	fprintf(stderr, "terminal test line %d: %s (errno=%d: %s)\n",
		line, condition, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

#define CHECK(condition) do { if (!(condition)) failed(#condition, __LINE__); } while (0)

static ssize_t test_write(int fd, const void *bytes, size_t length)
{
	if (fd != TEST_EPD_FD)
		return write(fd, bytes, length);
	CHECK(bytes && length == METALIO_EPD_FRAME_BYTES);
	++epd_writes;
	if (epd_failure == 1) {
		errno = EIO;
		return -1;
	}
	if (epd_failure == 2)
		return (ssize_t)length - 1;
	memcpy(epd_frame, bytes, length);
	return (ssize_t)length;
}

static int test_ioctl(int fd, unsigned long request, void *argument)
{
	if (fd == TEST_INPUT_FD) {
		unsigned char *keys = argument;
		struct input_absinfo *axis = argument;
		if ((input_failure == 1 && request == EVIOCGKEY((KEY_MAX + 8) / 8)) ||
		    (input_failure == 2 && request == EVIOCGABS(ABS_X)) ||
		    (input_failure == 3 && request == EVIOCGABS(ABS_Y))) {
			errno = EIO;
			return -1;
		}
		if (request == EVIOCGKEY((KEY_MAX + 8) / 8)) {
			memset(keys, 0, (KEY_MAX + 8) / 8);
			if (input_touching)
				keys[BTN_TOUCH / 8] |= 1U << (BTN_TOUCH % 8);
			if (input_enter)
				keys[KEY_ENTER / 8] |= 1U << (KEY_ENTER % 8);
			if (input_refresh)
				keys[KEY_REFRESH / 8] |= 1U << (KEY_REFRESH % 8);
			return (KEY_MAX + 8) / 8;
		} else {
			CHECK(request == EVIOCGABS(ABS_X) || request == EVIOCGABS(ABS_Y));
			memset(axis, 0, sizeof(*axis));
			axis->value = request == EVIOCGABS(ABS_X) ? input_x : input_y;
		}
		return 0;
	}
	if (fd != TEST_EPD_FD)
		return ioctl(fd, request, argument);
	CHECK(request == METALIO_EPD_REFRESH && epd_writes == epd_requests + 1);
	epd_mode = ((struct metalio_epd_refresh *)argument)->mode;
	++epd_requests;
	if (epd_failure == 3) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static struct terminal *new_fixture(void)
{
	destroy_fixture();
	fixture = calloc(1, sizeof(*fixture));
	CHECK(fixture);
	fixture->master = fixture->epd = -1;
	fixture->input = TEST_INPUT_FD;
	fixture->child = -1;
	input_touching = input_enter = input_refresh = false;
	input_x = input_y = input_failure = 0;
	CHECK(parser_init(fixture) == 0);
	return fixture;
}

static void feed(struct terminal *term, const char *bytes)
{
	size_t length = strlen(bytes);
	CHECK(vterm_input_write(term->vt, bytes, length) == length);
	vterm_screen_flush_damage(term->screen);
}

static VTermScreenCell cell_at(struct terminal *term, int row, int col)
{
	VTermScreenCell cell = {0};
	VTermPos position = { .row = row, .col = col };
	CHECK(vterm_screen_get_cell(term->screen, position, &cell));
	return cell;
}

static bool blank_at(struct terminal *term, int row, int col)
{
	uint32_t value = cell_at(term, row, col).chars[0];
	return !value || value == ' ';
}

static void cursor_at(struct terminal *term, int row, int col)
{
	VTermPos position;
	vterm_state_get_cursorpos(vterm_obtain_state(term->vt), &position);
	CHECK(position.row == row && position.col == col);
}

static void test_parser(void)
{
	struct terminal *term = new_fixture();
	feed(term, "hello\r\nworld");
	CHECK(cell_at(term, 0, 0).chars[0] == 'h');
	CHECK(cell_at(term, 1, 0).chars[0] == 'w');
	cursor_at(term, 1, 5);
	feed(term, "\033[1;3HX");
	CHECK(cell_at(term, 0, 2).chars[0] == 'X');
	CHECK(cell_at(term, 0, 3).chars[0] == 'l');
	cursor_at(term, 0, 3);
	feed(term, "\033[2;3H\033[K");
	CHECK(cell_at(term, 1, 1).chars[0] == 'o');
	CHECK(blank_at(term, 1, 2) && blank_at(term, 1, 4));
	feed(term, "\033[2J");
	for (int row = 0; row < TERMINAL_UI_ROWS; ++row)
		for (int col = 0; col < TERMINAL_UI_COLS; ++col)
			CHECK(blank_at(term, row, col));
	cursor_at(term, 1, 2); /* ED must not silently home the cursor. */
	feed(term, "\033[?25l");
	CHECK(!term->cursor_visible);
	feed(term, "\033[?25h\033[H\033[1;7mB\033[0mN");
	CHECK(term->cursor_visible);
	CHECK(cell_at(term, 0, 0).attrs.bold && cell_at(term, 0, 0).attrs.reverse);
	CHECK(!cell_at(term, 0, 1).attrs.bold && !cell_at(term, 0, 1).attrs.reverse);

	vterm_screen_reset(term->screen, 1);
	for (int line = 0; line < TERMINAL_UI_ROWS + 2; ++line) {
		char text[16];
		snprintf(text, sizeof(text), "%02d\r\n", line);
		feed(term, text);
	}
	CHECK(cell_at(term, 0, 0).chars[0] == '0');
	CHECK(cell_at(term, 0, 1).chars[0] == '3');
	CHECK(blank_at(term, TERMINAL_UI_ROWS - 1, 0));
	cursor_at(term, TERMINAL_UI_ROWS - 1, 0);
	feed(term, "\033[2S");
	CHECK(cell_at(term, 0, 1).chars[0] == '5');
	CHECK(blank_at(term, TERMINAL_UI_ROWS - 2, 0));
}

static void test_unicode(void)
{
	struct terminal *term = new_fixture();
	const char text[] = "╭─╮│╰─╯✓✗⚠…•→█⠋";
	const uint32_t codepoints[] = {
		0x256d, 0x2500, 0x256e, 0x2502, 0x2570, 0x2500, 0x256f,
		0x2713, 0x2717, 0x26a0, 0x2026, 0x2022, 0x2192, 0x2588, 0x280b,
	};
	const int count = sizeof(codepoints) / sizeof(codepoints[0]);
	struct terminal_ui *expected = calloc(1, sizeof(*expected));
	CHECK(expected);
	for (int col = 0; col < count; ++col)
		terminal_ui_put_cell(expected, 0, col, codepoints[col], false, false);
	terminal_ui_draw_keyboard(expected);
	for (int split = 0; split < 2; ++split) {
		vterm_screen_reset(term->screen, 1);
		feed(term, "\033[?25l");
		if (split) {
			for (size_t i = 0; i < sizeof(text) - 1; ++i) {
				char byte[] = { text[i], 0 };
				feed(term, byte);
			}
		} else {
			feed(term, text);
		}
		for (int col = 0; col < count; ++col) {
			CHECK(cell_at(term, 0, col).chars[0] == codepoints[col]);
			CHECK(cell_at(term, 0, col).width == 1);
		}
		cursor_at(term, 0, count);
		paint(term);
		CHECK(!memcmp(expected->frame, term->ui.frame, sizeof(expected->frame)));
	}
	feed(term, "\033[1;41H╭─");
	CHECK(cell_at(term, 0, 40).chars[0] == 0x256d);
	CHECK(cell_at(term, 1, 0).chars[0] == 0x2500);
	cursor_at(term, 1, 1);
	feed(term, "\033[2J\033[H\033[1;7m✓\033[0m");
	CHECK(cell_at(term, 0, 0).attrs.bold && cell_at(term, 0, 0).attrs.reverse);
	terminal_ui_clear(expected);
	terminal_ui_put_cell(expected, 0, 0, 0x2713, true, true);
	terminal_ui_draw_keyboard(expected);
	paint(term);
	CHECK(!memcmp(expected->frame, term->ui.frame, sizeof(expected->frame)));
	free(expected);
}

static void expect_key(struct terminal *term, int value, const char *bytes, size_t length)
{
	term->queued = 0;
	key(term, value);
	CHECK(term->queued == length);
	CHECK(!memcmp(term->outgoing, bytes, length));
}

static void test_keys_and_queue(void)
{
	struct terminal *term = new_fixture();
	expect_key(term, 'a', "a", 1);
	expect_key(term, TERMINAL_UI_KEY_ENTER, "\r", 1);
	expect_key(term, TERMINAL_UI_KEY_TAB, "\t", 1);
	expect_key(term, TERMINAL_UI_KEY_BACKSPACE, "\177", 1);
	expect_key(term, TERMINAL_UI_KEY_ESCAPE, "\033", 1);
	expect_key(term, TERMINAL_UI_KEY_CTRL_C, "\003", 1);
	expect_key(term, TERMINAL_UI_KEY_UP, "\033[A", 3);
	expect_key(term, TERMINAL_UI_KEY_DOWN, "\033[B", 3);
	expect_key(term, TERMINAL_UI_KEY_RIGHT, "\033[C", 3);
	expect_key(term, TERMINAL_UI_KEY_LEFT, "\033[D", 3);
	expect_key(term, TERMINAL_UI_KEY_NONE, "", 0);
	expect_key(term, TERMINAL_UI_KEY_MODIFIER, "", 0);
	feed(term, "\033[?1h");
	expect_key(term, TERMINAL_UI_KEY_UP, "\033OA", 3);
	feed(term, "\033[?1l");
	term->queued = sizeof(term->outgoing) - 1;
	output("x", 1, term);
	CHECK(term->queued == sizeof(term->outgoing) && !term->error);
	output("y", 1, term);
	CHECK(term->queued == sizeof(term->outgoing) && term->error == ENOBUFS);
	CHECK(term->outgoing[sizeof(term->outgoing) - 1] == 'x');
}

static void test_refresh_coalescing(void)
{
	struct terminal *term = new_fixture();
	term->terminal_dirty = true;
	term->first_change = 1000;
	term->last_change = 1100;
	CHECK(!refresh_due(term, 1219));
	CHECK(refresh_due(term, 1220));
	term->last_change = 1900;
	CHECK(!refresh_due(term, 1999));
	CHECK(refresh_due(term, 2000)); /* Continuous output still gets a frame. */
	term->busy = true;
	CHECK(!refresh_due(term, 5000));
	term->busy = false;
	term->terminal_dirty = false;
	CHECK(!refresh_due(term, 5000));
	changed(term, false);
	CHECK(term->terminal_dirty && term->first_change == term->last_change);
	term->first_change = 123;
	changed(term, false);
	CHECK(term->first_change == 123);
}

static void event(struct terminal *term, unsigned short type,
		  unsigned short code, int value)
{
	struct input_event item = { .type = type, .code = code, .value = value };
	if (type == EV_ABS && code == ABS_X)
		input_x = value;
	if (type == EV_ABS && code == ABS_Y)
		input_y = value;
	if (type == EV_KEY && code == BTN_TOUCH)
		input_touching = value != 0;
	if (type == EV_KEY && code == KEY_ENTER && value != 2)
		input_enter = value != 0;
	if (type == EV_KEY && code == KEY_REFRESH && value != 2)
		input_refresh = value != 0;
	CHECK(touch_event(term, &item) == 0);
}

static void contact(struct terminal *term, int x, int y, bool down)
{
	event(term, EV_ABS, ABS_X, x);
	event(term, EV_ABS, ABS_Y, y);
	event(term, EV_KEY, BTN_TOUCH, down);
	event(term, EV_SYN, SYN_REPORT, 0);
}

static void test_touch(void)
{
	struct terminal *term = new_fixture();
	const int keyboard_y = TERMINAL_UI_KEYBOARD_TOP + 30;
	contact(term, 34, keyboard_y, true);
	CHECK(term->queued == 1 && term->outgoing[0] == '1');
	contact(term, 90, keyboard_y, true);
	CHECK(term->queued == 1); /* Moving across keys must not repeat. */
	contact(term, 90, keyboard_y, false);
	contact(term, 90, keyboard_y, true);
	CHECK(term->queued == 2 && term->outgoing[1] == '2');
	contact(term, 90, keyboard_y, false);
	contact(term, 30, 30, true);
	CHECK(term->queued == 2);
	contact(term, 30, 30, false);
	term->terminal_dirty = term->keyboard_dirty = false;
	/* A press in the 6px gap cannot type or turn into a press by sliding. */
	contact(term, 75, keyboard_y, true);
	CHECK(term->queued == 2 && !term->keyboard_dirty);
	contact(term, 90, keyboard_y, true);
	CHECK(term->queued == 2 && !term->keyboard_dirty);
	contact(term, 90, keyboard_y, false);
	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 280, true);
	CHECK(term->ui.shift && term->keyboard_dirty && !term->terminal_dirty && term->queued == 2);
	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 280, false);
	term->ui.shift = false;
	event(term, EV_SYN, SYN_DROPPED, 0);
	contact(term, 34, keyboard_y, true);
	CHECK(term->dropped && term->queued == 2);
	contact(term, 34, keyboard_y, false);
	CHECK(!term->dropped);
	contact(term, 34, keyboard_y, true);
	CHECK(term->queued == 3 && term->outgoing[2] == '1');

	int descriptors[2];
	CHECK(pipe(descriptors) == 0);
	term->input = descriptors[0];
	CHECK(fcntl(term->input, F_SETFL, O_NONBLOCK) == 0);
	CHECK(read_touch(term) == 0); /* No events yet is not a device failure. */
	CHECK(write(descriptors[1], "bad", 3) == 3);
	errno = 0;
	CHECK(read_touch(term) == -1 && errno == EIO);
	close(descriptors[1]);
	errno = 0;
	CHECK(read_touch(term) == -1 && errno == EIO);
}

static struct terminal *display_fixture(void)
{
	struct terminal *term = new_fixture();
	term->epd = TEST_EPD_FD;
	term->terminal_dirty = false;
	epd_writes = epd_requests = 0;
	epd_failure = 0;
	return term;
}

static void complete_refresh(struct terminal *term)
{
	CHECK(term->busy);
	term->busy = false;
	refresh_completed(term);
}

static struct terminal *displayed_fixture(unsigned int modes)
{
	struct terminal *term = display_fixture();
	term->supported_modes = modes;
	feed(term, "old terminal");
	changed(term, false);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
	complete_refresh(term);
	return term;
}

static void check_terminal_pixels(const unsigned char *first, const unsigned char *second)
{
	/* Portrait y maps to native x; the keyboard starts on a native byte boundary. */
	CHECK(TERMINAL_UI_KEYBOARD_TOP % 8 == 0);
	for (unsigned int row = 0; row < METALIO_EPD_HEIGHT; ++row)
		CHECK(!memcmp(first + row * METALIO_EPD_STRIDE,
			      second + row * METALIO_EPD_STRIDE, TERMINAL_UI_KEYBOARD_TOP / 8));
}

static void test_keys_without_feedback(void)
{
	const int keyboard_y = TERMINAL_UI_KEYBOARD_TOP + 30;
	struct terminal *term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
	unsigned char *baseline = malloc(sizeof(term->ui.frame));
	CHECK(baseline);
	memcpy(baseline, term->ui.frame, sizeof(term->ui.frame));

	/* Hidden input and quick taps send bytes without any display requests. */
	contact(term, 34, keyboard_y, true);
	contact(term, 90, keyboard_y, true);
	contact(term, 90, keyboard_y, false);
	contact(term, 90, keyboard_y, true);
	contact(term, 90, keyboard_y, false);
	CHECK(term->queued == 2 && !memcmp(term->outgoing, "12", 2));
	CHECK(!term->terminal_dirty && !term->keyboard_dirty);
	CHECK(!refresh_due(term, milliseconds() + 100000) && epd_requests == 1);
	paint(term);
	CHECK(!memcmp(baseline, term->ui.frame, sizeof(term->ui.frame)));

	/* Ctrl-C also has no visual press/release cycle. Holding never repeats. */
	contact(term, 144, keyboard_y + 5 * 79, true);
	contact(term, 240, keyboard_y + 5 * 79, true);
	contact(term, 240, keyboard_y + 5 * 79, false);
	CHECK(term->queued == 3 && term->outgoing[2] == '\003');
	CHECK(!refresh_due(term, milliseconds() + 100000) && epd_requests == 1);

	/* Input during a scan cannot mutate the frame or schedule highlight scans. */
	feed(term, "\033[Hfirst echo");
	changed(term, false);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_DU);
	memcpy(baseline, epd_frame, sizeof(term->ui.frame));
	contact(term, 34, keyboard_y, true);
	contact(term, 34, keyboard_y, false);
	CHECK(term->queued == 4 && term->outgoing[3] == '1');
	CHECK(!memcmp(baseline, term->ui.frame, sizeof(term->ui.frame)));
	CHECK(!term->keyboard_dirty && !refresh_due(term, milliseconds() + 100000));
	complete_refresh(term);
	CHECK(!refresh_due(term, milliseconds() + 100000) && epd_requests == 2);
	feed(term, "\033[Hlatest echo");
	changed(term, false);
	CHECK(refresh_due(term, term->last_change + 120));
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_DU && epd_requests == 3);
	complete_refresh(term);
	CHECK(!refresh_due(term, milliseconds() + 100000));
	free(baseline);
}

static void test_layer_updates(void)
{
	const int shift_y = TERMINAL_UI_KEYBOARD_TOP + 280;
	struct terminal *term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
	unsigned char *baseline = malloc(sizeof(term->ui.frame));
	CHECK(baseline);
	memcpy(baseline, term->ui.frame, sizeof(term->ui.frame));
	contact(term, 34, shift_y, true);
	CHECK(term->ui.shift && !term->queued && term->keyboard_dirty);
	CHECK(refresh_due(term, term->last_change));
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_FEEDBACK);
	check_terminal_pixels(baseline, epd_frame);
	CHECK(memcmp(baseline, epd_frame, sizeof(term->ui.frame)));
	complete_refresh(term);
	contact(term, 34, shift_y, false);
	CHECK(term->ui.shift && !term->keyboard_dirty);
	CHECK(!refresh_due(term, milliseconds() + 100000));

	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 359, true);
	CHECK(term->ui.symbols && term->ui.shift && !term->queued);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_FEEDBACK);
	check_terminal_pixels(baseline, epd_frame);
	/* Output received during a layer scan is included in the next DU frame. */
	feed(term, "\033[Houtput during layer update");
	changed(term, false);
	CHECK(!memcmp(epd_frame, term->ui.frame, sizeof(term->ui.frame)));
	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 359, false);
	complete_refresh(term);
	CHECK(refresh_due(term, term->last_change + 120));
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_DU);
	CHECK(!term->terminal_dirty && !term->keyboard_dirty);
	complete_refresh(term);
	CHECK(!refresh_due(term, milliseconds() + 100000));

	/* Pending terminal output takes priority; no intermediate keyboard frame. */
	for (unsigned int step = 0; step < 3; ++step) {
		feed(term, "\033[Hlatest output");
		changed(term, false);
		contact(term, 34, shift_y, true);
		CHECK(refresh_due(term, term->last_change));
		CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_DU);
		CHECK(!term->terminal_dirty && !term->keyboard_dirty);
		contact(term, 34, shift_y, false);
		complete_refresh(term);
		CHECK(!refresh_due(term, milliseconds() + 100000));
	}
	free(baseline);
}

static void test_follow_capabilities_and_gc(void)
{
	const int keyboard_y = TERMINAL_UI_KEYBOARD_TOP + 280;
	const unsigned int capabilities[] = { 0, (1U << METALIO_EPD_DU) | (1U << METALIO_EPD_GC) };
	for (unsigned int i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); ++i) {
		struct terminal *term = displayed_fixture(capabilities[i]);
		contact(term, 34, keyboard_y, true);
		CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_DU);
		complete_refresh(term);
	}
	struct terminal *term = display_fixture();
	term->supported_modes = 1U << METALIO_EPD_FEEDBACK;
	contact(term, 34, keyboard_y, true);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC); /* First frame is never FOLLOW. */
	complete_refresh(term);
	term->updates = 119;
	contact(term, 34, keyboard_y, false);
	contact(term, 34, keyboard_y, true);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_FEEDBACK);
	CHECK(term->updates == 119);
	complete_refresh(term);
	CHECK(term->updates == 120);
	CHECK(!refresh_due(term, term->last_change + 100000)); /* No idle GC. */
	feed(term, "\033[Hperiodic GC");
	changed(term, false);
	contact(term, 34, keyboard_y, false);
	contact(term, 34, keyboard_y, true);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
	CHECK(!term->terminal_dirty && !term->keyboard_dirty);
	complete_refresh(term);
	CHECK(term->updates == 121);
	changed(term, false);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_DU);
	complete_refresh(term);
	term->updates = 240;
	changed(term, false);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
	complete_refresh(term);
	CHECK(term->updates == 241);
}

static void test_failed_refresh_keeps_pending_state(void)
{
	const unsigned int counts[] = { 1, 119, 120 };
	for (unsigned int count = 0; count < sizeof(counts) / sizeof(counts[0]); ++count)
	for (int failure = 1; failure <= 3; ++failure) {
		struct terminal *term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
		term->updates = counts[count];
		feed(term, "pending");
		changed(term, false);
		contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 280, true);
		epd_failure = failure;
		errno = 0;
		CHECK(refresh(term) == -1 && errno == EIO);
		CHECK(!term->busy && term->updates == counts[count]);
		CHECK(term->terminal_dirty && term->keyboard_dirty);
	}
}

static void physical_refresh(struct terminal *term, int value)
{
	event(term, EV_KEY, KEY_REFRESH, value);
	event(term, EV_SYN, SYN_REPORT, 0);
}

static void physical_enter(struct terminal *term, int value)
{
	event(term, EV_KEY, KEY_ENTER, value);
	event(term, EV_SYN, SYN_REPORT, 0);
}

static void test_physical_key_independence(void)
{
	struct terminal *term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
	physical_enter(term, 1);
	CHECK(term->enter_down && !term->refresh_down && !term->pending_full_refreshes);
	CHECK(term->queued == 1 && term->outgoing[0] == '\r');
	physical_enter(term, 1);
	physical_enter(term, 2);
	CHECK(term->queued == 1 && !term->keyboard_dirty);
	physical_refresh(term, 1);
	CHECK(term->enter_down && term->refresh_down && term->pending_full_refreshes == 1);
	physical_enter(term, 0);
	physical_enter(term, 2);
	CHECK(!term->enter_down && term->refresh_down && term->queued == 1);
	physical_enter(term, 1);
	CHECK(term->queued == 2 && term->outgoing[1] == '\r' && term->pending_full_refreshes == 1);
	physical_refresh(term, 0);
	physical_refresh(term, 1);
	CHECK(term->enter_down && term->refresh_down && term->pending_full_refreshes == 2);

	/* Both buttons in one input report remain independent while a frame is busy. */
	term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
	term->busy = true;
	event(term, EV_KEY, KEY_REFRESH, 1);
	event(term, EV_KEY, KEY_ENTER, 1);
	event(term, EV_SYN, SYN_REPORT, 0);
	CHECK(term->refresh_down && term->enter_down && term->pending_full_refreshes == 1);
	CHECK(term->queued == 1 && term->outgoing[0] == '\r' && !refresh_due(term, milliseconds()));
	physical_refresh(term, 2);
	physical_enter(term, 2);
	CHECK(term->queued == 1 && term->pending_full_refreshes == 1);
	physical_refresh(term, 0);
	physical_refresh(term, 1);
	physical_enter(term, 0);
	physical_enter(term, 1);
	CHECK(term->pending_full_refreshes == 2 && term->queued == 2);
	term->busy = false;
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
	CHECK(term->pending_full_refreshes == 1 && term->queued == 2);
}

static void test_two_key_resync(void)
{
	struct terminal *term = display_fixture();
	physical_enter(term, 1);
	physical_refresh(term, 1);
	event(term, EV_SYN, SYN_DROPPED, 0);
	event(term, EV_SYN, SYN_REPORT, 0);
	CHECK(term->dropped && term->enter_down && term->refresh_down);
	CHECK(term->queued == 1 && term->pending_full_refreshes == 1);
	physical_enter(term, 0);
	CHECK(term->dropped && !term->enter_down && term->refresh_down);
	physical_refresh(term, 1);
	CHECK(term->queued == 1 && term->pending_full_refreshes == 1);
	physical_refresh(term, 0);
	CHECK(!term->dropped && !term->enter_down && !term->refresh_down);
	physical_enter(term, 2);
	physical_refresh(term, 2);
	CHECK(term->queued == 1 && term->pending_full_refreshes == 1);
	physical_enter(term, 1);
	physical_refresh(term, 1);
	CHECK(term->queued == 2 && term->pending_full_refreshes == 2);

	/* Releasing refresh first cannot re-arm a held Enter after lost events. */
	event(term, EV_SYN, SYN_DROPPED, 0);
	physical_refresh(term, 0);
	CHECK(term->dropped && term->enter_down && !term->refresh_down);
	physical_enter(term, 1);
	CHECK(term->queued == 2 && term->pending_full_refreshes == 2);
	physical_enter(term, 0);
	CHECK(!term->dropped && !term->enter_down && !term->refresh_down);
}

static void test_physical_full_refresh(void)
{
	struct terminal *term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
	physical_refresh(term, 1);
	CHECK(term->refresh_down && !term->touching);
	CHECK(term->pending_full_refreshes == 1 && !term->queued);
	CHECK(!term->terminal_dirty && !term->keyboard_dirty);
	physical_refresh(term, 1); /* Duplicate down and kernel repeat never add requests. */
	physical_refresh(term, 2);
	CHECK(term->pending_full_refreshes == 1);
	CHECK(refresh_due(term, term->last_change));
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
	CHECK(!term->pending_full_refreshes);
	complete_refresh(term);
	physical_refresh(term, 1); /* A held button cannot trigger another full refresh. */
	physical_refresh(term, 2);
	CHECK(!refresh_due(term, term->last_change + 100000));
	physical_refresh(term, 0);
	physical_refresh(term, 2); /* An isolated repeat cannot synthesize a new press. */
	CHECK(!term->refresh_down && !term->pending_full_refreshes);
	physical_refresh(term, 1);
	physical_refresh(term, 0);
	physical_refresh(term, 1);
	physical_refresh(term, 0);
	CHECK(term->pending_full_refreshes == 2 && !term->queued);
	for (unsigned int remaining = 2; remaining; --remaining) {
		CHECK(refresh_due(term, term->last_change));
		CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
		CHECK(term->pending_full_refreshes == remaining - 1);
		complete_refresh(term);
	}
	CHECK(epd_requests == 4 && !refresh_due(term, term->last_change + 100000));

	/* On-screen Enter sends Return without press/release redraws. */
	physical_refresh(term, 1);
	contact(term, 620, TERMINAL_UI_KEYBOARD_TOP + 30 + 5 * 79, true);
	CHECK(term->queued == 1 && term->outgoing[0] == '\r');
	CHECK(term->touching && term->refresh_down);
	CHECK(!term->keyboard_dirty && term->pending_full_refreshes == 1);
	physical_refresh(term, 1);
	contact(term, 620, TERMINAL_UI_KEYBOARD_TOP + 30 + 5 * 79, true);
	CHECK(term->queued == 1 && term->pending_full_refreshes == 1);
	contact(term, 620, TERMINAL_UI_KEYBOARD_TOP + 30 + 5 * 79, false);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
	complete_refresh(term);
	CHECK(!term->keyboard_dirty && !refresh_due(term, milliseconds() + 100000));
}

static void test_full_refresh_during_refresh(void)
{
	struct terminal *term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
	unsigned char *previous = malloc(sizeof(term->ui.frame));
	CHECK(previous);
	feed(term, "pending output");
	changed(term, false);
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_DU);
	memcpy(previous, term->ui.frame, sizeof(term->ui.frame));
	physical_refresh(term, 1);
	physical_refresh(term, 0);
	physical_refresh(term, 1);
	physical_refresh(term, 0);
	CHECK(term->pending_full_refreshes == 2 && !term->queued && !term->keyboard_dirty);
	CHECK(!refresh_due(term, term->last_change + 100000));
	CHECK(!memcmp(previous, term->ui.frame, sizeof(term->ui.frame)));
	complete_refresh(term);
	CHECK(refresh_due(term, term->last_change));
	CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
	CHECK(term->pending_full_refreshes == 1);
	physical_refresh(term, 1);
	physical_refresh(term, 0);
	CHECK(term->pending_full_refreshes == 2);
	complete_refresh(term);
	for (unsigned int remaining = 2; remaining; --remaining) {
		CHECK(refresh_due(term, term->last_change));
		CHECK(refresh(term) == 0 && epd_mode == METALIO_EPD_GC);
		CHECK(term->pending_full_refreshes == remaining - 1);
		complete_refresh(term);
	}
	CHECK(epd_requests == 5 && !refresh_due(term, term->last_change + 100000));
	free(previous);

	/* A failed write or ioctl must leave the manual request pending. */
	for (int failure = 1; failure <= 3; ++failure) {
		term = displayed_fixture(1U << METALIO_EPD_FEEDBACK);
		physical_refresh(term, 1);
		physical_refresh(term, 0);
		epd_failure = failure;
		errno = 0;
		CHECK(refresh(term) == -1 && errno == EIO);
		CHECK(!term->busy && term->pending_full_refreshes == 1);
		CHECK(refresh_due(term, term->last_change));
	}
}

static void test_input_resync(void)
{
	struct terminal *term = display_fixture();
	physical_refresh(term, 1);
	event(term, EV_SYN, SYN_DROPPED, 0);
	physical_refresh(term, 1);
	CHECK(term->dropped && term->refresh_down && !term->queued && term->pending_full_refreshes == 1);
	physical_refresh(term, 0); /* No BTN_TOUCH event is needed to recover. */
	CHECK(!term->dropped && !term->refresh_down && !term->touching);
	physical_refresh(term, 1);
	CHECK(!term->queued && term->pending_full_refreshes == 2);
	physical_refresh(term, 0);

	/* A stale touch or bottom key keeps both sources suppressed until released. */
	event(term, EV_SYN, SYN_DROPPED, 0);
	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 30, true);
	CHECK(term->dropped && term->touching && term->was_touching);
	CHECK(term->touch_x == 34 && term->touch_y == TERMINAL_UI_KEYBOARD_TOP + 30);
	physical_refresh(term, 1);
	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 30, false);
	CHECK(term->dropped && !term->touching && term->refresh_down && !term->queued && term->pending_full_refreshes == 2);
	physical_refresh(term, 0);
	CHECK(!term->dropped);
	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 30, true);
	CHECK(term->queued == 1 && term->outgoing[0] == '1' && term->pending_full_refreshes == 2);
	contact(term, 34, TERMINAL_UI_KEYBOARD_TOP + 30, false);

	for (int failure = 1; failure <= 3; ++failure) {
		struct input_event report = { .type = EV_SYN, .code = SYN_REPORT };
		event(term, EV_SYN, SYN_DROPPED, 0);
		input_failure = failure;
		errno = 0;
		CHECK(touch_event(term, &report) == -1 && errno == EIO);
		CHECK(term->dropped && term->queued == 1 && term->pending_full_refreshes == 2);
		input_failure = 0;
		event(term, EV_SYN, SYN_REPORT, 0);
		CHECK(!term->dropped);
	}

	/* Snapshot failures must reach the event-loop caller, rather than press a key. */
	int descriptors[2];
	CHECK(pipe(descriptors) == 0);
	term->input = descriptors[0];
	struct input_event lost[] = {
		{ .type = EV_SYN, .code = SYN_DROPPED },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};
	CHECK(write(descriptors[1], lost, sizeof(lost)) == (ssize_t)sizeof(lost));
	errno = 0;
	CHECK(read_touch(term) == -1 && errno == ENOTTY);
	CHECK(term->dropped && term->queued == 1 && term->pending_full_refreshes == 2);
	close(descriptors[1]);
}

static void send_queued(struct terminal *term)
{
	uint64_t deadline = milliseconds() + 3000;
	while (term->queued) {
		ssize_t length = write(term->master, term->outgoing, term->queued);
		if (length > 0) {
			term->queued -= (size_t)length;
			memmove(term->outgoing, term->outgoing + length, term->queued);
		} else {
			CHECK(length < 0 && (errno == EAGAIN || errno == EINTR));
			struct pollfd item = { .fd = term->master, .events = POLLOUT };
			CHECK(milliseconds() < deadline && poll(&item, 1, 100) >= 0);
		}
	}
}

static void send_text(struct terminal *term, const char *text)
{
	for (; *text; ++text)
		key(term, (unsigned char)*text);
	send_queued(term);
}

static void send_key(struct terminal *term, int value)
{
	key(term, value);
	send_queued(term);
}

static void expect_pty(struct terminal *term, const char *expected)
{
	char received[32768] = {0};
	size_t used = 0;
	uint64_t deadline = milliseconds() + 5000;
	while (!strstr(received, expected)) {
		struct pollfd item = { .fd = term->master, .events = POLLIN };
		if (milliseconds() >= deadline)
			fprintf(stderr, "PTY expected '%s', received '%s'\n", expected, received);
		CHECK(milliseconds() < deadline);
		int result = poll(&item, 1, 100);
		if (result < 0 && errno == EINTR)
			continue;
		CHECK(result >= 0);
		if (!(item.revents & POLLIN))
			continue;
		CHECK(used + 1 < sizeof(received));
		ssize_t length = read(term->master, received + used, sizeof(received) - used - 1);
		if (length < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		CHECK(length > 0);
		CHECK(vterm_input_write(term->vt, received + used, (size_t)length) == (size_t)length);
		vterm_screen_flush_damage(term->screen);
		used += (size_t)length;
		received[used] = 0;
	}
}

static void test_real_pty(void)
{
	struct terminal *term = new_fixture();
	CHECK(start_shell(term) == 0);
	CHECK(term->child > 0 && (fcntl(term->master, F_GETFL) & O_NONBLOCK));
	struct winsize size;
	CHECK(ioctl(term->master, TIOCGWINSZ, &size) == 0);
	CHECK(size.ws_row == TERMINAL_UI_ROWS && size.ws_col == TERMINAL_UI_COLS);
	/* The escaped marker cannot accidentally match the command's input echo. */
	send_text(term, "stty -echo erase '^?'; printf '__%s__\\n' READY");
	send_key(term, TERMINAL_UI_KEY_ENTER);
	expect_pty(term, "__READY__\r\n");
	send_text(term, "printf 'BACKX");
	send_key(term, TERMINAL_UI_KEY_BACKSPACE);
	send_text(term, "SPACE\\n'");
	send_key(term, TERMINAL_UI_KEY_ENTER);
	expect_pty(term, "BACKSPACE\r\n");
	send_text(term, "printf 'RUNNING\\n'; sleep 30");
	send_key(term, TERMINAL_UI_KEY_ENTER);
	expect_pty(term, "RUNNING\r\n");
	struct timespec pause = { .tv_nsec = 50000000 };
	nanosleep(&pause, NULL);
	send_key(term, TERMINAL_UI_KEY_CTRL_C);
	send_text(term, "printf 'INTERRUPTED\\n'");
	send_key(term, TERMINAL_UI_KEY_ENTER);
	expect_pty(term, "INTERRUPTED\r\n");

	/* METALIO_TEST_ASH=1 exercises the target's existing interactive BusyBox shell. */
	const char *ash_setting = getenv("METALIO_TEST_ASH");
	bool target_ash = ash_setting && !strcmp(ash_setting, "1");
	if (target_ash || access("/bin/bash", X_OK) == 0) {
		if (target_ash) {
			/* BusyBox skips line editing when ICANON is set but ECHO is clear. */
			send_text(term, "stty echo; printf '__%s__\\n' EDITING_READY");
			send_key(term, TERMINAL_UI_KEY_ENTER);
			expect_pty(term, "__EDITING_READY__\r\n");
		} else {
			send_text(term, "exec /bin/bash --noprofile --norc -i");
			send_key(term, TERMINAL_UI_KEY_ENTER);
		}
		send_text(term, "PS1=''; printf 'HISTORY_OK\\n'");
		send_key(term, TERMINAL_UI_KEY_ENTER);
		expect_pty(term, "HISTORY_OK\r\n");
		send_key(term, TERMINAL_UI_KEY_UP);
		send_key(term, TERMINAL_UI_KEY_ENTER);
		expect_pty(term, "HISTORY_OK\r\n");
		send_text(term, "__metalio_unique_completion() { printf 'TAB_OK\\n'; }");
		send_key(term, TERMINAL_UI_KEY_ENTER);
		send_text(term, "__metalio_unique_comp");
		send_key(term, TERMINAL_UI_KEY_TAB);
		send_key(term, TERMINAL_UI_KEY_ENTER);
		expect_pty(term, "TAB_OK\r\n");
	} else {
		puts("Host bash unavailable: history/Tab PTY checks skipped; key encoding was checked.");
	}
}

int main(void)
{
	CHECK(atexit(destroy_fixture) == 0);
	test_parser();
	test_unicode();
	test_keys_and_queue();
	test_refresh_coalescing();
	test_touch();
	test_keys_without_feedback();
	test_layer_updates();
	test_follow_capabilities_and_gc();
	test_failed_refresh_keeps_pending_state();
	test_physical_key_independence();
	test_two_key_resync();
	test_physical_full_refresh();
	test_full_refresh_during_refresh();
	test_input_resync();
	test_real_pty();
	destroy_fixture();
	puts("terminal parser, keys, refresh, evdev, and real PTY checks passed");
	return 0;
}

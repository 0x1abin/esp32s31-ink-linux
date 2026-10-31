/* SPDX-License-Identifier: GPL-2.0-only */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <vterm.h>
#include "terminal-ui.h"

struct terminal {
	struct terminal_ui ui;
	VTerm *vt;
	VTermScreen *screen;
	int master, epd, input, error;
	pid_t child;
	bool terminal_dirty, keyboard_dirty, busy, cursor_visible, touching, was_touching, dropped;
	bool enter_down, refresh_down;
	int touch_x, touch_y;
	uint64_t first_change, last_change;
	unsigned int updates, supported_modes;
	unsigned int pending_full_refreshes;
	char outgoing[65536];
	size_t queued;
};

static volatile sig_atomic_t stopped;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	stopped = 1;
}

static uint64_t milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + now.tv_nsec / 1000000U;
}

static void changed(struct terminal *term, bool keyboard)
{
	uint64_t now = milliseconds();
	if (!term->terminal_dirty && !term->keyboard_dirty)
		term->first_change = now;
	term->last_change = now;
	if (keyboard)
		term->keyboard_dirty = true;
	else
		term->terminal_dirty = true;
}

static void output(const char *bytes, size_t length, void *user)
{
	struct terminal *term = user;
	if (length > sizeof(term->outgoing) - term->queued) {
		term->error = ENOBUFS;
		return;
	}
	memcpy(term->outgoing + term->queued, bytes, length);
	term->queued += length;
}

static int term_property(VTermProp property, VTermValue *value, void *user)
{
	struct terminal *term = user;
	if (property == VTERM_PROP_CURSORVISIBLE)
		term->cursor_visible = value->boolean;
	return 1;
}

static const VTermScreenCallbacks screen_callbacks = {
	.settermprop = term_property,
};

static int parser_init(struct terminal *term)
{
	term->vt = vterm_new(TERMINAL_UI_ROWS, TERMINAL_UI_COLS);
	if (!term->vt) {
		errno = ENOMEM;
		return -1;
	}
	vterm_set_utf8(term->vt, 1);
	vterm_output_set_callback(term->vt, output, term);
	term->screen = vterm_obtain_screen(term->vt);
	if (!term->screen) {
		errno = ENOMEM;
		return -1;
	}
	vterm_screen_set_callbacks(term->screen, &screen_callbacks, term);
	term->cursor_visible = true;
	vterm_screen_reset(term->screen, 1);
	terminal_ui_init(&term->ui);
	changed(term, false);
	return 0;
}

static int start_shell(struct terminal *term)
{
	char slave_name[128];
	struct winsize size = { .ws_row = TERMINAL_UI_ROWS, .ws_col = TERMINAL_UI_COLS };
	term->master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (term->master < 0 || grantpt(term->master) || unlockpt(term->master))
		return -1;
	char *name = ptsname(term->master);
	if (!name || strlen(name) >= sizeof(slave_name)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(slave_name, name);
	if (ioctl(term->master, TIOCSWINSZ, &size))
		return -1;
	term->child = fork();
	if (term->child < 0)
		return -1;
	if (!term->child) {
		int slave;
		if (setsid() < 0)
			_exit(126);
		slave = open(slave_name, O_RDWR);
		if (slave < 0 || ioctl(slave, TIOCSCTTY, 0))
			_exit(126);
		for (int fd = 0; fd < 3; ++fd)
			if (dup2(slave, fd) < 0)
				_exit(126);
		if (slave > 2)
			close(slave);
		close(term->master);
		setenv("TERM", "xterm", 1);
		setenv("HOME", "/root", 1);
		setenv("LC_ALL", "C", 1);
		setenv("HISTFILE", "/dev/null", 1);
		execl("/bin/sh", "sh", "-i", (char *)NULL);
		_exit(127);
	}
	return fcntl(term->master, F_SETFL, O_NONBLOCK);
}

static void key(struct terminal *term, int value)
{
	VTermKey special = VTERM_KEY_NONE;
	switch (value) {
	case TERMINAL_UI_KEY_ENTER: special = VTERM_KEY_ENTER; break;
	case TERMINAL_UI_KEY_TAB: special = VTERM_KEY_TAB; break;
	case TERMINAL_UI_KEY_BACKSPACE: special = VTERM_KEY_BACKSPACE; break;
	case TERMINAL_UI_KEY_ESCAPE: special = VTERM_KEY_ESCAPE; break;
	case TERMINAL_UI_KEY_UP: special = VTERM_KEY_UP; break;
	case TERMINAL_UI_KEY_DOWN: special = VTERM_KEY_DOWN; break;
	case TERMINAL_UI_KEY_LEFT: special = VTERM_KEY_LEFT; break;
	case TERMINAL_UI_KEY_RIGHT: special = VTERM_KEY_RIGHT; break;
	case TERMINAL_UI_KEY_CTRL_C:
		vterm_keyboard_unichar(term->vt, 'c', VTERM_MOD_CTRL);
		break;
	default:
		if (value > 0)
			vterm_keyboard_unichar(term->vt, (uint32_t)value, VTERM_MOD_NONE);
		break;
	}
	if (special != VTERM_KEY_NONE)
		vterm_keyboard_key(term->vt, special, VTERM_MOD_NONE);
}

static int resync_input(struct terminal *term)
{
	unsigned char keys[(KEY_MAX + 8) / 8] = {0};
	struct input_absinfo x, y;

	if (ioctl(term->input, EVIOCGKEY(sizeof(keys)), keys) < 0 ||
	    ioctl(term->input, EVIOCGABS(ABS_X), &x) < 0 ||
	    ioctl(term->input, EVIOCGABS(ABS_Y), &y) < 0)
		return -1;
	term->touch_x = x.value;
	term->touch_y = y.value;
	term->touching = term->was_touching = !!(keys[BTN_TOUCH / 8] & (1U << (BTN_TOUCH % 8)));
	term->enter_down = !!(keys[KEY_ENTER / 8] & (1U << (KEY_ENTER % 8)));
	term->refresh_down = !!(keys[KEY_REFRESH / 8] & (1U << (KEY_REFRESH % 8)));
	/* A lost packet cannot turn a held contact into a new command. */
	term->dropped = term->touching || term->enter_down || term->refresh_down;
	return 0;
}

static int touch_event(struct terminal *term, const struct input_event *event)
{
	if (event->type == EV_SYN && event->code == SYN_DROPPED) {
		term->dropped = true;
		return 0;
	}
	if (term->dropped) {
		return event->type == EV_SYN && event->code == SYN_REPORT ? resync_input(term) : 0;
	}
	if (event->type == EV_ABS) {
		if (event->code == ABS_X)
			term->touch_x = event->value;
		if (event->code == ABS_Y)
			term->touch_y = event->value;
	}
	if (event->type == EV_KEY && event->code == BTN_TOUCH)
		term->touching = event->value != 0;
	if (event->type == EV_KEY && event->code == KEY_ENTER) {
		if (event->value == 1 && !term->enter_down) {
			term->enter_down = true;
			key(term, TERMINAL_UI_KEY_ENTER);
		} else if (!event->value)
			term->enter_down = false;
	}
	if (event->type == EV_KEY && event->code == KEY_REFRESH) {
		if (event->value == 1 && !term->refresh_down) {
			term->refresh_down = true;
			++term->pending_full_refreshes;
		} else if (!event->value)
			term->refresh_down = false;
	}
	if (event->type == EV_SYN && event->code == SYN_REPORT) {
		if (term->touching && !term->was_touching) {
			int value = terminal_ui_key_at(&term->ui, term->touch_x, term->touch_y);
			if (value == TERMINAL_UI_KEY_MODIFIER)
				changed(term, true);
			else
				key(term, value);
		}
		term->was_touching = term->touching;
	}
	return 0;
}

static int read_touch(struct terminal *term)
{
	struct input_event events[32];
	ssize_t length = read(term->input, events, sizeof(events));
	if (length < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	if (!length || length % sizeof(events[0])) {
		errno = EIO;
		return -1;
	}
	for (size_t i = 0; i < (size_t)length / sizeof(events[0]); ++i)
		if (touch_event(term, &events[i]))
			return -1;
	return 0;
}

static void paint(struct terminal *term)
{
	VTermPos cursor;
	terminal_ui_clear(&term->ui);
	for (int row = 0; row < TERMINAL_UI_ROWS; ++row)
		for (int col = 0; col < TERMINAL_UI_COLS; ++col) {
			VTermScreenCell cell = {0};
			VTermPos position = { .row = row, .col = col };
			vterm_screen_get_cell(term->screen, position, &cell);
			terminal_ui_put_cell(&term->ui, row, col,
				cell.attrs.conceal ? ' ' : cell.chars[0],
				cell.attrs.bold, cell.attrs.reverse);
		}
	vterm_state_get_cursorpos(vterm_obtain_state(term->vt), &cursor);
	if (term->cursor_visible)
		terminal_ui_cursor(&term->ui, cursor.row, cursor.col);
	terminal_ui_draw_keyboard(&term->ui);
}

static bool refresh_due(const struct terminal *term, uint64_t now)
{
	if (term->pending_full_refreshes)
		return !term->busy;
	return (term->terminal_dirty || term->keyboard_dirty) && !term->busy &&
		(term->keyboard_dirty ||
		 now - term->last_change >= 120 || now - term->first_change >= 1000);
}

static void refresh_completed(struct terminal *term)
{
	++term->updates;
}

static int refresh(struct terminal *term)
{
	struct metalio_epd_refresh request = {
		.mode = term->pending_full_refreshes || !(term->updates % 120) ?
			METALIO_EPD_GC : METALIO_EPD_DU,
	};
	if (request.mode != METALIO_EPD_GC &&
	    (term->supported_modes & (1U << METALIO_EPD_FEEDBACK)) && term->keyboard_dirty &&
	    !term->terminal_dirty)
		request.mode = METALIO_EPD_FEEDBACK;
	/* Layer-only changes keep the last completed terminal pixels and cursor. */
	if (request.mode == METALIO_EPD_FEEDBACK)
		terminal_ui_draw_keyboard(&term->ui);
	else
		paint(term);
	ssize_t length = write(term->epd, term->ui.frame, sizeof(term->ui.frame));
	if (length != (ssize_t)sizeof(term->ui.frame)) {
		if (length >= 0)
			errno = EIO;
		return -1;
	}
	if (ioctl(term->epd, METALIO_EPD_REFRESH, &request))
		return -1;
	if (term->pending_full_refreshes)
		--term->pending_full_refreshes;
	term->busy = true;
	if (request.mode != METALIO_EPD_FEEDBACK)
		term->terminal_dirty = false;
	term->keyboard_dirty = false;
	return 0;
}

static int display_init(struct terminal *term)
{
	struct metalio_epd_info info;
	struct metalio_epd_status status;
	term->epd = open("/dev/metalio-epd", O_RDWR | O_CLOEXEC);
	if (term->epd < 0 || ioctl(term->epd, METALIO_EPD_GET_INFO, &info) ||
	    ioctl(term->epd, METALIO_EPD_GET_STATUS, &status))
		return -1;
	if (info.abi_version != METALIO_EPD_ABI_VERSION ||
	    info.width != METALIO_EPD_WIDTH || info.height != METALIO_EPD_HEIGHT ||
	    info.stride != METALIO_EPD_STRIDE || info.frame_bytes != sizeof(term->ui.frame)) {
		errno = EPROTO;
		return -1;
	}
	term->supported_modes = info.supported_modes;
	if (status.busy) {
		errno = EBUSY;
		return -1;
	}
	term->input = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (term->input < 0)
		return -1;
	struct input_absinfo x, y;
	if (ioctl(term->input, EVIOCGABS(ABS_X), &x) ||
	    ioctl(term->input, EVIOCGABS(ABS_Y), &y))
		return -1;
	if (x.minimum != 0 || y.minimum != 0 || x.maximum != TERMINAL_UI_WIDTH - 1 ||
	    y.maximum != TERMINAL_UI_HEIGHT - 1) {
		fprintf(stderr, "touch range must be portrait 0..683 x 0..1215; adjust device tree\n");
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int run(struct terminal *term)
{
	while (!stopped && !term->error) {
		struct pollfd fds[] = {
			{ term->master, POLLIN | (term->queued ? POLLOUT : 0), 0 },
			{ term->input, POLLIN, 0 },
			{ term->epd, term->busy ? POLLIN : 0, 0 },
		};
		if (poll(fds, 3, 40) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (fds[1].revents & POLLIN)
			if (read_touch(term))
				return -1;
		if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			errno = ENODEV;
			return -1;
		}
		if (fds[0].revents & POLLOUT) {
			ssize_t length = write(term->master, term->outgoing, term->queued);
			if (length > 0) {
				term->queued -= (size_t)length;
				memmove(term->outgoing, term->outgoing + length, term->queued);
			} else if (length < 0 && errno != EAGAIN && errno != EINTR)
				return -1;
		}
		if (fds[0].revents & POLLIN) {
			char bytes[4096];
			ssize_t length = read(term->master, bytes, sizeof(bytes));
			if (length > 0) {
				if (vterm_input_write(term->vt, bytes, (size_t)length) != (size_t)length) {
					errno = EIO;
					return -1;
				}
				vterm_screen_flush_damage(term->screen);
				changed(term, false);
			} else if (!length || (errno != EAGAIN && errno != EINTR))
				return length < 0 && errno != EIO ? -1 : 0;
		}
		if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
			return 0;
		if (fds[2].revents & (POLLIN | POLLERR)) {
			struct metalio_epd_status status;
			if (ioctl(term->epd, METALIO_EPD_GET_STATUS, &status))
				return -1;
			if (status.last_error) {
				errno = status.last_error < 0 ? -status.last_error : EIO;
				return -1;
			}
			term->busy = status.busy;
			if (!term->busy)
				refresh_completed(term);
		}
		if (fds[2].revents & (POLLHUP | POLLNVAL)) {
			errno = ENODEV;
			return -1;
		}
		uint64_t now = milliseconds();
		if (refresh_due(term, now) && refresh(term))
			return -1;
	}
	if (term->error) {
		errno = term->error;
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static struct terminal term = { .master = -1, .epd = -1, .input = -1, .child = -1 };
	int result = 1;
	(void)argv;
	if (argc != 1) {
		fprintf(stderr, "Usage: metalio-terminal\n");
		return 2;
	}
	struct sigaction action = { .sa_handler = stop_handler };
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (parser_init(&term) || display_init(&term) || start_shell(&term) || run(&term))
		perror("metalio-terminal");
	else
		result = 0;
	if (term.epd >= 0)
		close(term.epd);
	if (term.input >= 0)
		close(term.input);
	if (term.master >= 0)
		close(term.master);
	if (term.child > 0) {
		kill(-term.child, SIGKILL);
		kill(term.child, SIGKILL);
		while (waitpid(term.child, NULL, 0) < 0 && errno == EINTR) {}
	}
	if (term.vt)
		vterm_free(term.vt);
	return result;
}

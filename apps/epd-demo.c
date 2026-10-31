/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/metalio_epd.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static unsigned char frame[METALIO_EPD_FRAME_BYTES];

/* Five columns, low bit at the top. Lowercase uses the uppercase glyphs. */
static const struct {
	char character;
	unsigned char columns[5];
} glyphs[] = {
	{' ', {0,0,0,0,0}}, {'!', {0,0,95,0,0}},
	{'-', {8,8,8,8,8}}, {'.', {0,96,96,0,0}},
	{':', {0,54,54,0,0}}, {'/', {32,16,8,4,2}},
	{'?', {2,1,81,9,6}}, {'_', {64,64,64,64,64}},
	{'0', {62,81,73,69,62}}, {'1', {0,66,127,64,0}},
	{'2', {66,97,81,73,70}}, {'3', {33,65,69,75,49}},
	{'4', {24,20,18,127,16}}, {'5', {39,69,69,69,57}},
	{'6', {60,74,73,73,48}}, {'7', {1,113,9,5,3}},
	{'8', {54,73,73,73,54}}, {'9', {6,73,73,41,30}},
	{'A', {126,17,17,17,126}}, {'B', {127,73,73,73,54}},
	{'C', {62,65,65,65,34}}, {'D', {127,65,65,34,28}},
	{'E', {127,73,73,73,65}}, {'F', {127,9,9,9,1}},
	{'G', {62,65,73,73,122}}, {'H', {127,8,8,8,127}},
	{'I', {0,65,127,65,0}}, {'J', {32,64,65,63,1}},
	{'K', {127,8,20,34,65}}, {'L', {127,64,64,64,64}},
	{'M', {127,2,12,2,127}}, {'N', {127,4,8,16,127}},
	{'O', {62,65,65,65,62}}, {'P', {127,9,9,9,6}},
	{'Q', {62,65,81,33,94}}, {'R', {127,9,25,41,70}},
	{'S', {70,73,73,73,49}}, {'T', {1,1,127,1,1}},
	{'U', {63,64,64,64,63}}, {'V', {31,32,64,32,31}},
	{'W', {63,64,56,64,63}}, {'X', {99,20,8,20,99}},
	{'Y', {7,8,112,8,7}}, {'Z', {97,81,73,69,67}},
};

static void black_pixel(unsigned int x, unsigned int y)
{
	frame[y * METALIO_EPD_STRIDE + x / 8U] |= 128U >> (x % 8U);
}

static int draw_text(const char *text)
{
	unsigned int x = 48, y = 48;
	const unsigned int scale = 4;

	for (; *text; ++text) {
		unsigned char ch = (unsigned char)*text;
		size_t glyph;
		unsigned int column, row, dx, dy;

		if (ch == '\n' || x + 5U * scale > METALIO_EPD_WIDTH - 48U) {
			x = 48;
			y += 10U * scale;
			if (ch == '\n')
				continue;
		}
		if (ch < 32 || ch > 126 || y + 7U * scale > METALIO_EPD_HEIGHT - 48U) {
			errno = EINVAL;
			return -1;
		}
		if (ch >= 'a' && ch <= 'z')
			ch -= 'a' - 'A';
		for (glyph = 0; glyph < sizeof(glyphs) / sizeof(glyphs[0]); ++glyph)
			if (glyphs[glyph].character == ch)
				break;
		if (glyph == sizeof(glyphs) / sizeof(glyphs[0]))
			glyph = 6; /* Unsupported punctuation is visibly a question mark. */
		for (column = 0; column < 5; ++column)
			for (row = 0; row < 7; ++row)
				if (glyphs[glyph].columns[column] & (1U << row))
					for (dy = 0; dy < scale; ++dy)
						for (dx = 0; dx < scale; ++dx)
							black_pixel(x + column * scale + dx,
								    y + row * scale + dy);
		x += 6U * scale;
	}
	return 0;
}

static int preview(const char *path)
{
	FILE *stream = !strcmp(path, "-") ? stdout : fopen(path, "wb");
	int failed;

	if (!stream)
		return -1;
	failed = fprintf(stream, "P4\n%u %u\n", METALIO_EPD_WIDTH,
			 METALIO_EPD_HEIGHT) < 0;
	if (!failed)
		failed = fwrite(frame, 1, sizeof(frame), stream) != sizeof(frame);
	if (stream == stdout)
		failed |= fflush(stream) != 0;
	else
		failed |= fclose(stream) != 0;
	if (failed && !errno)
		errno = EIO;
	return failed ? -1 : 0;
}

static int display(const char *device, unsigned int mode)
{
	struct metalio_epd_info info = {0};
	struct metalio_epd_status status;
	struct metalio_epd_refresh request = { .mode = mode };
	struct pollfd pollfd;
	ssize_t written;
	int fd, result, saved_errno;

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ioctl(fd, METALIO_EPD_GET_INFO, &info) < 0)
		goto error;
	if (info.abi_version != METALIO_EPD_ABI_VERSION ||
	    info.width != METALIO_EPD_WIDTH || info.height != METALIO_EPD_HEIGHT ||
	    info.stride != METALIO_EPD_STRIDE || info.frame_bytes != sizeof(frame)) {
		errno = EPROTO;
		goto error;
	}
	if (!((info.supported_modes ? info.supported_modes :
	       (1U << METALIO_EPD_DU) | (1U << METALIO_EPD_GC)) & (1U << mode))) {
		fprintf(stderr, "Requested refresh mode is not supported by this kernel\n");
		errno = EOPNOTSUPP;
		goto error;
	}
	if (ioctl(fd, METALIO_EPD_GET_STATUS, &status) < 0)
		goto error;
	if (status.busy) {
		errno = EBUSY;
		goto error;
	}
	do {
		written = write(fd, frame, sizeof(frame));
	} while (written < 0 && errno == EINTR);
	if (written != (ssize_t)sizeof(frame)) {
		if (written >= 0)
			errno = EIO;
		goto error;
	}
	if (ioctl(fd, METALIO_EPD_REFRESH, &request) < 0)
		goto error;
	pollfd = (struct pollfd){ .fd = fd, .events = POLLIN };
	do {
		do {
			result = poll(&pollfd, 1, 30000);
		} while (result < 0 && errno == EINTR);
		if (result <= 0) {
			if (!result)
				errno = ETIMEDOUT;
			goto error;
		}
		if (pollfd.revents & (POLLNVAL | POLLHUP)) {
			errno = EIO;
			goto error;
		}
		if (ioctl(fd, METALIO_EPD_GET_STATUS, &status) < 0)
			goto error;
		if (status.last_error) {
			errno = status.last_error < 0 ? -status.last_error : EIO;
			goto error;
		}
	} while (status.busy);
	printf("Refresh completed: sequence=%u elapsed=%u ms\n", status.sequence,
	       status.elapsed_ms);
	return close(fd);

error:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s white|black|checker|text [--mode gc|du|feedback] "
		"[--device PATH | --output PBM] [--text ASCII]\n"
		"Default: /dev/metalio-epd, complete GC refresh. PBM bypasses hardware.\n"
		"DU uses DU14 (legacy: DU20); feedback requires FOLLOW8 support.\n"
		"Text uses uppercase glyphs; unsupported punctuation appears as ?.\n", program);
}

int main(int argc, char **argv)
{
	const char *device = "/dev/metalio-epd", *output = NULL;
	const char *text = "METALIO E-INK4 PLUS\nLINUX DISPLAY DEMO\n1216 X 684 - E0470A14-AF\n"
		"0123456789\nABCDEFGHIJKLMNOPQRSTUVWXYZ";
	unsigned int mode = METALIO_EPD_GC, x, y;
	int i, device_set = 0, text_set = 0;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	for (i = 2; i < argc; i += 2) {
		if (i + 1 == argc)
			goto invalid;
		if (!strcmp(argv[i], "--device")) {
			device = argv[i + 1];
			device_set = 1;
		} else if (!strcmp(argv[i], "--output")) {
			output = argv[i + 1];
		} else if (!strcmp(argv[i], "--text")) {
			text = argv[i + 1];
			text_set = 1;
		} else if (!strcmp(argv[i], "--mode")) {
			if (!strcmp(argv[i + 1], "gc"))
				mode = METALIO_EPD_GC;
			else if (!strcmp(argv[i + 1], "du"))
				mode = METALIO_EPD_DU;
			else if (!strcmp(argv[i + 1], "feedback"))
				mode = METALIO_EPD_FEEDBACK;
			else
				goto invalid;
		} else {
			goto invalid;
		}
	}
	if ((output && device_set) || (text_set && strcmp(argv[1], "text")))
		goto invalid;
	if (!strcmp(argv[1], "black")) {
		memset(frame, 255, sizeof(frame));
	} else if (!strcmp(argv[1], "checker")) {
		for (y = 0; y < METALIO_EPD_HEIGHT; ++y)
			for (x = 0; x < METALIO_EPD_WIDTH; ++x)
				if (((x / 32U) + (y / 32U)) % 2U)
					black_pixel(x, y);
	} else if (!strcmp(argv[1], "text")) {
		if (draw_text(text)) {
			fprintf(stderr, "Text must be printable ASCII and fit on the panel\n");
			return 2;
		}
	} else if (strcmp(argv[1], "white")) {
		goto invalid;
	}
	if ((output ? preview(output) : display(device, mode)) < 0) {
		perror(output ? output : device);
		return 1;
	}
	return 0;

invalid:
	usage(argv[0]);
	return 2;
}

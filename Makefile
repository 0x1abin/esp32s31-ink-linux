CC ?= cc
CFLAGS ?= -O2

.PHONY: all demo test
all: demo

demo: outputs/host/metalio-epd-demo

outputs/host/metalio-epd-demo: apps/epd-demo.c include/uapi/linux/metalio_epd.h
	mkdir -p outputs/host
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -Iinclude/uapi $< -o $@

test:
	CC="$(CC)" ./scripts/test.sh

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Iinclude
LDFLAGS_WIN = -lgdi32 -luser32

SRC = src/main.c src/config.c src/image.c src/stitch.c src/capture.c src/png_io.c src/platform_win32.c

.PHONY: all windows clean test_stitch

all: windows

windows:
	$(CC) $(CFLAGS) -o scroll_capture.exe $(SRC) $(LDFLAGS_WIN)

clean:
	rm -f scroll_capture.exe stitch_test

test_stitch: src/image.c src/stitch.c tests/stitch_test.c
	$(CC) $(CFLAGS) -DTEST_STITCH -o stitch_test src/image.c src/stitch.c tests/stitch_test.c -lm
	./stitch_test

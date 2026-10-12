# cfspeed - C build
#
# libcurl is the only dependency. It is used behind a thin transport layer so
# the measurement engine never talks to it directly.

CC      ?= cc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=
LDLIBS  := -lcurl -lm

# Winsock for name resolution and address formatting on Windows. Detected from
# the compiler rather than `uname`, which MSYS2 does not always provide.
ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32 -lpthread
endif

SRC := src/main.c src/term.c src/net.c src/engine.c src/ui.c \
       src/history.c src/views.c src/cli.c \
       src/stats.c src/chart.c src/fmt.c src/keymap.c src/actions.c
OBJ := $(SRC:.c=.o)
BIN := cfspeed

HDRS := src/term.h src/net.h src/engine.h src/ui.h src/theme.h src/env_size.h \
       src/history.h src/views.h src/cli.h src/stats.h src/chart.h \
       src/fmt.h src/keymap.h src/actions.h

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# The test binary links only the modules with no libcurl dependency, so it
# builds and runs anywhere without the transport layer.
TESTSRC := tests/test_units.c src/test.c src/stats.c src/chart.c src/fmt.c src/actions.c

test: $(TESTSRC)
	$(CC) $(CFLAGS) -o test_units $^ $(LDFLAGS) -lm
	./test_units

clean:
	rm -f $(OBJ) $(BIN) test_units

.PHONY: all clean test
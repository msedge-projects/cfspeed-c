# cfspeed - C build
#
# libcurl is the only dependency. It is used behind a thin transport layer so
# the measurement engine never talks to it directly.

CC      ?= cc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=
LDLIBS  :=

# The Windows build uses WinHTTP, the HTTP stack that ships with Windows. That
# is what makes it dependency-free: the libcurl build needed libcurl plus an
# OpenSSL stack behind a 155 KB program, which meant 19 files and 14.4 MB on
# disk. WinHTTP also reads the Windows trust store natively, so no CA bundle
# has to be shipped either.
#
# The POSIX build keeps libcurl. The choice is made once at build time; net.h
# is identical either way and the engine never sees a transport.
ifeq ($(OS),Windows_NT)
  NETSRC := src/netwin.c
  LDLIBS += -lwinhttp -lws2_32 -lpthread   # winhttp for HTTP, winsock for getaddrinfo
else
  NETSRC := src/net.c
  LDLIBS += -lcurl -lm
endif

SRC := src/main.c src/term.c src/engine.c src/ui.c \
       src/history.c src/views.c src/cli.c \
       src/stats.c src/chart.c src/fmt.c src/keymap.c src/actions.c \
       src/menu.c $(NETSRC)
OBJ := $(SRC:.c=.o)
BIN := cfspeed

HDRS := src/term.h src/net.h src/engine.h src/ui.h src/theme.h src/env_size.h \
       src/history.h src/views.h src/cli.h src/stats.h src/chart.h \
       src/fmt.h src/keymap.h src/actions.h src/menu.h src/netwin.c

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# The pure modules - stats, chart, fmt, actions - carry no libcurl dependency,
# so the suite builds and runs without a network.
TESTMODS := src/test.c src/stats.c src/chart.c src/fmt.c src/actions.c
TESTSRC := tests/test_units.c $(TESTMODS)
HISTTEST := tests/test_history.c src/test.c src/history.c

test: $(TESTSRC)
	$(CC) $(CFLAGS) -Isrc -o test_units $^ $(LDFLAGS) -lm
	./test_units

test-history: $(HISTTEST)
	$(CC) $(CFLAGS) -Isrc -o test_history $^ $(LDFLAGS) -lm
	./test_history

VIEWSMODS := src/views.c src/term.c src/history.c src/fmt.c src/chart.c src/stats.c

test-views: tests/test_views_render.c src/test.c $(VIEWSMODS)
	$(CC) $(CFLAGS) -Isrc -o test_views $^ $(LDFLAGS) -lm
	./test_views

clean:
	rm -f $(OBJ) $(BIN) test_units test_history test_views

.PHONY: all clean test test-history test-views
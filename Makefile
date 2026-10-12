# cfspeed - C build
#
# libcurl is the only dependency. It is used behind a thin transport layer so
# the measurement engine never talks to it directly.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=
LDLIBS  := -lcurl -lm

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(UNAME_S),Windows)
  LDLIBS += -lpthread
endif

SRC := src/main.c src/term.c src/net.c src/engine.c src/ui.c
OBJ := $(SRC:.c=.o)
BIN := cfspeed

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c src/term.h src/net.h src/engine.h src/ui.h src/theme.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
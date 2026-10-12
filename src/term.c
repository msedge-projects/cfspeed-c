#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <termios.h>
#  include <unistd.h>
#  include <sys/ioctl.h>
#  include <time.h>
#endif

/* ------------------------------------------------------------------ state */

static Screen *g_front = NULL; /* what the terminal currently shows */
static Screen *g_back = NULL;  /* what we are building this frame */
static int g_alt_screen = 0;

#if defined(_WIN32)
static HANDLE g_in = INVALID_HANDLE_VALUE;
static DWORD  g_old_in = 0;
static DWORD  g_old_out = 0;
#else
static struct termios g_old_term;
#endif

/* ------------------------------------------------------------------ timing */

uint64_t now_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* ------------------------------------------------------------------ utf-8 */

int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ------------------------------------------------------------- lifecycle */

int term_size(int *w, int *h) {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) return -1;
    *w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    *h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return -1;
    *w = ws.ws_col;
    *h = ws.ws_row;
    if (*w == 0 || *h == 0) return -1;
#endif
    return 0;
}

int term_init(void) {
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", stdout);
    fflush(stdout);

#if defined(_WIN32)
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_in, &g_old_in) || !GetConsoleMode(out, &g_old_out)) return -1;

    /* ENABLE_VIRTUAL_TERMINAL_INPUT is what makes Windows emit real ANSI key
     * sequences, so the same parser works on all three platforms. */
    DWORD in_mode = g_old_in;
    in_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    in_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(g_in, in_mode)) return -1;

    DWORD out_mode = g_old_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(out, out_mode);
#else
    if (tcgetattr(STDIN_FILENO, &g_old_term) != 0) return -1;
    struct termios raw = g_old_term;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* 100ms read timeout, so poll() can return */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
#endif

    /* Mouse reporting: press and release only, no motion, which is all the UI
     * needs and keeps the escape traffic small. */
    fputs("\x1b[?1000h\x1b[?1006h", stdout);
    fflush(stdout);
    g_alt_screen = 1;

    int w, h;
    if (term_size(&w, &h) != 0) { w = 80; h = 24; }
    g_front = screen_create(w, h);
    g_back = screen_create(w, h);
    return (g_front && g_back) ? 0 : -1;
}

void term_shutdown(void) {
    if (g_alt_screen) {
        fputs("\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l", stdout);
        fflush(stdout);
        g_alt_screen = 0;
    }
#if defined(_WIN32)
    if (g_in != INVALID_HANDLE_VALUE) SetConsoleMode(g_in, g_old_in);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_old_out);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_term);
#endif
    screen_free(g_front);
    screen_free(g_back);
    g_front = g_back = NULL;
}

/* ----------------------------------------------------------------- input */

#if defined(_WIN32)
/* PeekConsoleInput gives us records rather than a byte stream on Windows; this
 * translates them into the same byte soup the POSIX path parses. */
static int win_read_byte(unsigned char *out) {
    INPUT_RECORD rec;
    DWORD got = 0;
    if (!PeekConsoleInput(g_in, &rec, 1, &got) || got == 0) {
        Sleep(15);
        if (!PeekConsoleInput(g_in, &rec, 1, &got) || got == 0) return 0;
    }
    ReadConsoleInput(g_in, &rec, 1, &got);
    if (rec.EventType != KEY_EVENT) return 0;
    if (!rec.Event.KeyEvent.bKeyDown) return 0;
    *out = (unsigned char)rec.Event.KeyEvent.uChar.AsciiChar;
    return 1;
}
#else
static int posix_read_byte(unsigned char *out) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n != 1) return 0;
    *out = c;
    return 1;
}
#endif

static int read_byte(unsigned char *out) {
#if defined(_WIN32)
    return win_read_byte(out);
#else
    return posix_read_byte(out);
#endif
}

/* Prefixed: <imm.h> already defines MOD_SHIFT/MOD_ALT on Windows. */
#define CFS_MOD_SHIFT 1
#define CFS_MOD_CTRL  2
#define CFS_MOD_ALT   4

Event term_poll(void) {
    Event ev;
    memset(&ev, 0, sizeof ev);
    unsigned char c;

    if (!read_byte(&c)) {
        /* A read timeout is not an error; the caller keeps its own clock. */
        int w, h;
        if (term_size(&w, &h) == 0 && g_back && (g_back->w != w || g_back->h != h)) {
            ev.type = EV_RESIZE;
        }
        return ev;
    }

    /* ESC introduces either a key or a sequence. */
    if (c != 0x1b) {
        if (c == 0x0d || c == 0x0a) { ev.type = EV_KEY; ev.key = KEY_ENTER; return ev; }
        if (c == 0x09) { ev.type = EV_KEY; ev.key = KEY_TAB; return ev; }
        if (c == 0x08 || c == 0x7f) { ev.type = EV_KEY; ev.key = KEY_BACKSPACE; return ev; }
        if (c < 0x20) return ev;
        ev.type = EV_KEY; ev.key = KEY_CHAR; ev.ch = (char)c;
        return ev;
    }

    if (!read_byte(&c)) { ev.type = EV_KEY; ev.key = KEY_ESC; return ev; }
    if (c != '[' && c != 'O') { ev.type = EV_KEY; ev.key = KEY_ESC; return ev; }
    if (!read_byte(&c)) { ev.type = EV_KEY; ev.key = KEY_ESC; return ev; }

    if (c == '<') {
        /* SGR mouse: ESC [ < button ; col ; row M|m. Only presses are used. */
        unsigned int b = 0, col = 0, row = 0;
        char tail = 0;
        if (!read_byte((unsigned char *)&b)) return ev;
        while (read_byte((unsigned char *)&c) && c >= '0' && c <= '9')
            col = col * 10 + (unsigned)(c - '0');
        if (!read_byte((unsigned char *)&c)) return ev;
        while (c >= '0' && c <= '9') { row = row * 10 + (unsigned)(c - '0'); read_byte((unsigned char *)&c); }
        tail = (char)c;
        if (tail == 'M' && (b & 0x20)) {
            ev.type = EV_MOUSE;
            ev.mx = (int)col - 1;
            ev.my = (int)row - 1;
            ev.mbtn = (int)(b & 0x03);
            if (b & 0x04) ev.mods |= CFS_MOD_SHIFT;
            if (b & 0x08) ev.mods |= CFS_MOD_ALT;
            if (b & 0x10) ev.mods |= CFS_MOD_CTRL;
        }
        return ev;
    }

    {
        int key = KEY_NONE;
        switch (c) {
            case 'A': key = KEY_UP;    break;
            case 'B': key = KEY_DOWN;  break;
            case 'C': key = KEY_RIGHT; break;
            case 'D': key = KEY_LEFT;  break;
            case 'H': key = KEY_HOME;  break;
            case 'F': key = KEY_END;   break;
            case '~': {
                unsigned char n = 0;
                if (!read_byte(&n)) return ev;
                switch (n) {
                    case '1': case '7': key = KEY_HOME;   break;
                    case '3': key = KEY_DELETE;           break;
                    case '5': key = KEY_PGUP;             break;
                    case '6': key = KEY_PGDN;             break;
                    case '8': key = KEY_END;              break;
                    default: break;
                }
                break;
            }
            default: break;
        }
        if (key != KEY_NONE) { ev.type = EV_KEY; ev.key = key; }
        return ev;
    }
}

/* ---------------------------------------------------------------- screen */

Screen *screen_create(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Screen *s = (Screen *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->cells = (Cell *)calloc((size_t)w * (size_t)h, sizeof(Cell));
    if (!s->cells) { free(s); return NULL; }
    screen_clear(s);
    return s;
}

void screen_free(Screen *s) {
    if (!s) return;
    free(s->cells);
    free(s);
}

void screen_clear(Screen *s) {
    for (int i = 0; i < s->w * s->h; i++) {
        s->cells[i].utf8[0] = ' ';
        s->cells[i].utf8[1] = '\0';
        s->cells[i].fg = FG_NONE;
        s->cells[i].bg = BG_NONE;
        s->cells[i].bold = 0;
        s->cells[i].underline = 0;
    }
}

void cell_put(Screen *s, int x, int y, const char *utf8, uint32_t fg, uint32_t bg,
              int bold, int underline) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    Cell *c = &s->cells[y * s->w + x];
    snprintf(c->utf8, sizeof c->utf8, "%s", utf8);
    c->fg = fg;
    c->bg = bg;
    c->bold = (uint8_t)(bold ? 1 : 0);
    c->underline = (uint8_t)(underline ? 1 : 0);
}

void cell_fill(Screen *s, int x, int y, int w, int h, uint32_t bg) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < s->w && j < s->h) {
                Cell *c = &s->cells[j * s->w + i];
                c->utf8[0] = ' ';
                c->utf8[1] = '\0';
                c->fg = FG_NONE;
                c->bg = bg;
                c->bold = 0;
                c->underline = 0;
            }
}

static int utf8_width_char(const char *p) {
    unsigned char c = (unsigned char)p[0];
    if (c == 0) return 0;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 1;
    if ((c & 0xF0) == 0xE0) return 1;
    return 2; /* four-byte forms occupy two columns in practice */
}

int cell_text(Screen *s, int x, int y, int max, const char *text, uint32_t fg,
              uint32_t bg, int bold) {
    int col = x;
    const char *p = text;
    while (*p && col < x + max) {
        int wch = utf8_width_char(p);
        int n = 1;
        if ((unsigned char)*p >= 0xF0) n = 4;
        else if ((unsigned char)*p >= 0xE0) n = 3;
        else if ((unsigned char)*p >= 0xC0) n = 2;
        char buf[5] = {0};
        memcpy(buf, p, (size_t)(n < 4 ? n : 4));
        if (col < x + max) cell_put(s, col, y, buf, fg, bg, bold, 0);
        col += wch;
        p += n;
    }
    return col - x;
}

/* ---------------------------------------------------------------- render */

static void emit_sgr(FILE *out, const Cell *c) {
    if (c->fg == FG_NONE && c->bg == BG_NONE && !c->bold && !c->underline) {
        fputs("\x1b[0m", out);
        return;
    }
    fputs("\x1b[0", out);
    if (c->bold) fputs(";1", out);
    if (c->underline) fputs(";4", out);
    if (c->fg != FG_NONE)
        fprintf(out, ";38;2;%u;%u;%u", (c->fg >> 16) & 0xFF, (c->fg >> 8) & 0xFF, c->fg & 0xFF);
    if (c->bg != BG_NONE)
        fprintf(out, ";48;2;%u;%u;%u", (c->bg >> 16) & 0xFF, (c->bg >> 8) & 0xFF, c->bg & 0xFF);
    fputs("m", out);
}

void frame_invalidate(void) {
    /* Force the next present to rewrite everything. */
    if (g_front) {
        for (int i = 0; i < g_front->w * g_front->h; i++)
            g_front->cells[i].utf8[0] = 0;
    }
}

void frame_present(Screen *s) {
    int w, h;
    if (term_size(&w, &h) == 0 && (w != g_front->w || h != g_front->h)) {
        /* Resize: adopt the new geometry and force a full repaint. */
        Screen *nf = screen_create(w, h);
        if (!nf) return;
        screen_free(g_front);
        g_front = nf;
        frame_invalidate();
        fputs("\x1b[2J\x1b[H", stdout);
        fflush(stdout);
        return;
    }

    FILE *out = stdout;
    int cur_x = -1, cur_y = -1;
    int dirty = 0;

    for (int y = 0; y < s->h && y < g_front->h; y++) {
        for (int x = 0; x < s->w && x < g_front->w; x++) {
            const Cell *c = &s->cells[y * s->w + x];
            const Cell *p = &g_front->cells[y * g_front->w + x];
            if (memcmp(c->utf8, p->utf8, 5) == 0 && c->fg == p->fg &&
                c->bg == p->bg && c->bold == p->bold && c->underline == p->underline)
                continue;
            dirty = 1;
            if (cur_y != y || cur_x != x) fprintf(out, "\x1b[%d;%dH", y + 1, x + 1);
            emit_sgr(out, c);
            fputs(c->utf8[0] ? c->utf8 : " ", out);
            cur_x = x + 1;
            cur_y = y;
        }
    }

    memcpy(g_front->cells, s->cells, (size_t)s->w * (size_t)s->h * sizeof(Cell));

    if (dirty) {
        fputs("\x1b[0m", out);
        fflush(out);
    }
}

/* Expose the back buffer so the UI can compose a frame. */
Screen *screen_back(void) { return g_back; }
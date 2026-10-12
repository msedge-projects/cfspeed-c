/* Terminal layer: raw mode, a cell buffer, and a diffing renderer.
 *
 * The renderer only writes cells that changed since the last frame, which is
 * what lets the UI sit at 0% CPU while idle: with nothing new to draw, the
 * frame is simply skipped and no bytes go out at all.
 */
#ifndef CFS_TERM_H
#define CFS_TERM_H

#include <stdint.h>
#include <stddef.h>

/* --- colours ---------------------------------------------------------------
 * Colours are 24-bit packed as 0xRRGGBB. A cell that paints no background
 * uses BG_NONE, so the terminal's own background - including a translucent or
 * image-backed one - shows through untouched.
 */
#define FG_NONE 0x1000000u
#define BG_NONE 0x1000001u

typedef struct {
    char     utf8[5]; /* NUL-terminated, up to one codepoint */
    uint32_t fg;
    uint32_t bg;
    uint8_t  bold;
    uint8_t  underline;
} Cell;

typedef struct {
    int   w, h;
    Cell *cells;
} Screen;

/* --- input ---------------------------------------------------------------- */
enum {
    EV_NONE = 0,
    EV_KEY,
    EV_MOUSE,
    EV_RESIZE,
    EV_QUIT
};

enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DELETE
};

typedef struct {
    int  type;
    int  key;    /* KEY_* */
    char ch;     /* for KEY_CHAR */
    int  mods;   /* bit0 shift, bit1 ctrl, bit2 alt */
    int  mx, my; /* EV_MOUSE: zero-based column and row */
    int  mbtn;   /* EV_MOUSE: 0 = left */
} Event;

/* --- lifecycle ------------------------------------------------------------ */
int  term_init(void);
void term_shutdown(void);
int  term_size(int *w, int *h);

/* Blocking read of the next event. Returns EV_NONE if stdin closed. */
Event term_poll(void);

/* --- frame ---------------------------------------------------------------- */
Screen *screen_create(int w, int h);
void    screen_free(Screen *s);
void    screen_clear(Screen *s);

void cell_put(Screen *s, int x, int y, const char *utf8, uint32_t fg, uint32_t bg,
              int bold, int underline);
void cell_fill(Screen *s, int x, int y, int w, int h, uint32_t bg);
/* Write `text` clipped to `max` columns; returns columns advanced. */
int  cell_text(Screen *s, int x, int y, int max, const char *text, uint32_t fg,
               uint32_t bg, int bold);

/* Render and flush the diff against the previous frame. */
void frame_present(Screen *s);
void frame_invalidate(void);

int  utf8_encode(uint32_t cp, char *out);

/* Monotonic milliseconds. */
uint64_t now_ms(void);

#endif /* CFS_TERM_H */
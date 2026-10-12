/* The actions menu: view only. See actions.h for why the model and the drawing
 * are separate files.
 *
 * The overlay is transparent by policy. Every cell the panel touches is written
 * with BG_NONE, so the terminal's own background - including a translucent or
 * image-backed one - shows through behind the menu exactly as it does behind
 * the rest of the app. The one exception is the selected row, which inverts the
 * way draw_nav's active tab does: T_LAT behind T_SURFACE. That is why the panel
 * is framed by thin rules rather than by a fill: the structure has to be
 * legible when there is no background to carry it.
 *
 * The rows are erased to blank cells at BG_NONE before the label goes down.
 * That clears whatever view was underneath without painting over it, which is
 * the difference between an overlay the user's terminal is still visible
 * through and one that merely claims to be.
 */
#include "menu.h"

#include "theme.h"

#define RULE_H "\xe2\x94\x80" /* U+2500 BOX DRAWINGS LIGHT HORIZONTAL */
#define RULE_V "\xe2\x94\x82" /* U+2502 BOX DRAWINGS LIGHT VERTICAL */

#define FOOTER_FULL  "enter select  esc close  up/down move"
#define FOOTER_SHORT "enter/esc  \xe2\x86\x91/\xe2\x86\x93" /* enter/esc  up/down */

/* The narrowest panel worth drawing, which is the short footer plus the space
 * either side of it. Tying the two together means a panel is never shown
 * without the keys that dismiss it, which is the one row a cramped terminal
 * would otherwise have to drop. Anything narrower draws nothing. */
#define PANEL_MIN_W 18

/* Rows the panel costs on top of the item list: the titled rule and the rule
 * carrying the footer. */
#define PANEL_CHROME 2

/* menu_height() is handed no menu, so it cannot answer for one. It reports the
 * geometry of the most recent menu_draw - the overlay the caller is currently
 * composing - and 0 whenever that draw produced nothing: closed, too small, or
 * never called. */
static int g_rows = 0;

int menu_height(void) { return g_rows; }

/* ------------------------------------------------------------------ pieces */

static void hrule(Screen *s, int x, int y, int n) {
    for (int i = 0; i < n; i++) cell_put(s, x + i, y, RULE_H, T_BORDER, BG_NONE, 0, 0);
}

/* Columns a string occupies on screen. Not strlen: the arrows in the short
 * footer are three bytes each and one column, so measuring in bytes puts the
 * dash on the wrong side of the trailing space and drops a character. The
 * widths and the step sizes deliberately mirror term.c's own accounting, so a
 * label laid out here lands where cell_text actually put it. */
static int cols(const char *s) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int step;
        if (*p < 0x80)                  step = 1;
        else if ((*p & 0xE0) == 0xC0)   step = 2;
        else if ((*p & 0xF0) == 0xE0)   step = 3;
        else                            step = 4;
        n += (step == 4) ? 2 : 1; /* four-byte forms take two columns */
        p += step;
    }
    return n;
}

/* A horizontal rule with `label` punched into it. The rule goes down first and
 * the label is written over it, so the row is exactly `n` columns wide whatever
 * the label, and a label too long for the panel simply leaves the rule plain
 * rather than running off the edge. */
static void titled_rule(Screen *s, int x, int y, int n, const char *label,
                        uint32_t col, int bold) {
    cell_fill(s, x, y, n, 1, BG_NONE);
    hrule(s, x, y, n);

    int lw = cols(label);
    if (lw + 4 > n) return; /* no room for a space either side */
    cell_put(s, x + 1, y, " ", FG_NONE, BG_NONE, 0, 0);
    cell_put(s, x + 2 + lw, y, " ", FG_NONE, BG_NONE, 0, 0);
    cell_text(s, x + 2, y, lw, label, col, BG_NONE, bold);
}

/* ----------------------------------------------------------------- overlay */

void menu_draw(Screen *s, const ActionMenu *m) {
    g_rows = 0;
    if (!s || !s->cells || s->w <= 0 || s->h <= 0) return;
    if (!m || !m->open) return;

    int w = s->w, h = s->h;

    /* Width is driven by the footer, the one row whose text is fixed, and then
     * clamped to the screen. Below PANEL_MIN_W the panel would be a column of
     * stubs, so nothing is drawn. */
    int want = cols(FOOTER_FULL) + 4;
    int pw = (want < PANEL_MIN_W) ? PANEL_MIN_W : want;
    if (pw > w) pw = w;
    if (pw < PANEL_MIN_W) return;

    /* count is the model's own number, re-checked because the view is the last
     * thing standing between a stale count and an out-of-bounds read. */
    int count = m->count;
    if (count < 0) count = 0;
    if (count > ITEM_COUNT) count = ITEM_COUNT;

    int room = h - PANEL_CHROME;
    if (room < 1) return; /* no room even for a single row */
    int shown = (count < room) ? count : room;
    int ph = shown + PANEL_CHROME;

    /* Same reasoning for sel: actions.c clamps it, and a view that takes that on
     * trust would turn any future slip there into a read past items[]. */
    int sel = -1;
    if (count > 0 && m->sel >= 0 && m->sel < count) sel = m->sel;

    /* More rows than fit: window the list around the selection and clamp at
     * both ends, so the highlighted row is always on screen and the top and
     * bottom rows stay reachable. */
    int first = 0;
    if (shown < count && sel >= 0) {
        first = sel - shown / 2;
        if (first < 0) first = 0;
        if (first > count - shown) first = count - shown;
    }

    int x0 = (w - pw) / 2;
    int y0 = (h - ph) / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;

    int ylast = y0 + ph - 1; /* the footer owns the bottom rule */

    titled_rule(s, x0, y0, pw, "Actions", T_TEXT_HI, 1);

    for (int i = first; i < first + shown; i++) {
        int y = y0 + 1 + (i - first);
        if (y >= ylast) break; /* backstop: never draw over the footer rule */

        const char *label = actions_label((MenuItem)m->items[i]);
        if (!label) label = "";

        if (i == sel) {
            /* The one deliberate background in the app, matching draw_nav's
             * active tab: a T_LAT pill carrying T_SURFACE text. */
            cell_fill(s, x0, y, pw, 1, T_LAT);
            cell_put(s, x0, y, RULE_V, T_SURFACE, T_LAT, 0, 0);
            cell_put(s, x0 + pw - 1, y, RULE_V, T_SURFACE, T_LAT, 0, 0);
            cell_text(s, x0 + 2, y, pw - 3, label, T_SURFACE, T_LAT, 1);
        } else {
            /* BG_NONE erase: the glyphs underneath go away, the background
             * underneath them does not. */
            cell_fill(s, x0, y, pw, 1, BG_NONE);
            cell_put(s, x0, y, RULE_V, T_BORDER, BG_NONE, 0, 0);
            cell_put(s, x0 + pw - 1, y, RULE_V, T_BORDER, BG_NONE, 0, 0);
            cell_text(s, x0 + 2, y, pw - 3, label, T_TEXT, BG_NONE, 0);
        }
    }

    titled_rule(s, x0, ylast, pw,
                (want <= pw) ? FOOTER_FULL : FOOTER_SHORT, T_TEXT_DIM, 0);

    g_rows = ph;
}

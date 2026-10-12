/* Keymap implementation. See keymap.h for why this layer exists. */
#include "keymap.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- bindings --
 *
 * One row per binding. `name` and `label` are both here so the HELP tab and
 * keymap_keyname can be generated from this table; keeping a second hand-
 * written list of "what the keys do" is how a help screen starts lying.
 *
 * Modifier matching is exact (see binding_matches), so "q" and "Ctrl+Q" are
 * two distinct keys and only one of them quits. Every letter row carries
 * mods == 0 for the same reason: a bare letter is a binding, and the same
 * letter under a modifier is not that binding any more.
 */

typedef struct {
    TermKey      key;
    Action       action;
    const char  *name;  /* how the key is printed */
    const char  *label; /* what it does */
} Binding;

#define CH(c, m)  { EV_KEY, KEY_CHAR, (c), (m), 0 }
#define SP(k, m)  { EV_KEY, (k), 0, (m), 0 }

static const Binding BINDINGS[] = {
    /* Navigation. Tab/Shift+Tab are listed separately because they are
     * separate keys; matching on key alone would make Shift+Tab do the same
     * thing as Tab. */
    { SP(KEY_TAB, 0),             ACT_TAB_NEXT, "Tab",       "Next tab" },
    { SP(KEY_TAB, KM_MOD_SHIFT),  ACT_TAB_PREV, "Shift+Tab", "Previous tab" },
    { CH('1', 0),                 ACT_TAB_1,    "1",         "Live tab" },
    { CH('2', 0),                 ACT_TAB_2,    "2",         "Charts tab" },
    { CH('3', 0),                 ACT_TAB_3,    "3",         "Details tab" },
    { CH('4', 0),                 ACT_TAB_4,    "4",         "History tab" },
    { CH('5', 0),                 ACT_TAB_5,    "5",         "Help tab" },

    /* Transport and view, as advertised on the tab strip in ui.c:
     * "space pause  +/- zoom  m actions  f test  p ping  d down  u up  r rerun  q quit" */
    { CH(' ', 0),                 ACT_PAUSE,      "Space",     "Pause or resume the run" },
    { CH('+', 0),                 ACT_ZOOM_IN,    "+",         "Zoom in" },
    { CH('-', 0),                 ACT_ZOOM_OUT,   "-",         "Zoom out" },
    { CH('=', 0),                 ACT_ZOOM_RESET, "=",         "Reset zoom" },
    { CH('m', 0),                 ACT_MENU,       "m",         "Actions menu" },
    /* Esc is the conventional menu key, and the menu is the only place with
     * anywhere to put "clear history", "export" and "reveal". */
    { SP(KEY_ESC, 0),             ACT_MENU,       "Esc",       "Actions menu" },
    { CH('f', 0),                 ACT_MODE_FULL,  "f",         "Full test mode" },
    { CH('p', 0),                 ACT_MODE_PING,  "p",         "Ping mode" },
    { CH('d', 0),                 ACT_MODE_DOWN,  "d",         "Download mode" },
    { CH('u', 0),                 ACT_MODE_UP,    "u",         "Upload mode" },
    { CH('r', 0),                 ACT_RERUN,      "r",         "Re-run the test" },

    /* Data and quit. */
    { CH('x', 0),                 ACT_CLEAR_HISTORY, "x",      "Clear history" },
    { CH('e', 0),                 ACT_EXPORT,       "e",        "Export the last run" },
    { CH('v', 0),                 ACT_REVEAL,       "v",        "Reveal connection detail" },
    { CH('?', 0),                 ACT_HELP,         "?",       "Show this help" },
    { CH('H', 0),                 ACT_HELP,         "H",       "Show this help" },
    { CH('q', 0),                 ACT_QUIT,       "q",         "Quit" },
    /* Ctrl+C quits because a TUI that ignores it cannot be stopped; Ctrl+Q
     * deliberately does not, since Ctrl+Q is indistinguishable from XON on a
     * terminal that has flow control enabled. */
    { CH('c', KM_MOD_CTRL),       ACT_QUIT,         "Ctrl+C",  "Quit" },

    /* Mouse. The row-0 gate cannot live in this row - it depends on where the
     * pointer was - so keymap_action applies it. Keeping the row here is what
     * puts the binding in HELP. */
    { { EV_MOUSE, KEY_NONE, 0, 0, 0 }, ACT_CLICK, "Click tab strip", "Switch tab" },
};

#define NBINDINGS (int)(sizeof BINDINGS / sizeof BINDINGS[0])

/* Identity test for a binding row against a decoded key. Deliberately not
 * strcmp-able and deliberately exact on modifiers: the whole reason a user
 * cannot quit with a stray Ctrl+Q is that this function refuses to blur the
 * two. */
static int binding_matches(const Binding *b, const TermKey *k) {
    if (b->key.type != k->type) return 0;
    if (b->key.key != k->key) return 0;
    if (b->key.key == KEY_CHAR && b->key.ch != k->ch) return 0;
    if ((b->key.mods & 0x7) != (k->mods & 0x7)) return 0;
    if (b->key.type == EV_MOUSE && b->key.btn != k->btn) return 0;
    return 1;
}

static const Binding *find_binding(TermKey k) {
    for (int i = 0; i < NBINDINGS; i++)
        if (binding_matches(&BINDINGS[i], &k)) return &BINDINGS[i];
    return NULL;
}

/* ---------------------------------------------------------------- actions -- */

int keymap_wants_point(Action a, int *mouse_col, int *mouse_row) {
    /* Coordinates come from the caller's own variables; there is nothing to
     * report here, but the query keeps the UI from hard-coding an action
     * check that would then have to be kept in sync with this file. */
    if (mouse_col) *mouse_col = -1;
    if (mouse_row) *mouse_row = -1;
    return a == ACT_CLICK;
}

Action keymap_action(TermKey k, int mouse_col, int mouse_row) {
    /* A mouse event is only ever ACT_CLICK, and only when all three hold:
     * left button (btn 0 - which is what a wheel or the right button is not),
     * a real coordinate, and the tab strip row. term.c has already filtered
     * to press events, but repeating the check here is what stops a malformed
     * or hand-built TermKey from turning a scroll into a tab switch. */
    if (k.type == EV_MOUSE) {
        if (k.btn != 0) return ACT_NONE;
        if (mouse_col < 0 || mouse_row != KM_TAB_ROW) return ACT_NONE;
        return ACT_CLICK;
    }
    if (k.type != EV_KEY) return ACT_NONE;

    const Binding *b = find_binding(k);
    return b ? b->action : ACT_NONE;
}

/* ------------------------------------------------------------- key names -- */

/* Fallback for keys with no binding. Not a second binding list - it only ever
 * answers "what was that key", never "what does it do". */
/* Turn a raw control byte into the lowercase letter it stands for, so a
 * terminal that has no way to report a shift key still lands on the same
 * binding as one that does. */
static char ctrl_letter(int c) {
    if (c < 1 || c > 26) return '?';
    return (char)('a' + (c - 1));
}

static void keyname_builtin(TermKey k, char *out, size_t cap) {
    static const struct { int key; const char *name; } SPECIALS[] = {
        { KEY_UP,        "Up" },
        { KEY_DOWN,      "Down" },
        { KEY_LEFT,      "Left" },
        { KEY_RIGHT,     "Right" },
        { KEY_ENTER,     "Enter" },
        { KEY_ESC,       "Esc" },
        { KEY_BACKSPACE, "Backspace" },
        { KEY_TAB,       "Tab" },
        { KEY_HOME,      "Home" },
        { KEY_END,       "End" },
        { KEY_PGUP,      "PgUp" },
        { KEY_PGDN,      "PgDn" },
        { KEY_DELETE,    "Del" },
    };

    if (k.type == EV_MOUSE) {
        snprintf(out, cap, "Mouse%d", k.btn);
        return;
    }
    if (k.key == KEY_CHAR) {
        /* Built from the character, not from a row lookup, so an unbound
         * letter still prints as itself rather than as nothing. */
        if (k.mods & KM_MOD_CTRL) {
            int c = (unsigned char)k.ch;
            /* A control byte can arrive verbatim; show the letter it means.
             * Modifier names are uppercase because that is how they are
             * written, and because "Ctrl+c" reads as a typo. */
            char l = c < 0x20 ? ctrl_letter(c)
                  : (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A'))
                  : (char)c;
            snprintf(out, cap, "Ctrl+%c", l);
            return;
        }
        if (k.mods & KM_MOD_ALT) {
            snprintf(out, cap, "Alt+%c", k.ch);
            return;
        }
        if (k.mods & KM_MOD_SHIFT) {
            snprintf(out, cap, "Shift+%c", k.ch);
            return;
        }
        snprintf(out, cap, "%c", k.ch);
        return;
    }
    for (size_t i = 0; i < sizeof SPECIALS / sizeof SPECIALS[0]; i++) {
        if (SPECIALS[i].key == k.key) {
            snprintf(out, cap, "%s", SPECIALS[i].name);
            return;
        }
    }
    snprintf(out, cap, "Key%d", k.key);
}

void keymap_keyname(TermKey k, char *out, size_t cap) {
    if (cap == 0) return; /* nothing to write, not even a terminator */
    out[0] = '\0';
    const Binding *b = find_binding(k);
    if (b) snprintf(out, cap, "%s", b->name);
    else    keyname_builtin(k, out, cap);
}

/* ------------------------------------------------------------------- help -- */

int keymap_help(char *out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = '\0';

    /* Column width is measured off the table rather than hard-coded, so a
     * binding with a longer name widens the column instead of colliding with
     * the description. */
    size_t width = 0;
    for (int i = 0; i < NBINDINGS; i++) {
        size_t n = strlen(BINDINGS[i].name);
        if (n > width) width = n;
    }

    size_t pos = 0;
    int lines = 0;

    for (int i = 0; i < NBINDINGS; i++) {
        /* Build the whole line before committing any of it: a partially
         * written line would be counted as a line and would have no newline,
         * which is worse than stopping cleanly. */
        char line[192];
        int n = snprintf(line, sizeof line, "  %s", BINDINGS[i].name);
        if (n < 0) break;
        size_t len = (size_t)n;
        for (size_t p = len; p < width + 3; p++) {
            if (p + 1 >= sizeof line) break;
            line[p] = ' ';
            len = p + 1;
        }
        n = snprintf(line + len, sizeof line - len, "%s\n", BINDINGS[i].label);
        if (n < 0) break;
        len += (size_t)n;

        /* Room for the line *and* everything already written, plus the terminator,
         * or stop. Checking the line alone would let a full buffer overflow. */
        if (pos + len + 1 > cap) break;
        memcpy(out + pos, line, len);
        pos += len;
        out[pos] = '\0';
        lines++;
    }
    return lines;
}

/* ---------------------------------------------------------------- decoder -- */

TermKey keymap_from_event(Event ev) {
    TermKey k;
    memset(&k, 0, sizeof k);
    k.type = ev.type;

    if (ev.type == EV_MOUSE) {
        k.btn = ev.mbtn;
        k.mods = ev.mods & 0x7;
        return k;
    }
    if (ev.type != EV_KEY) return k;

    k.key = ev.key;
    k.mods = ev.mods & 0x7;
    if (ev.key == KEY_CHAR) k.ch = ev.ch;

    /* A control byte should never reach here as a bare character: it would
     * compare equal to whatever letter its ASCII value happens to be. Fold it
     * back into the modifier it stands for so Ctrl+C can be a binding. */
    if (k.key == KEY_CHAR && (unsigned char)k.ch < 0x20) {
        int c = (unsigned char)k.ch;
        k.ch = ctrl_letter(c);
        k.mods |= KM_MOD_CTRL;
    }
    return k;
}

/* Coordinates come back through out-params rather than inside TermKey: the
 * fixed keymap_action signature takes them as arguments, so packing them into
 * the key as well would create a second copy that could disagree. */
TermKey keymap_decode_at(const char *bytes, int *col, int *row) {
    TermKey k;
    memset(&k, 0, sizeof k);
    if (col) *col = -1;
    if (row) *row = -1;
    if (!bytes) return k;

    const unsigned char *s = (const unsigned char *)bytes;
    size_t n = strlen(bytes);
    size_t i = 0;

    /* A literal control byte is the only way Ctrl+<letter> survives a byte
     * stream, so turn it into a key + modifier before anything else looks at
     * it. ESC is excluded: it is below 0x20 too, but it introduces a
     * sequence rather than standing for a key of its own. */
    if (n > 0 && s[0] < 0x20 && s[0] != 0x1b &&
        s[0] != 0x09 && s[0] != 0x0a && s[0] != 0x0d) {
        k.type = EV_KEY;
        k.key = KEY_CHAR;
        k.mods = KM_MOD_CTRL;
        k.ch = ctrl_letter((int)s[0]);
        return k;
    }

    if (n > 0 && s[0] != 0x1b) {
        if (s[0] == 0x0d || s[0] == 0x0a) { k.type = EV_KEY; k.key = KEY_ENTER; return k; }
        if (s[0] == 0x09) { k.type = EV_KEY; k.key = KEY_TAB; return k; }
        if (s[0] == 0x08 || s[0] == 0x7f) { k.type = EV_KEY; k.key = KEY_BACKSPACE; return k; }
        k.type = EV_KEY;
        k.key = KEY_CHAR;
        k.ch = (char)s[0];
        return k;
    }

    /* ESC sequence: ESC [ ... */
    i = 1;
    if (i >= n || (s[i] != '[' && s[i] != 'O')) { k.type = EV_KEY; k.key = KEY_ESC; return k; }
    i++;
    if (i >= n) return k; /* truncated: zeroed key, maps to ACT_NONE */

    /* SGR mouse: ESC [ < button ; col ; row M */
    if (s[i] == '<') {
        i++;
        unsigned b = 0, col_raw = 0, row_raw = 0;
        int saw = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { b = b * 10 + (unsigned)(s[i] - '0'); i++; saw = 1; }
        if (i >= n || s[i] != ';' || !saw) return k;
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') { col_raw = col_raw * 10 + (unsigned)(s[i] - '0'); i++; }
        if (i >= n || s[i] != ';') return k;
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') { row_raw = row_raw * 10 + (unsigned)(s[i] - '0'); i++; }
        if (i >= n) return k;
        /* 'm' is a release; a release must not select a tab. */
        if (s[i] != 'M') return k;
        /* The motion bit (0x20) means drag, not press. */
        if (b & 0x20) return k;

        k.type = EV_MOUSE;
        k.btn = (int)(b & 0x7fu);
        if (b & 0x04) k.mods |= KM_MOD_SHIFT;
        if (b & 0x08) k.mods |= KM_MOD_ALT;
        if (b & 0x10) k.mods |= KM_MOD_CTRL;
        /* SGR coordinates are 1-based; the UI works in zero-based cells. */
        if (col && k.btn == 0) *col = (int)col_raw - 1;
        if (row && k.btn == 0) *row = (int)row_raw - 1;
        return k;
    }

    switch (s[i]) {
        case 'A': k.type = EV_KEY; k.key = KEY_UP;    return k;
        case 'B': k.type = EV_KEY; k.key = KEY_DOWN;  return k;
        case 'C': k.type = EV_KEY; k.key = KEY_RIGHT; return k;
        case 'D': k.type = EV_KEY; k.key = KEY_LEFT;  return k;
        case 'H': k.type = EV_KEY; k.key = KEY_HOME;  return k;
        case 'F': k.type = EV_KEY; k.key = KEY_END;   return k;
        /* Shift+Tab. term.c's table does not carry this today, so without the
         * row below BackTab would decode to nothing and previous-tab would be
         * unreachable on the POSIX path. */
        case 'Z': k.type = EV_KEY; k.key = KEY_TAB; k.mods = KM_MOD_SHIFT; return k;
        case '~': {
            i++;
            if (i >= n) return k;
            switch (s[i]) {
                case '1': case '7': k.type = EV_KEY; k.key = KEY_HOME;   return k;
                case '3':            k.type = EV_KEY; k.key = KEY_DELETE; return k;
                case '5':            k.type = EV_KEY; k.key = KEY_PGUP;   return k;
                case '6':            k.type = EV_KEY; k.key = KEY_PGDN;   return k;
                case '8':            k.type = EV_KEY; k.key = KEY_END;    return k;
                default: return k;
            }
        }
        default: return k;
    }
}
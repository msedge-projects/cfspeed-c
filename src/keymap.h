/* Keymap: the single place where a decoded key becomes an intent.
 *
 * Everything above this layer deals in Actions, never in key codes, so the
 * binding set can be changed in one table without touching the UI, and the
 * HELP tab cannot drift out of sync with what the keys actually do - a help
 * screen that lies about the bindings is worse than no help screen at all.
 *
 * TermKey is the normalised form of one decoded key. It is deliberately not
 * an `Event`: an Event also carries the cursor position and the event type
 * discriminator, neither of which is an identity, while `mbtn` is - it is the
 * difference between a click and a scroll, and collapsing those two is exactly
 * the bug that makes a wheel spin flip your tab. See keymap_action.
 */
#ifndef CFS_KEYMAP_H
#define CFS_KEYMAP_H
#include "term.h"

typedef enum {
    ACT_NONE = 0,
    ACT_TAB_NEXT, ACT_TAB_PREV, ACT_TAB_1, ACT_TAB_2, ACT_TAB_3, ACT_TAB_4, ACT_TAB_5,
    ACT_PAUSE, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_ZOOM_RESET,
    ACT_MODE_FULL, ACT_MODE_PING, ACT_MODE_DOWN, ACT_MODE_UP,
    ACT_RERUN, ACT_MENU, ACT_HELP, ACT_QUIT, ACT_CLEAR_HISTORY, ACT_EXPORT,
    ACT_REVEAL,
    /* A left-button click at a terminal column/row; the UI resolves which
       tab strip cell that lands on and issues the matching ACT_TAB_*. */
    ACT_CLICK
} Action;

/* Modifier bits. Same values as term.c's CFS_MOD_* and as the bit layout
 * term.h documents for Event.mods, so a TermKey can be built from an Event
 * without a translation step. */
#define KM_MOD_SHIFT 1
#define KM_MOD_CTRL  2
#define KM_MOD_ALT   4

/* One decoded key, reduced to the fields that determine what it means. */
typedef struct {
    int  type; /* EV_KEY or EV_MOUSE */
    int  key;  /* KEY_* */
    char ch;   /* KEY_CHAR payload */
    int  mods; /* KM_MOD_* */
    int  btn;  /* EV_MOUSE button code; 0 is left */
} TermKey;

/* Map a decoded key to an action. `mouse_col`/`mouse_row` are only consulted
   for mouse events. Returns ACT_NONE for anything unhandled — never a
   spurious action. */
Action keymap_action(TermKey k, int mouse_col, int mouse_row);

/* Human-readable binding list for the HELP tab: one "key   description"
   line per binding, newline separated, into `out`. Returns the number of
   lines written. Must stay in sync with keymap_action automatically — derive
   it from the same table, do not hand-maintain a second list. */
int    keymap_help(char *out, size_t cap);

/* Name of a key for display, e.g. "Ctrl+C", "Space", "Shift+Tab". Writes
   into `out`, always NUL-terminated, never overflows `cap`. */
void   keymap_keyname(TermKey k, char *out, size_t cap);

/* --- additions -------------------------------------------------------------
 * The three functions above are the whole interface the UI needs. The four
 * declarations below exist because `TermKey` is defined here rather than in
 * term.h, so something has to put a real decoder's output into it, and a
 * caller should not have to hand-assemble a struct literal to do so.
 */

/* Normalise one `Event` from term_poll. */
TermKey keymap_from_event(Event ev);

/* Parse one terminal byte sequence ("\t", "\x1b[Z", "\x1b[<0;6;1M") into a
 * TermKey. Mirrors the grammar term.c accepts, including folding raw control
 * bytes into their Ctrl+<letter> meaning. Mouse coordinates (zero-based) come
 * back through `col`/`row`, or -1 when the key has no position. Returns a
 * zeroed TermKey for a truncated or malformed sequence, which maps to
 * ACT_NONE. */
TermKey keymap_decode_at(const char *bytes, int *col, int *row);

/* Actions that need a position - currently just ACT_CLICK - have nowhere to
 * put it in the return value. The UI reads these right after
 * keymap_action() returns; `mouse_col`/`mouse_row` are the coordinates that
 * were passed in. */
int  keymap_wants_point(Action a, int *mouse_col, int *mouse_row);

/* Row of the tab strip. A click is only meaningful here; everything below is
 * content, and a click there belongs to the view, not to us. */
#define KM_TAB_ROW 0

#endif /* CFS_KEYMAP_H */
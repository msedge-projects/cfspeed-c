/* The actions menu: model only. See actions.h for why this is separated from
 * the view that draws it. */
#include "actions.h"

/* `sel` is only ever valid if the array can hold every entry, so the header's
 * two promises have to be arithmetically compatible. */
_Static_assert(ITEM_COUNT <= ACTIONS_MAX_ITEMS,
               "ActionMenu.items[] cannot hold every MenuItem");

/* Refill items[] with what is enabled right now, in MenuItem order so the menu
 * never reshuffles itself as the engine changes state. Slots past count are
 * poisoned with ITEM_COUNT rather than left stale: it costs nothing and a view
 * that trusts the array instead of the count gets an obviously invalid row
 * rather than an item the user already ruled out. */
static void rebuild(ActionMenu *m, int running, int paused) {
    int n = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (actions_enabled((MenuItem)i, running, paused)) m->items[n++] = i;
    }
    m->count = n;
    for (int i = n; i < ITEM_COUNT; i++) m->items[i] = ITEM_COUNT;
}

/* An out-of-range sel reaching the renderer is an out-of-bounds read, so this
 * runs after every op. Closed means nothing is highlighted, and an empty menu
 * has no valid index at all; both collapse to 0, which the view must check
 * against count before dereferencing. */
static void clamp_sel(ActionMenu *m) {
    if (!m->open || m->count <= 0) {
        m->sel = 0;
        return;
    }
    if (m->sel < 0) m->sel = 0;
    if (m->sel > m->count - 1) m->sel = m->count - 1;
}

const char *actions_label(MenuItem it) {
    switch (it) {
        case ITEM_RESUME:        return "Resume test";
        case ITEM_PAUSE:         return "Pause test";
        case ITEM_RERUN:         return "Rerun test";
        case ITEM_ZOOM_IN:       return "Zoom in";
        case ITEM_ZOOM_OUT:      return "Zoom out";
        case ITEM_ZOOM_RESET:    return "Reset zoom";
        case ITEM_EXPORT_JSON:   return "Export JSON";
        case ITEM_CLEAR_HISTORY: return "Clear history";
        case ITEM_QUIT:          return "Quit";
        case ITEM_COUNT:         break;
    }
    return ""; /* not a real MenuItem; the view must never be handed NULL */
}

int actions_enabled(MenuItem it, int running, int paused) {
    /* Resume is meaningful only while a run is parked and Pause only while one
     * is in flight. Neither is greyed-out-but-present: a row that cannot move
     * is worse than a row that is not offered at all, so they leave the list
     * instead. Everything else - rerun, zoom, export, history, quit - stays
     * available in every state, which is what guarantees the menu is never
     * empty and sel therefore always has somewhere valid to point. */
    if (it == ITEM_RESUME) return running && paused;
    if (it == ITEM_PAUSE)  return running && !paused;
    if (it >= ITEM_RESUME && it < ITEM_COUNT) return 1;
    return 0; /* ITEM_COUNT, or a value that is not a MenuItem at all */
}

MenuItem actions_apply(ActionMenu *m, MenuOp op, int running, int paused) {
    if (!m) return ITEM_COUNT;

    /* A zeroed or garbage `open` must not be able to smuggle a bogus truthy
     * value through, so the flag is normalised to exactly 0 or 1 before
     * anything branches on it. sel/count are rebuilt from scratch below. */
    m->open = m->open ? 1 : 0;

    /* The enabled set is rebuilt first and unconditionally: the engine can
     * finish or pause a run while the menu sits open, so a set captured when
     * the menu opened is stale by the time the next key arrives. */
    rebuild(m, running, paused);

    /* clamp before the switch so a corrupted sel cannot index items[] inside
     * ACTMENU_TOGGLE, and again after it because every branch moves sel. */
    clamp_sel(m);

    MenuItem chosen = ITEM_COUNT;
    switch (op) {
        case ACTMENU_NONE:
            /* No key. The rebuild above is the whole job: it is how an open
             * menu keeps tracking engine state that moved underneath it. */
            break;

        case ACTMENU_OPEN:
            m->open = 1;
            /* Top of the list, not wherever the highlight was left. Opening
             * the menu again after a cancel should not resume a selection the
             * user never made. */
            m->sel = 0;
            break;

        case ACTMENU_NAVIGATE:
            /* items[] is gapless - it holds enabled entries only - so one step
             * lands on the next real item and cannot skip onto a disabled one.
             * Wrap rather than stop at the end: a cursor stuck on the last row
             * reads as a dead key. */
            if (m->open && m->count > 0) {
                m->sel = (m->sel + 1) % m->count;
            }
            break;

        case ACTMENU_NAVIGATE_BACK:
            if (m->open && m->count > 0) {
                /* count is never 0 inside this branch, so the unsigned
                 * subtraction below cannot wrap. */
                m->sel = (m->sel + m->count - 1) % m->count;
            }
            break;

        case ACTMENU_TOGGLE:
            if (m->open && m->count > 0) {
                chosen = (MenuItem)m->items[m->sel];
            }
            /* Selecting closes the menu either way. A keypress that chose
             * nothing must not leave it open, or the user is left looking at an
             * overlay with no obvious way back. */
            m->open = 0;
            m->sel  = 0;
            break;

        case ACTMENU_CANCEL:
            m->open = 0;
            m->sel  = 0;
            break;
    }

    clamp_sel(m);
    return chosen;
}

void actions_init(ActionMenu *m) {
    if (!m) return;
    m->open  = 0;
    m->sel   = 0;
    m->count = 0;
    for (int i = 0; i < ITEM_COUNT; i++) m->items[i] = ITEM_COUNT;
}
/* The actions menu: model only.
 *
 * This file owns which items exist, which of them make sense right now, and
 * how the highlight moves. It draws nothing and knows nothing about the
 * terminal - a separate view turns an ActionMenu into pixels. Keeping the
 * decision separate from the drawing is what makes the menu testable: the
 * invariant that actually matters (sel always lands on an enabled item) can be
 * hammered for ten thousand keypresses without a screen attached.
 */
#ifndef CFS_ACTIONS_H
#define CFS_ACTIONS_H
#include <stddef.h>

#define ACTIONS_MAX_ITEMS 16

typedef enum {
    ACTMENU_NONE = 0,     /* closed */
    ACTMENU_OPEN,
    ACTMENU_NAVIGATE,     /* down / j; changes the highlight only */
    ACTMENU_NAVIGATE_BACK,/* up / k */
    ACTMENU_TOGGLE,       /* enter or space: choose the highlighted item */
    ACTMENU_CANCEL        /* esc or m again: close without acting */
} MenuOp;

typedef enum {
    ITEM_RESUME = 0,
    ITEM_PAUSE,
    ITEM_RERUN,
    ITEM_ZOOM_IN,
    ITEM_ZOOM_OUT,
    ITEM_ZOOM_RESET,
    ITEM_EXPORT_JSON,
    ITEM_CLEAR_HISTORY,
    ITEM_QUIT,
    ITEM_COUNT
} MenuItem;

typedef struct {
    int      open;
    int      sel;         /* index into the enabled set, not MenuItem */
    int      items[ITEM_COUNT]; /* MenuItem values, enabled ones only */
    int      count;
} ActionMenu;

/* Describe an item for rendering, e.g. "Rerun test". Never NULL. */
const char *actions_label(MenuItem it);

/* True when the item makes sense right now, e.g. Resume is pointless while
   idle, Pause while not running. `running` is the engine state. */
int  actions_enabled(MenuItem it, int running, int paused);

/* Apply an op to the menu. Recomputes the enabled set and clamps `sel` so it
   always points at a valid entry - closing the menu must never leave a
   dangling selection. Returns the MenuItem chosen on ACTMENU_TOGGLE, or
   ITEM_COUNT when nothing was chosen.
 *
 * Safe on a zero-initialised or never-initialised menu, and when nothing is
   enabled: callers never have to check before calling.
 *
 * ACTMENU_NAVIGATE moves the highlight one step forward (down / j) with
 * wraparound; ACTMENU_NAVIGATE_BACK moves it one step back (up / k). Both skip
 * disabled rows. The key layer is the only place that knows which key was
 * pressed, which is why the direction is not baked in
 * here - see the NAVIGATE case in actions.c. */
MenuItem actions_apply(ActionMenu *m, MenuOp op, int running, int paused);

/* Closed, nothing highlighted, enabled set built for the idle state so a
   caller that draws before the first actions_apply has something sane. */
void actions_init(ActionMenu *m);
#endif
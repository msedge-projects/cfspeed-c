#ifndef CFS_UI_H
#define CFS_UI_H

#include "engine.h"
#include "term.h"
#include "history.h"

/* `zoom` shrinks the visible time window, showing the most recent samples at a
 * finer scale. 1 is the whole run. */
void ui_draw(Screen *s, const Engine *e, const History *hist, int tab, int zoom);

/* Row highlighted in the HISTORY tab. Owned by main.c's event loop, drawn
 * here, so the selection survives a redraw without the UI keeping state. */
void ui_set_history_sel(int row);
int  ui_history_sel(void);

/* Which tab a click at (col, row) landed on, or -1. Shares draw_nav's geometry
 * rather than repeating it, because a hit test that drifts from what is drawn
 * selects the wrong tab with no visible symptom. */
int  ui_hit_tab(int col, int row, int w, int h);

#endif /* CFS_UI_H */
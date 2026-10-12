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

#endif /* CFS_UI_H */
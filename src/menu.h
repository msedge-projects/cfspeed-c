#ifndef CFS_MENU_H
#define CFS_MENU_H
#include "actions.h"
#include "term.h"

/* Draw the actions menu as a centred overlay on top of whatever is already in
   `s`. A no-op when the menu is closed, so the caller can draw unconditionally
   after the main view. Must never index outside the screen. */
void menu_draw(Screen *s, const ActionMenu *m);

/* Rows the overlay occupies, so the caller can decide whether there is room
   for it. Returns 0 when the menu is closed. */
int  menu_height(void);
#endif

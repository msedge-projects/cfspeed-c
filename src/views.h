/* The four non-live tabs: charts, details, run history and the key reference.
 *
 * ui.c owns the header strip and the tab bar. These views draw into the band
 * between them, so none of them needs to know how the frame is composed and
 * none of them fights the chrome for the same row.
 */
#ifndef CFS_VIEWS_H
#define CFS_VIEWS_H

#include "engine.h"
#include "term.h"

/* Unconditional, and deliberately so. History is a fixed MAX_RUNS array by
 * value and nothing else in the tree has that shape, so a stand-in declaration
 * here would not be a second opinion on the layout - it would be a different
 * type that binds silently at compile time and is then never populated. The
 * two adapters at the top of views.c absorb a genuine layout change; a missing
 * header should stop the build, not reach the terminal. */
#include "history.h"

/* Each view draws its own title strip and leaves the last row to the tab bar,
 * so they can be dropped into the frame in any order without overlapping. */
#define VIEWS_BODY_TOP 2
#define VIEWS_BODY_BOT 1

/* Below this the spec-sheet and the key table stop being legible, and below
 * this the charts stop being worth drawing at all. */
#define VIEWS_MIN_W 60
#define VIEWS_MIN_H 20

void view_charts(Screen *s, const Engine *e, const History *h);
void view_details(Screen *s, const Engine *e, const History *h);
void view_history(Screen *s, const Engine *e, const History *h, int sel);
void view_help(Screen *s, const Engine *e, const History *h);

#endif /* CFS_VIEWS_H */
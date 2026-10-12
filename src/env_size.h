/* Screen size when there is no console to ask.
 *
 * A TUI launched from a pipe or a log has no window size. Reading COLUMNS and
 * LINES keeps it usable there, and the caller clamps to a sane default if
 * those are absent too.
 */
#ifndef CFS_ENV_SIZE_H
#define CFS_ENV_SIZE_H

#include <stdlib.h> /* getenv, atoi */

#define MIN_SCREEN_W 60
#define MIN_SCREEN_H 14

/* Parses COLUMNS/LINES with strtol and clamps to something drawable. A pipe or
 * a CI log can carry a garbage value, and an unvalidated atoi out of range
 * makes every later bounds check useless. */
static inline void env_size(int *w, int *h) {
    const char *c = getenv("COLUMNS");
    const char *l = getenv("LINES");
    long v;
    if (c && *c) {
        v = strtol(c, NULL, 10);
        if (v > 0 && v < 10000) *w = (int)v;
    }
    if (l && *l) {
        v = strtol(l, NULL, 10);
        if (v > 0 && v < 10000) *h = (int)v;
    }
}

#endif /* CFS_ENV_SIZE_H */
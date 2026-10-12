/* Screen size when there is no console to ask.
 *
 * A TUI launched from a pipe or a log has no window size. Reading COLUMNS and
 * LINES keeps it usable there, and the caller clamps to a sane default if
 * those are absent too.
 */
#ifndef CFS_ENV_SIZE_H
#define CFS_ENV_SIZE_H

#define MIN_SCREEN_W 60
#define MIN_SCREEN_H 14

static inline void env_size(int *w, int *h) {
    const char *c = getenv("COLUMNS");
    const char *l = getenv("LINES");
    if (c && *c) *w = atoi(c);
    if (l && *l) *h = atoi(l);
}

#endif /* CFS_ENV_SIZE_H */
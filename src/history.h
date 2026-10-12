/* Run history on disk.
 *
 * One JSON object per line inside a top-level array, oldest first, trimmed to
 * the newest MAX_RUNS on save. RunResult carries its raw sample arrays, so a
 * full History is several megabytes: keep it out of small stack frames.
 */
#ifndef CFS_HISTORY_H
#define CFS_HISTORY_H

#include "engine.h"

#define MAX_RUNS 200

typedef struct { RunResult runs[MAX_RUNS]; int count; } History;

void history_load(History *h);        /* a missing file leaves an empty history */
int  history_save(const History *h);  /* 0 on success */
int  history_clear(History *h);       /* drop the file and zero the struct */
const char *history_path(void);

#endif /* CFS_HISTORY_H */
/* Command line and headless output.
 *
 * The TUI is the default because that is what most runs want, but it is a poor
 * citizen in a pipeline: the alternative screen and the cursor addressing make
 * its output useless to anything reading a file or a CI log. These entry points
 * give the same run two other mouths - a plain summary on stderr and one JSON
 * object on stdout - without a second implementation of the measurement.
 */
#ifndef CFS_CLI_H
#define CFS_CLI_H

#include "engine.h"

typedef struct {
    Mode      mode;
    int       headless;     /* no TUI: print and exit */
    int       json;         /* --json: emit one JSON object on stdout and exit */
    int       quiet;        /* --quiet: suppress the human-readable summary */
    int       iterations;   /* runs to perform; default 1 */
    int       duration_s;   /* override phase duration; 0 = use defaults */
    int       no_history;   /* --no-history: do not persist */
    char      out[512];     /* --out <path>: write the JSON summary here too */
} Options;

void cli_parse(int argc, char **argv, Options *o);
/* Emit the human-readable summary for a finished run to stderr. */
void cli_summary(const Engine *e);
/* Emit the run as JSON on stdout. One compact object, no trailing newline. */
void cli_json(const Engine *e);
/* Validate that the options make sense together. Returns 0 on success, or a
   non-zero exit code after printing a message to stderr. */
int cli_validate(const Options *o);

#endif /* CFS_CLI_H */
/* Measurement engine.
 *
 * Runs as a state machine stepped from the UI's frame loop, so there is no
 * second thread that can race the renderer. Only the per-connection workers are
 * threads, and each does exactly one thing: accumulate a byte counter.
 */
#ifndef CFS_ENGINE_H
#define CFS_ENGINE_H

#include <stdint.h>

#include "net.h"

typedef enum { PH_IDLE, PH_DOWNLOAD, PH_UPLOAD, PH_FINALISING, PH_DONE, PH_ERROR } Phase;
typedef enum { MODE_FULL, MODE_LATENCY, MODE_DOWNLOAD, MODE_UPLOAD } Mode;

#define MAX_SAMPLES 1200
#define MAX_PROBES  400

typedef struct {
    uint64_t bytes;
    double   avg, p95, max;
    double   samples[MAX_SAMPLES];
    int      nsamples;
} Throughput;

typedef struct {
    Throughput down, up;
    LatStats   idle, loaded_down;
    double     bufferbloat_ms;
    double     stability_cv;
    char       stab_grade[4];
    char       bloat_grade[4];
    Meta       meta;
    uint64_t   timestamp_ms;
    int        valid;
} RunResult;

typedef struct Engine Engine;

Engine *engine_new(const char *base_url, Mode mode);
void    engine_free(Engine *e);

/* Advance the machine. Call once per frame. Returns 1 while a run is live. */
int  engine_poll(Engine *e);
/* True on the frame the run finishes. */
int  engine_finished(const Engine *e);

void engine_cancel(Engine *e);
void engine_restart(Engine *e, Mode mode);

Phase    engine_phase(const Engine *e);
const char *phase_label(Phase p);
uint64_t engine_elapsed_ms(const Engine *e);
double   engine_instant(const Engine *e);
const RunResult *engine_result(const Engine *e);
const Meta      *engine_meta(const Engine *e);
Mode     engine_mode(const Engine *e);
const char *engine_error(const Engine *e);

/* Latency samples drawn live, for the third chart. */
const double *engine_probe_samples(const Engine *e, int *count);
double engine_probe_instant(const Engine *e);

/* Live sample accessors for the two throughput charts. */
const double *engine_down_samples(const Engine *e, int *count);
const double *engine_up_samples(const Engine *e, int *count);

void stats_finalise(double *samples, int n, uint64_t bytes, uint64_t duration_ms,
                    Throughput *out);

#endif /* CFS_ENGINE_H */
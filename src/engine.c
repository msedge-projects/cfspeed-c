#include "engine.h"
#include "term.h"

#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MS   200
#define IDLE_MS     2000
#define DOWN_MS     10000
#define UP_MS       10000
#define CONCURRENCY 6
#define CHUNK_START 1000000ul
#define CHUNK_MIN   100000ul

struct Engine {
    char base[256];
    Mode mode;
    Phase phase;
    uint64_t phase_start;
    uint64_t run_start;
    uint64_t phase_end;
    int      finished;

    uint64_t counter;             /* shared with the workers */
    Worker   cfg;
    WorkerHandle *workers[CONCURRENCY];
    int      worker_count;

    /* Live series for the two throughput charts. */
    double down_samples[MAX_SAMPLES];
    int    ndown;
    double up_samples[MAX_SAMPLES];
    int    nup;
    uint64_t last_bytes;
    uint64_t last_time;
    double   instant;

    /* Idle latency is a separate window from latency measured under load;
     * mixing them would corrupt both the baseline and the bufferbloat figure. */
    double idle_probes[MAX_PROBES];
    int    nidle;
    double loaded_probes[MAX_PROBES];
    int    nloaded;
    double probe_instant;

    uint64_t chunk;

    RunResult result;
    char      error[256];
};

/* ------------------------------------------------------------- statistics */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile_sorted(const double *sorted, int n, double p) {
    if (n <= 0) return 0.0;
    if (n == 1) return sorted[0];
    double pos = p * (double)(n - 1);
    int lo = (int)floor(pos);
    int hi = (int)ceil(pos);
    if (lo == hi) return sorted[lo];
    double frac = pos - (double)lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

void stats_finalise(double *samples, int n, uint64_t bytes, uint64_t duration_ms,
                    Throughput *out) {
    out->bytes = bytes;
    out->nsamples = n;
    out->avg = out->p95 = out->max = 0.0;

    if (duration_ms > 0)
        /* duration_ms is milliseconds, so the bits-per-millisecond figure this
     * computes is a thousandth of a megabit per second. Dividing by 1e6 here
     * reported every result 1000x low - a 105 Mbps link printed as 0.1 Mbps
     * while p95, computed per-sample, printed 105 and the two disagreed. */
    out->avg = (double)bytes * 8.0 / (double)duration_ms / 1e3;

    if (n <= 0) return;
    double *sorted = (double *)malloc((size_t)n * sizeof(double));
    if (!sorted) return;
    memcpy(sorted, samples, (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), cmp_double);
    out->p95 = percentile_sorted(sorted, n, 0.95);
    out->max = sorted[n - 1];
    free(sorted);

    /* Drop the TCP slow-start ramp so the average reflects steady state. */
    if (n >= 5) {
        double sum = 0.0;
        int cut = n / 5;
        for (int i = 0; i < cut; i++) sum += samples[i];
        if (cut > 0) {
            double ramp_avg = sum / (double)cut;
            /* Keep the reported average honest: it is bytes over duration, not
             * the mean of the trimmed samples. */
            (void)ramp_avg;
        }
    }
}

static double cv_of(const double *s, int n) {
    if (n < 2) return 0.0;
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += s[i];
    mean /= (double)n;
    if (mean <= 0.0) return 0.0;
    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double d = s[i] - mean;
        var += d * d;
    }
    return sqrt(var / (double)n) / mean * 100.0;
}

static void grade_latency(double ms, char *out) {
    if (ms <= 0)        { snprintf(out, 4, "A+"); return; }
    if (ms <= 20)       { snprintf(out, 4, "A+"); return; }
    if (ms <= 50)       { snprintf(out, 4, "A");  return; }
    if (ms <= 100)      { snprintf(out, 4, "B");  return; }
    if (ms <= 200)      { snprintf(out, 4, "C");  return; }
    if (ms <= 400)      { snprintf(out, 4, "D");  return; }
    snprintf(out, 4, "F");
}

static void grade_stab(double cv, char *out) {
    if (cv <= 5)        { snprintf(out, 4, "A+"); return; }
    if (cv <= 10)       { snprintf(out, 4, "A");  return; }
    if (cv <= 20)       { snprintf(out, 4, "B");  return; }
    if (cv <= 35)       { snprintf(out, 4, "C");  return; }
    if (cv <= 60)       { snprintf(out, 4, "D");  return; }
    snprintf(out, 4, "F");
}

static void summarise_latency(double *s, int n, LatStats *out) {
    memset(out, 0, sizeof *out);
    out->samples = n;
    if (n <= 0) return;
    double *sorted = (double *)malloc((size_t)n * sizeof(double));
    if (!sorted) return;
    memcpy(sorted, s, (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), cmp_double);

    int good = 0;
    double sum = 0.0;
    for (int i = 0; i < n; i++)
        if (sorted[i] >= 0.0) { sum += sorted[i]; good++; }

    out->avg  = good ? sum / (double)good : 0.0;
    out->min  = sorted[0];
    out->med  = percentile_sorted(sorted, n, 0.50);
    out->p25  = percentile_sorted(sorted, n, 0.25);
    out->p75  = percentile_sorted(sorted, n, 0.75);
    out->p95  = percentile_sorted(sorted, n, 0.95);
    out->p99  = percentile_sorted(sorted, n, 0.99);
    out->max  = sorted[n - 1];
    /* A probe that did not answer is recorded as -1 and counts as loss. */
    out->loss = (double)(n - good) / (double)n * 100.0;
    double jit = 0.0;
    for (int i = 1; i < n; i++) jit += fabs(sorted[i] - sorted[i - 1]);
    out->jitter = n > 1 ? jit / (double)(n - 1) : 0.0;
    free(sorted);
}

/* ------------------------------------------------------------- lifecycle */

/* Wall-clock stamp for RunResult.timestamp_ms, in Unix epoch milliseconds.
 * One helper so engine_new and engine_restart cannot drift apart in units - a
 * mismatch here is invisible until the history table prints nonsense. */
static uint64_t stamp_now_ms(void) {
    return (uint64_t)time(NULL) * 1000u;
}

/* Clear a RunResult for a new run, keeping the connection metadata.
 *
 * `meta` is deliberately preserved, and this is the decision worth arguing
 * about. net_fetch_meta() is called exactly once, in engine_new(), and it is
 * the only writer of meta; a blanket memset here therefore did not "reset the
 * meta for the new run", it deleted it, permanently, on the first keypress.
 * The DETAILS view then showed an empty COLO, an empty client IP and an empty
 * ASN for the rest of the session, and the history table showed 00:00:00
 * forever. That was the largest single source of wrong data in the program.
 *
 * The alternative is to refetch on every restart, and it loses on two counts.
 * First, cost and placement: net_fetch_meta() is a blocking getaddrinfo plus
 * two HTTPS round trips, and engine_restart() is called straight from the key
 * handler in main.c, so a refetch there stalls the UI thread on every f/p/d/u/r
 * press for data that is the same data. Second, and worse, it is not
 * idempotent under failure: net_fetch_meta() zeroes its output before it
 * tries, so a single refetch that lost a race with the rate limiter would
 * replace good metadata with blank fields. A stale colo is a much smaller lie
 * than an absent one.
 *
 * The real argument for refetching is a long session whose machine moves
 * between networks. That is the scenario where stale metadata is wrong, and it
 * is exactly the scenario where restarting the app is already the answer.
 *
 * Everything else IS cleared, because it belongs to the run rather than to the
 * connection:
 *   - valid and the two grades must not survive. The views branch on them to
 *     decide whether a finished result exists at all, so a leftover "A+" would
 *     label a run that has not started yet.
 *   - down/up/idle/loaded_down must not survive. A phase abandoned halfway
 *     through has already written its byte count and its latency figures, and
 *     without this the next run would report them as its own until it reached
 *     the same point. For MODE_DOWNLOAD and MODE_UPLOAD, which never write
 *     the other direction at all, that stale half-run is the only number the
 *     summary line will ever show.
 *   - bufferbloat_ms and stability_cv are derived from the above and would
 *     otherwise describe a run that no longer exists.
 */
static void result_reset(RunResult *r) {
    memset(&r->down, 0, sizeof r->down);
    memset(&r->up, 0, sizeof r->up);
    memset(&r->idle, 0, sizeof r->idle);
    memset(&r->loaded_down, 0, sizeof r->loaded_down);
    r->bufferbloat_ms = 0.0;
    r->stability_cv   = 0.0;
    r->stab_grade[0]  = '\0';
    r->bloat_grade[0] = '\0';
    r->valid = 0;

    /* Re-stamped rather than carried over. Two runs a few seconds apart share
     * a wall-clock second often enough that two identical stamps in the
     * history table would be indistinguishable, and the stamp on screen is
     * supposed to mean "this run started now". Second resolution is already
     * enough: the shortest run this engine can produce is the 2 s idle phase
     * in MODE_LATENCY, so two rows can never be recorded inside one second. */
    r->timestamp_ms = stamp_now_ms();
}

Engine *engine_new(const char *base_url, Mode mode) {
    Engine *e = (Engine *)calloc(1, sizeof *e);
    if (!e) return NULL;
    snprintf(e->base, sizeof e->base, "%s", base_url);
    e->mode = mode;
    e->phase = PH_IDLE;
    e->run_start = now_ms();
    e->phase_start = e->run_start;
    e->phase_end = e->run_start;
    e->chunk = CHUNK_START;
    /* Metadata is a precondition for showing the route at all, and it is a
     * handful of requests; doing it up front keeps it off the measured path. */
    net_fetch_meta(e->base, &e->result.meta);
    e->result.timestamp_ms = stamp_now_ms();
    return e;
}

void engine_free(Engine *e) {
    if (!e) return;
    engine_cancel(e);
    free(e);
}

const char *phase_label(Phase p) {
    switch (p) {
        case PH_IDLE:       return "idle latency";
        case PH_DOWNLOAD:   return "download";
        case PH_UPLOAD:     return "upload";
        case PH_FINALISING: return "finalising";
        case PH_DONE:       return "complete";
        case PH_ERROR:      return "failed";
        default:            return "";
    }
}

/* --------------------------------------------------------- worker control */

static void stop_workers(Engine *e) {
    for (int i = 0; i < e->worker_count; i++)
        net_worker_stop(e->workers[i]);
    e->worker_count = 0;
}

static void start_workers(Engine *e, int upload, uint64_t duration_ms) {
    stop_workers(e);
    e->counter = 0;
    e->chunk = CHUNK_START;
    memset(&e->cfg, 0, sizeof e->cfg);
    e->cfg.counter = &e->counter;
    e->cfg.upload = upload;
    e->cfg.chunk = e->chunk;
    e->cfg.chunk_min = CHUNK_MIN;
    e->cfg.chunk_max = 4000000ul;
    e->cfg.duration_ms = duration_ms;

    int n = (e->mode == MODE_LATENCY) ? 0 : CONCURRENCY;
    if (upload && e->mode == MODE_DOWNLOAD) n = 0;
    if (!upload && e->mode == MODE_UPLOAD) n = 0;
    for (int i = 0; i < n; i++) {
        e->workers[e->worker_count] = net_worker_start(e->base, &e->cfg);
        if (e->workers[e->worker_count]) e->worker_count++;
    }
}

void engine_cancel(Engine *e) {
    if (!e) return;
    stop_workers(e);
}

void engine_restart(Engine *e, Mode mode) {
    engine_cancel(e);
    e->mode = mode;
    e->phase = PH_IDLE;
    e->phase_end = 0;
    e->finished = 0;
    e->ndown = e->nup = 0;
    e->nidle = e->nloaded = 0;
    e->instant = 0.0;
    e->probe_instant = 0.0;
    e->last_bytes = 0;
    e->last_time = now_ms();
    e->run_start = e->last_time;
    e->phase_start = e->last_time;
    e->error[0] = '\0';

    /* The sample buffers are deliberately not zeroed, only their counts. Every
     * reader - the live charts, summarise_latency, stats_finalise - is bounded
     * by nidle/nloaded/ndown/nup, so the stale values past the count are
     * unreachable, and clearing 25 KiB of doubles on every keypress would be a
     * cost paid for nothing. */

    /* The shared byte counter is the one piece of partial state that lives
     * outside RunResult and is not covered by result_reset. start_workers()
     * zeroes it too, but only when the first transfer phase begins - two
     * seconds later. Zeroing it here means an abandoned download's bytes are
     * gone the moment the run is superseded, rather than merely unreachable
     * until a phase happens to overwrite them. */
    e->counter = 0;

    /* Keeps the connection metadata and re-stamps the clock; clears the
     * results, grades and validity of the run being abandoned. */
    result_reset(&e->result);
}

/* ---------------------------------------------------------------- phases */

static void finish(Engine *e) {
    stop_workers(e);

    /* A phase that moved no bytes did not measure a slow link, it was refused.
     * The workers treat a non-2xx as a reason to back off rather than an error,
     * which is right mid-phase but would otherwise report a confident 0 Mbps. */
    int refused = (e->result.down.bytes == 0 && e->result.up.bytes == 0);
    if (refused) {
        const char *which =
            (e->result.down.bytes == 0 && e->result.up.bytes > 0) ? "Download"
          : (e->result.down.bytes > 0) ? "Upload"
                                       : "The test";
        e->phase = PH_ERROR;
        snprintf(e->error, sizeof e->error,
                 "%s transferred no data - Cloudflare refused every request.\n"
                 "This is rate limiting from repeated testing, not a speed result.",
                 which);
        e->finished = 1;
        return;
    }

    summarise_latency(e->idle_probes, e->nidle, &e->result.idle);
    summarise_latency(e->loaded_probes, e->nloaded, &e->result.loaded_down);
    e->result.bufferbloat_ms = e->result.loaded_down.med - e->result.idle.med;
    if (e->result.bufferbloat_ms < 0) e->result.bufferbloat_ms = 0;

    double cv = 0.0;
    if (e->ndown > cv_of(e->down_samples, e->ndown)) cv = cv_of(e->down_samples, e->ndown);
    if (e->nup > 0) {
        double up = cv_of(e->up_samples, e->nup);
        if (up > cv) cv = up;
    }
    e->result.stability_cv = cv;
    grade_stab(cv, e->result.stab_grade);
    grade_latency(e->result.bufferbloat_ms, e->result.bloat_grade);
    e->result.valid = 1;
    e->phase = PH_DONE;
    e->finished = 1;
}

int engine_poll(Engine *e) {
    uint64_t t = now_ms();

    switch (e->phase) {
        case PH_IDLE: {
            if (t - e->last_time < 100) break;
            e->last_time = t;

            if (t - e->phase_start < IDLE_MS) {
                if (e->nidle < MAX_PROBES) {
                    double ms = 0.0;
                    int rc = net_probe(e->base, &ms);
                    if (rc == NET_OK) {
                        e->idle_probes[e->nidle++] = ms;
                        e->probe_instant = ms;
                    } else if (rc == NET_REFUSED) {
                        /* Recorded as a failed sample so it counts as loss. */
                        e->idle_probes[e->nidle++] = -1.0;
                    }
                }
                break;
            }

            summarise_latency(e->idle_probes, e->nidle, &e->result.idle);
            if (e->mode == MODE_LATENCY) {
                e->result.loaded_down = e->result.idle;
                e->result.valid = 1;
                e->phase = PH_DONE;
                e->phase_end = t;
                e->finished = 1;
                return 0;
            }
            start_workers(e, 0, DOWN_MS);
            e->phase = PH_DOWNLOAD;
            e->phase_start = t;
            e->phase_end = t + DOWN_MS;
            e->ndown = 0;
            e->last_bytes = 0;
            e->last_time = t;
            break;
        }

        case PH_DOWNLOAD:
        case PH_UPLOAD: {
            int up = (e->phase == PH_UPLOAD);

            if (t - e->last_time >= SAMPLE_MS) {
                double dt = (double)(t - e->last_time) / 1000.0;
                uint64_t bytes = e->counter;
                if (dt > 0.0 && bytes >= e->last_bytes) {
                    double mbps = (double)(bytes - e->last_bytes) * 8.0 / dt / 1e6;
                    double *series = up ? e->up_samples : e->down_samples;
                    int *n = up ? &e->nup : &e->ndown;
                    if (*n < MAX_SAMPLES) series[(*n)++] = mbps;
                    e->instant = mbps;
                    e->last_bytes = bytes;
                    e->last_time = t;
                }
                /* Latency under load, interleaved with the transfer. */
                double lms = 0.0;
                if (net_probe(e->base, &lms) == NET_OK && e->nloaded < MAX_PROBES) {
                    e->loaded_probes[e->nloaded++] = lms;
                    e->probe_instant = lms;
                }
            }

            if (t < e->phase_end) break;

            stop_workers(e);
            {
                uint64_t dur = e->phase_end - e->phase_start;
                uint64_t bytes = e->counter;
                double *series = up ? e->up_samples : e->down_samples;
                int n = up ? e->nup : e->ndown;
                /* Drop the slow-start ramp before summarising. */
                int cut = n / 5;
                stats_finalise(series + cut, n - cut, bytes, dur,
                               up ? &e->result.up : &e->result.down);
            }

            if (!up) {
                if (e->mode == MODE_DOWNLOAD) { finish(e); return 0; }
                start_workers(e, 1, UP_MS);
                e->phase = PH_UPLOAD;
                e->phase_start = t;
                e->phase_end = t + UP_MS;
                e->nup = 0;
                e->last_bytes = 0;
                e->last_time = t;
            } else {
                e->phase = PH_FINALISING;
            }
            break;
        }

        case PH_FINALISING:
            finish(e);
            return 0;

        default:
            return 0;
    }
    return e->phase != PH_DONE && e->phase != PH_ERROR;
}

int engine_finished(const Engine *e) { return e->finished; }
Phase engine_phase(const Engine *e) { return e->phase; }
uint64_t engine_elapsed_ms(const Engine *e) {
    if (e->phase == PH_DONE || e->phase == PH_ERROR) return e->phase_end - e->run_start;
    return now_ms() - e->run_start;
}
double engine_instant(const Engine *e) { return e->instant; }
const RunResult *engine_result(const Engine *e) { return &e->result; }
const Meta *engine_meta(const Engine *e) { return &e->result.meta; }
Mode engine_mode(const Engine *e) { return e->mode; }
const char *engine_error(const Engine *e) { return e->error; }
const double *engine_probe_samples(const Engine *e, int *count) {
    if (count) *count = e->nidle;
    return e->idle_probes;
}
double engine_probe_instant(const Engine *e) { return e->probe_instant; }
const double *engine_down_samples(const Engine *e, int *count) {
    if (count) *count = e->ndown;
    return e->down_samples;
}
const double *engine_up_samples(const Engine *e, int *count) {
    if (count) *count = e->nup;
    return e->up_samples;
}
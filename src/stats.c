/* Pure descriptive statistics for measurement samples.
 *
 * Every function here is total: any input, including NULL, empty arrays,
 * NaN/Inf samples and huge n, yields a finite result or exactly 0.0. The
 * renderer draws these numbers straight into bar charts, and a NaN reaching a
 * width computation silently poisons the whole layout, so the contract is
 * enforced at the boundary rather than trusted from callers.
 */
#include "stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Above this we cannot keep a copy on the stack; 2 KiB is cheap enough that
 * the common case never touches the allocator. */
#define STATS_STACK_CAP ((size_t)256)

/* ------------------------------------------------------------------ guards */

/* NaN is the only value that renders as garbage; inf draws as a full bar. */
static double sane(double x) {
    return isfinite(x) ? x : 0.0;
}

/* A total order is only meaningful once NaN is gone, which is why callers
 * filter before sorting rather than teaching the comparator about NaN. */
static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ----------------------------------------------------------------- scratch */

typedef struct {
    double *p;                       /* points at stack[] or the heap block */
    double  stack[STATS_STACK_CAP];
    int     on_heap;
} Scratch;

static void scratch_free(Scratch *s) {
    if (s->on_heap) {
        free(s->p);
        s->on_heap = 0;
    }
    s->p = NULL;
}

/* Returns 0 only when n exceeds SIZE_MAX/sizeof(double) or malloc fails. */
static int scratch_init(Scratch *s, size_t n) {
    s->p = s->stack;
    s->on_heap = 0;
    if (n <= STATS_STACK_CAP) return 1;
    if (n > (size_t)-1 / sizeof(double)) return 0;
    double *blk = (double *)malloc(n * sizeof(double));
    if (!blk) return 0;
    s->p = blk;
    s->on_heap = 1;
    return 1;
}

/* Copies the finite samples, sorts them, and reports how many survived.
 * Dropping non-finite values rather than poisoning them keeps n honest: a
 * stalled socket that yielded three NaN probes must not shift every rank. */
static int sorted_scratch(const double *v, size_t n, Scratch *s, size_t *out_m) {
    if (!v || n == 0 || !out_m) return 0;
    if (!scratch_init(s, n)) return 0;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (isfinite(v[i])) s->p[m++] = v[i];
    }
    if (m > 1) qsort(s->p, m, sizeof(double), cmp_double);
    *out_m = m;
    return 1;
}

/* -------------------------------------------------------------- primitives */

/* 0-based index of the nearest-rank element: ceil(p*m), clamped to 1..m, so
 * p=0 yields the smallest and p=1 the largest. */
static size_t rank_index(size_t m, double p) {
    if (m == 0) return 0;
    double q;
    if (!(p > 0.0))      q = 0.0;   /* the ! catches NaN, which must not pass */
    else if (p > 1.0)    q = 1.0;
    else                 q = p;
    double r = ceil(q * (double)m);
    if (r < 1.0)          r = 1.0;
    else if (r > (double)m) r = (double)m;
    return (size_t)r - 1;
}

/* Kahan summation: latency samples share a large offset, so a naive running
 * sum drops exactly the low bits that the variance is computed from. */
static double kahan_mean(const double *s, size_t m) {
    double sum = 0.0, comp = 0.0;
    for (size_t i = 0; i < m; i++) {
        double y = s[i] - comp;
        double t = sum + y;
        comp = (t - sum) - y;
        sum = t;
    }
    return (m == 0) ? 0.0 : sum / (double)m;
}

/* Unsorted mean over the finite samples, reporting how many were usable. */
static double mean_finite(const double *v, size_t n, size_t *out_m) {
    if (!v) n = 0;
    double sum = 0.0, comp = 0.0;
    size_t  m = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) continue;
        double y = v[i] - comp;
        double t = sum + y;
        comp = (t - sum) - y;
        sum = t;
        m++;
    }
    if (out_m) *out_m = m;
    if (m == 0) return 0.0;
    return sane(sum / (double)m);
}

/* Mean of an already-compacted, already-sorted scratch copy. */
static double mean_sorted(const double *s, size_t m) {
    if (m == 0) return 0.0;
    return sane(kahan_mean(s, m));
}

/* ----------------------------------------------------------------- exports */

double stats_percentile(const double *v, size_t n, double p) {
    Scratch s;
    size_t  m;
    if (!sorted_scratch(v, n, &s, &m)) return 0.0;
    double out = (m == 0) ? 0.0 : s.p[rank_index(m, p)];
    scratch_free(&s);
    return sane(out);
}

/* Median is p50 under the same nearest-rank rule the other percentiles use:
 * one definition, so the chart legend can never disagree with the summary. */
double stats_median(const double *v, size_t n) {
    return stats_percentile(v, n, 0.5);
}

/* Summing needs no ordering, so this stays allocation-free; only the
 * rank-based exports pay for a sorted scratch copy. */
double stats_mean(const double *v, size_t n) {
    if (!v || n == 0) return 0.0;
    return sane(mean_finite(v, n, NULL));
}

double stats_min(const double *v, size_t n) {
    if (!v || n == 0) return 0.0;
    double out = 0.0;
    int    seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) continue;
        if (!seen || v[i] < out) { out = v[i]; seen = 1; }
    }
    return sane(out);
}

double stats_max(const double *v, size_t n) {
    if (!v || n == 0) return 0.0;
    double out = 0.0;
    int    seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) continue;
        if (!seen || v[i] > out) { out = v[i]; seen = 1; }
    }
    return sane(out);
}

/* Two-pass population variance over the finite samples: the textbook one-pass
 * sum-of-squares form goes negative once the values sit far from zero, and
 * square-rooting that yields NaN. Sample variance (n-1) would understate
 * spread for short runs, which is the common case in a progress bar. */
double stats_stddev(const double *v, size_t n) {
    if (!v || n == 0) return 0.0;
    size_t  m = 0;
    double mu = mean_finite(v, n, &m);
    if (m == 0) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(v[i])) continue;
        double d = v[i] - mu;
        acc += d * d;
    }
    double var = acc / (double)m;
    return sane(var > 0.0 ? sqrt(var) : 0.0);
}

/* RFC 3550 interarrival jitter: mean |v[i]-v[i-1]| over adjacent finite
 * pairs. Pairs touching a NaN are skipped, not bridged, because interpolating
 * across a dropped probe would invent a latency the network never showed. */
double stats_jitter(const double *v, size_t n) {
    if (!v || n < 2) return 0.0;
    double acc = 0.0;
    size_t  pairs = 0;
    for (size_t i = 1; i < n; i++) {
        if (!isfinite(v[i]) || !isfinite(v[i - 1])) continue;
        acc += fabs(v[i] - v[i - 1]);
        pairs++;
    }
    if (pairs == 0) return 0.0;
    return sane(acc / (double)pairs);
}

/* A non-positive mean has no meaningful relative spread, so CV is reported as
 * 0 rather than dividing by ~0 and blowing the bar up to infinity. */
double stats_cv_percent(const double *v, size_t n) {
    double mean = stats_mean(v, n);
    if (!(mean > 0.0)) return 0.0;
    return sane(stats_stddev(v, n) / mean * 100.0);
}

double stats_loss_percent(size_t received, size_t sent) {
    if (sent == 0 || received >= sent) return 0.0;  /* over-report is not loss */
    return sane((double)(sent - received) * 100.0 / (double)sent);
}

/* Shifting every sample by a constant is the same operation as subtracting
 * the idle median, so it is done in place on the sorted scratch copy. */
double stats_bufferbloat(const double *loaded, size_t n, double idle_median) {
    if (!loaded || n == 0 || !(idle_median > 0.0)) return 0.0;
    Scratch s;
    size_t  m;
    if (!sorted_scratch(loaded, n, &s, &m)) return 0.0;
    for (size_t i = 0; i < m; i++) s.p[i] = sane(s.p[i] - idle_median);
    double out = (m == 0) ? 0.0 : s.p[rank_index(m, 0.5)];
    scratch_free(&s);
    return sane(out);
}

/* Comparisons are written as plain <=; a NaN argument fails every test and
 * falls through to the worst grade, which is the honest reading of a failed
 * measurement. */
char stats_grade_bufferbloat(double ms) {
    if (ms <=   5.0) return 'A';
    if (ms <=  30.0) return 'B';
    if (ms <=  50.0) return 'C';
    if (ms <= 100.0) return 'D';
    if (ms <= 200.0) return 'E';
    return 'F';
}

char stats_grade_stability(double cv_percent) {
    if (cv_percent <=  2.0) return 'A';
    if (cv_percent <=  4.0) return 'B';
    if (cv_percent <=  8.0) return 'C';
    if (cv_percent <= 15.0) return 'D';
    if (cv_percent <= 25.0) return 'E';
    return 'F';
}

char stats_grade_loss(double loss_percent) {
    if (loss_percent <= 0.0) return 'A';   /* zero, and the negative over-count */
    if (loss_percent <= 0.5) return 'B';
    if (loss_percent <= 1.0) return 'C';
    if (loss_percent <= 3.0) return 'D';
    if (loss_percent <= 8.0) return 'E';
    return 'F';
}

/* Trimming is only worth its rank lookups once there are enough samples for
 * the tails to actually hold something, hence the 8-sample floor; below it the
 * 5th and 95th ranks collapse onto the endpoints and cost a sort for no gain. */
double stats_trimmed_mean(const double *v, size_t n) {
    if (!v || n == 0) return 0.0;
    Scratch s;
    size_t  m;
    if (!sorted_scratch(v, n, &s, &m)) return stats_mean(v, n);
    double out;
    if (m < 8) {
        out = mean_sorted(s.p, m);
    } else {
        size_t lo = rank_index(m, 0.05);
        size_t hi = rank_index(m, 0.95);
        if (hi < lo) hi = lo;
        double acc = 0.0;
        for (size_t i = lo; i <= hi; i++) acc += s.p[i];
        out = acc / (double)(hi - lo + 1);
    }
    scratch_free(&s);
    return sane(out);
}
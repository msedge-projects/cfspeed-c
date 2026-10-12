/* Chart scaling and bucketing math. See chart.h for the contract. */
#include "chart.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ stats */

/* chart.c owns its own order statistic rather than calling into stats.h.
 * stats.h's stats_percentile is nearest-rank, but ui.c prints the
 * linearly-interpolated p95 that engine.c computes, so delegating would
 * quietly change the number on screen. Keeping a private copy also makes
 * chart.c linkable with no other object and allocates nothing, whereas the
 * shared helpers must sort, which means malloc. */
static double chart_local_percentile(const double *s, size_t n, double p);

/* k-th smallest finite sample, k 1-based over the finite ones only, in O(1)
 * memory and no allocation.
 *
 * Bisects the value range until the bracket collapses onto a single
 * representable double. At that point count(s < hi) < k <= count(s <= hi)
 * holds, which is exactly the definition of the k-th order statistic -- so
 * this returns the true value, not an approximation. Two hundred halvings
 * cannot be exhausted by any finite input, so the loop is provably
 * terminating; the guard just makes that visible to the reader.
 *
 * Non-finite samples are excluded here for the same reason they are excluded
 * from peak and mean: one inf from a failed transfer must not be able to
 * report an infinite p95 on a trace whose peak is 40. */
static double order_stat(const double *s, size_t n, size_t k) {
    if (n == 0) return 0.0;

    double lo = 0.0, hi = 0.0;
    size_t seen = 0;
    for (size_t i = 0; i < n; i++) {
        double v = s[i];
        if (!isfinite(v)) continue;
        if (seen == 0 || v < lo) lo = v;
        if (seen == 0 || v > hi) hi = v;
        seen++;
    }
    if (seen == 0) return 0.0;
    if (k < 1) k = 1;
    if (k > seen) k = seen;
    if (!(lo < hi)) return hi;

    for (int it = 0; it < 200; it++) {
        /* Halving the endpoints rather than (lo+hi)/2 cannot overflow on
         * extreme magnitudes. */
        double mid = lo * 0.5 + hi * 0.5;
        if (!(mid > lo && mid < hi)) break; /* adjacent doubles: converged */

        size_t c = 0;
        for (size_t i = 0; i < n; i++)
            if (isfinite(s[i]) && s[i] <= mid) c++;
        if (c >= k) hi = mid; else lo = mid;
    }
    return hi;
}

static double chart_local_percentile(const double *s, size_t n, double p) {
    if (n == 0) return 0.0;

    /* Rank space is over the finite samples, matching order_stat's domain. */
    size_t seen = 0;
    for (size_t i = 0; i < n; i++)
        if (isfinite(s[i])) seen++;
    if (seen == 0) return 0.0;
    if (seen == 1) return order_stat(s, n, 1);

    /* Linear interpolation between the two order statistics bracketing p, as
     * engine.c already does: the result stays continuous in n instead of
     * jumping a whole step whenever a sample lands exactly on a rank. */
    double pos = p * (double)(seen - 1);
    size_t ilo = (size_t)floor(pos);
    size_t ihi = (size_t)ceil(pos);
    if (ilo == ihi) return order_stat(s, n, ilo + 1);

    double frac = pos - (double)ilo;
    double a = order_stat(s, n, ilo + 1);
    double b = order_stat(s, n, ihi + 1);
    return a * (1.0 - frac) + b * frac;
}

/* ------------------------------------------------------------------ scale */

void chart_scale(const double *samples, size_t n, int width, ChartScale *out) {
    if (!out) return;

    /* bars == 0 is the contract: h[] is entirely undefined, including the
     * NaN that a zero-height normalisation would otherwise produce. The
     * struct is zeroed wholesale, so no caller can read a stale height. */
    memset(out, 0, sizeof *out);
    if (!samples || n == 0 || width <= 0) {
        /* Nothing was plotted, so the axis is as dead as an all-zero trace:
         * flag it rather than leaving zeroed clear, or a caller drawing
         * "0 samples" and a caller drawing "50 Mbps of nothing" both claim
         * to have data. */
        out->zeroed = 1;
        return;
    }

    int bars = width < CHART_MAX_BARS ? width : CHART_MAX_BARS;
    if ((size_t)bars > n) bars = (int)n;
    out->bars = bars;

    /* Accumulated over finite samples only. One NaN from a failed transfer
     * must not turn the mean -- and with it every downstream label -- to NaN. */
    double peak = 0.0, sum = 0.0;
    size_t  counted = 0;
    for (size_t i = 0; i < n; i++) {
        double v = samples[i];
        if (!isfinite(v)) continue;
        if (v > peak) peak = v;
        sum += v;
        counted++;
    }
    out->peak = peak;
    out->zeroed = peak <= 0.0;

    if (out->zeroed) {
        /* Documented as all-zero, so mean and p95 are forced too: a negative
         * mean beside a zero-topped axis would be a lie, not a statistic. */
        out->mean = 0.0;
        out->p95 = 0.0;
        return;
    }

    out->mean = counted ? sum / (double)counted : 0.0;
    out->p95 = chart_local_percentile(samples, n, 0.95);

    /* Normalising against the nice ceiling rather than the raw peak keeps the
     * peak bar just short of the top edge, and makes heights directly
     * comparable with chart_ticks() over the same top. */
    double top = chart_nice_ceiling(peak);

    for (int i = 0; i < bars; i++) {
        double v = 0.0;
        if ((size_t)bars == n) {
            /* Fewer samples than columns: one sample per column, no padding. */
            v = samples[i];
        } else {
            /* Peak of the bucket. Integer bucket edges keep every column the
             * same width and every sample in exactly one column, with no
             * drift accumulating over 1200 iterations. */
            size_t lo = (size_t)i * n / (size_t)bars;
            size_t hi = ((size_t)i + 1) * n / (size_t)bars;
            if (hi <= lo) hi = lo + 1;
            if (hi > n) hi = n;
            for (size_t k = lo; k < hi; k++)
                if (samples[k] > v) v = samples[k];
        }

        double bh = v / top;
        if (!(bh > 0.0)) bh = 0.0; /* negatives and NaN collapse to the floor */
        if (bh > 1.0) bh = 1.0;
        out->h[i] = (float)bh;
    }
}

/* ------------------------------------------------------------------- axis */

double chart_nice_ceiling(double raw) {
    if (!(raw > 0.0)) return 1.0; /* NaN, zero, negatives: any unit will do */
    if (!isfinite(raw)) raw = 1e300; /* +inf: name the largest decade we can */

    int e = (int)floor(log10(raw));
    double m = raw / pow(10.0, (double)e);

    /* log10 can land a hair either side of a decade boundary through rounding,
     * which would turn 100.0 into 10x10^1 and the label into 1000. Re-derive
     * the mantissa until it is genuinely in [1,10). */
    while (m >= 10.0) { m /= 10.0; e++; }
    while (m < 1.0) { m *= 10.0; e--; }

    double nice;
    if (m <= 1.0) nice = 1.0;
    else if (m <= 2.0) nice = 2.0;
    else if (m <= 5.0) nice = 5.0;
    else nice = 10.0;

    return nice * pow(10.0, (double)e);
}

/* Shortest rendering that is still unambiguous. Fixed point everywhere an
 * axis actually lives; exponent form only where it would otherwise be an
 * unreadable run of zeros. Keeps the formatted length bounded, which is what
 * lets the buffer arithmetic below stay simple. */
static int fmt_tick(char *buf, size_t bufsz, double v, int prec) {
    double a = v < 0.0 ? -v : v;
    if (a >= 1e15 || (a > 0.0 && a < 1e-4)) return snprintf(buf, bufsz, "%.0e", v);
    return snprintf(buf, bufsz, "%.*f", prec, v);
}

/* Append one label, separator included, only when the whole thing fits.
 * Refusing rather than clipping means a short buffer yields a shorter axis
 * instead of a corrupt one. */
static int tick_push(char *out, size_t cap, size_t *used, double v, int prec) {
    char tmp[32];
    int n = fmt_tick(tmp, sizeof tmp, v, prec);
    if (n < 0 || (size_t)n >= sizeof tmp) return 0;

    size_t len = (size_t)n;
    size_t sep = *used ? 1u : 0u; /* separator between labels only */
    if (*used + sep + len + 1u > cap) return 0; /* +1 for the terminator */

    if (sep) out[*used] = '\n';
    memcpy(out + *used + sep, tmp, len + 1u);
    *used += sep + len;
    return 1;
}

int chart_ticks(double top, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    /* Same fallback as ui.c's draw_chart: a dead axis still needs a scale to
     * draw the baseline against. The lower clamp keeps step away from
     * underflow, which would otherwise collapse every label to zero. */
    if (!(top > 0.0) || top < 1e-9) top = 1.0;
    if (!isfinite(top)) top = 1e300;

    /* One step at or above top/5 means top/step lands in [2.5,5], so the axis
     * always resolves to between four and six labels without a search. */
    double step = chart_nice_ceiling(top / 5.0);

    int prec = 0;
    for (double t = step; t < 1.0 && prec < 6; t *= 10.0) prec++;

    double eps = top * 1e-9; /* tolerant equality: 5*step may land a ulp over */
    double last = 0.0;
    size_t used = 0;
    int count = 0;

    for (int k = 0; k <= 6; k++) {
        double v = (double)k * step;
        if (k > 0 && v >= top) break;
        if (k > 0 && fabs(v - last) < eps) continue;
        if (!tick_push(out, cap, &used, v, prec)) break;
        last = v;
        count++;
    }

    /* Anchor the run on `top` itself, unless the grid already landed there. */
    if (fabs(top - last) >= eps && tick_push(out, cap, &used, top, prec)) count++;

    return count;
}

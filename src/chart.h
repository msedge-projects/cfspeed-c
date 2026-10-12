/* Chart scaling and bucketing math.
 *
 * Deliberately free of any terminal dependency: this file knows how many
 * columns there are and how tall each one is, nothing about cells, colour or
 * cursor position. A renderer turns the heights into rows of block glyphs.
 * Keeping the arithmetic pure is what makes the axis testable without a tty.
 *
 * The semantics are lifted from draw_chart() in ui.c, unchanged: buckets keep
 * the peak rather than the mean, because a speed trace exists to show the
 * spike, and averaging it away is the one thing it must not do. */
#ifndef CFS_CHART_H
#define CFS_CHART_H

#include <stddef.h>

/* stats.h is owned by a sibling module that may not have landed yet. Feature-
 * detect rather than hard-include so this header stays usable in either state;
 * chart.c carries a local copy of the one statistic it needs when it is absent. */
#if defined(__has_include)
#  if __has_include("stats.h")
#    include "stats.h"
#    define CFS_CHART_HAVE_STATS_H 1
#  endif
#endif
#ifndef CFS_CHART_HAVE_STATS_H
#  define CFS_CHART_HAVE_STATS_H 0
#endif

#define CHART_MAX_BARS 512

typedef struct {
    /* One normalised height per column, 0.0..1.0. The caller turns these
       into rows of block characters.
       Only indices [0, bars) are written by chart_scale(); every index >= bars
       holds whatever was in the struct before, and must not be read. A
       zero-length call (no samples, or width <= 0) leaves bars == 0, which is
       the caller's signal that there is nothing to draw at all. */
    float  h[CHART_MAX_BARS];
    double peak;        /* largest sample, for the axis top label */
    double mean;
    double p95;
    int    bars;        /* how many entries of h[] are valid */
    int    zeroed;      /* 1 when every sample was <= 0 */
} ChartScale;

/* Build a ChartScale from n samples into width columns.
   - If width exceeds CHART_MAX_BARS it is clamped, never overflowed.
   - Bucketing is peak-of-bucket: the tallest sample in each column wins.
   - If every sample is <= 0 the result is all-zero with zeroed=1 and peak 0,
     so the caller can show an axis without inventing a non-zero height.
     A call with no samples, or a non-positive width, is vacuously of that
     kind too: bars == 0 and zeroed == 1.
   Heights are normalised against chart_nice_ceiling(peak), so pairing them
   with chart_ticks(chart_nice_ceiling(peak)) gives a bar/label pair that agree
   and still leaves the peak short of the top edge. */
void chart_scale(const double *samples, size_t n, int width, ChartScale *out);

/* Pick a "nice" axis maximum (1/2/5 x 10^k) at or above `raw`, so the top
   label reads 100 rather than 87.3. Never returns 0; a raw <= 0 yields 1. */
double chart_nice_ceiling(double raw);

/* Write up to `cap` bytes of human-readable tick labels for an axis running
   0..top into `out`, newline separated, e.g. "0\n50\n100". `cap` is a byte
   count including the terminator. Returns the number of labels written.
   Produces 4-6 labels; never writes past cap. If the labels do not fit, the
   run stops at the last whole label that does -- output is always a
   NUL-terminated, well-formed prefix, never a clipped one. */
int   chart_ticks(double top, char *out, size_t cap);

#endif /* CFS_CHART_H */

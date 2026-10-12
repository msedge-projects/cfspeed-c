/* Unit tests for the four pure modules: stats, chart, fmt, actions.
 *
 * These modules have the two properties this project cares most about --
 * they are total (any input yields a usable answer) and they are pure (no
 * terminal, no clock, no network) -- and until now neither was checked. The
 * tests below are written to catch the failure modes that actually reach a
 * user: a NaN that renders as garbage, a median that quietly disagrees with
 * the legend, a buffer that runs one byte past its cap, and a menu highlight
 * that lands on an item the user cannot pick.
 *
 * Nothing here may be "fixed" by relaxing an assertion. If a test in this file
 * fails, the module is wrong, and the fix belongs in the module.
 *
 * Build:
 *   cc -std=gnu11 -O2 -Wall -Wextra -o t tests/test_units.c \
 *      src/test.c src/stats.c src/chart.c src/fmt.c src/actions.c -lm
 */
#include "test.h"

#include "actions.h"
#include "chart.h"
#include "fmt.h"
#include "stats.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================== helpers */

/* The harness never passes the value under test to FAIL_MSG, so tests that care
 * about a specific string say it themselves. */
static void fail_str(const char *what, const char *got)
{
    FAIL_MSG("%s produced \"%s\"", what, got ? got : "(null)");
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring search written by hand rather than with strcasestr
 * or tolower: strcasestr is POSIX-only and tolower is UB on a negative char,
 * and this file has to build identically on MinGW and on glibc/macOS. */
static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;

    if (hay == NULL || nl == 0) return 0;
    for (; *hay != '\0'; hay++) {
        for (i = 0; i < nl && hay[i] != '\0'; i++) {
            if (ascii_lower(hay[i]) != needle[i]) break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

/* The single most important assertion in this file.
 *
 * fmt.c's outputs are written straight to a tty. C's printf renders a
 * non-finite double as "nan" or "inf", which is the exact string that makes a
 * terminal draw escape sequences or garbage, and it is also what a reader
 * mistakes for a measurement. So every formatter, for every hostile input, has
 * to produce something that is finite or the "--" placeholder -- never those
 * two three-letter words. Checking with strstr would only catch the lowercase
 * spelling; glibc prints "nan" but the casing has varied, so scan both. */
static void assert_no_inf(const char *s)
{
    if (s == NULL) {
        FAIL_MSG("formatter returned NULL");
        return;
    }
    if (contains_ci(s, "nan")) fail_str("nan in output", s);
    if (contains_ci(s, "inf")) fail_str("inf in output", s);
}

/* Deterministic 64-bit xorshift. Seeded explicitly per test so a CI failure
 * reproduces exactly; rand() would not, because its sequence differs between
 * the CRT implementations this project is built with. */
#define FUZZ_SEED 0x243F6A8885A308D3ULL
static uint64_t g_rng = FUZZ_SEED;

static uint64_t rng_next(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

/* ========================================================================= stats */

/* 1..100. Long enough to give the trimmed mean real tails to cut, which is the
 * whole point of that function. */
static void ramp_1_to_100(double *out)
{
    for (int i = 0; i < 100; i++) out[i] = (double)(i + 1);
}

TEST(stats_percentiles_match_the_nearest_rank_definition)
{
    /* Nearest rank, ceil(p*n) clamped to 1..n. These are known values rather
     * than values read back out of the implementation, so switching to linear
     * interpolation -- which is a defensible choice many libraries make -- is
     * caught here instead of silently changing every latency number on screen. */
    const double v[10] = { 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 }; /* deliberately unsorted */
    ASSERT_DBL(stats_percentile(v, 10, 0.00), 1.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 0.10), 1.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 0.25), 3.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 0.50), 5.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 0.75), 8.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 0.90), 9.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 0.95), 10.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 1.00), 10.0, 0.0);

    /* Out-of-range p must clamp rather than index off the end. */
    ASSERT_DBL(stats_percentile(v, 10, -5.0), 1.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 10, 7.5), 10.0, 0.0);

    /* A NaN percentile is not a percentile; it must not reach rank_index as a
     * comparison that quietly reads as "p == 0" through a different path. */
    ASSERT(stats_percentile(v, 10, NAN) >= 1.0);
}

TEST(stats_median_even_length_is_the_documented_nearest_rank_p50)
{
    /* stats.h defines percentiles as nearest-rank and stats.c defines median as
     * p50 under that same rule, on purpose, so the chart legend and the summary
     * can never disagree. The consequence on an even-length array is that median
     * is the LOWER of the two middle values, not their average.
     *
     * This matters enough to be pinned rather than inferred: the classic
     * averaging median would give 25.0 here and the module gives 20.0. If the
     * averaging definition is ever wanted, change it in stats.c -- and change
     * this expectation with it, deliberately. */
    const double v[4] = { 10.0, 20.0, 30.0, 40.0 };
    ASSERT_DBL(stats_median(v, 4), 20.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 4, 0.5), 20.0, 0.0);

    /* Odd length has no such ambiguity: the middle value is the median. */
    const double odd[5] = { 50.0, 10.0, 30.0, 20.0, 40.0 };
    ASSERT_DBL(stats_median(odd, 5), 30.0, 0.0);
}

TEST(stats_mean_and_stddev_on_a_known_array)
{
    const double v[8] = { 2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0 };
    ASSERT_DBL(stats_mean(v, 8), 5.0, 1e-12);
    ASSERT_DBL(stats_stddev(v, 8), 2.0, 1e-12); /* population sd of this set */
    ASSERT_DBL(stats_min(v, 8), 2.0, 0.0);
    ASSERT_DBL(stats_max(v, 8), 9.0, 0.0);
}

TEST(stats_stddev_survives_a_large_constant_offset)
{
    /* The textbook one-pass sum-of-squares form (E[x^2] - E[x]^2) computes
     * 1e16 minus 1e16 here and loses every significant bit -- and square
     * rooting the negative residue gives NaN, which then poisons the chart.
     * Latency samples sit far from zero in exactly this way (a 12 ms idle
     * offset with 1 ms of jitter), so this is the realistic case, not a
     * synthetic one. stats.c uses a two-pass mean, so the answer is exact. */
    const double v[4] = { 1.0e8, 1.0e8 + 1.0, 1.0e8 + 2.0, 1.0e8 + 3.0 };
    ASSERT_DBL(stats_mean(v, 4), 1.0e8 + 1.5, 1e-6);
    ASSERT_DBL(stats_stddev(v, 4), 1.118033988749895, 1e-9);

    /* Same thing one decade up: doubles have 53 bits, so at 1e9 the two
     * squares share only their top ~16 digits. */
    const double w[4] = { 1.0e9, 1.0e9 + 1.0, 1.0e9 + 2.0, 1.0e9 + 3.0 };
    ASSERT_DBL(stats_stddev(w, 4), 1.118033988749895, 1e-9);
}

TEST(stats_stddev_of_an_all_equal_array_is_exactly_zero)
{
    /* Exact, not "near zero". An all-equal run is the common case at the start
     * of a speed test before any variance appears, and a stability grade must
     * read 'A' for it rather than a rounding artefact. Tolerance 0 makes any
     * nonzero residue -- or a NaN from a negative variance -- fail. */
    const double v[16];
    for (int i = 0; i < 16; i++) v[i] = 5.0;
    ASSERT_DBL(stats_stddev(v, 16), 0.0, 0.0);
    ASSERT_DBL(stats_mean(v, 16), 5.0, 0.0);
}

TEST(stats_cv_of_an_all_equal_array_is_zero_and_not_nan)
{
    /* This is the specific NaN trap in the module. CV is stddev/mean, and the
     * all-equal array is exactly the input where a mean of 0 meets a divide --
     * 0/0 is NaN. A NaN CV reaches the UI as a stability bar with no height and
     * a letter grade of 'F' for a perfectly steady link.
     *
     * ASSERT_DBL treats NaN as a failure by construction (the harness checks
     * is_nan() before comparing), so these two lines fail on NaN even though
     * "got != want" would also be true of it. */
    const double v[8];
    for (int i = 0; i < 8; i++) v[i] = 100.0;
    ASSERT_DBL(stats_cv_percent(v, 8), 0.0, 1e-12);

    /* Same division, reached through a mean that is genuinely 0. */
    const double zeros[4] = { 0.0, 0.0, 0.0, 0.0 };
    ASSERT_DBL(stats_cv_percent(zeros, 4), 0.0, 1e-12);

    /* And the ordinary case, so the zero above cannot be produced by a
     * function that always returns 0. */
    const double spread[4] = { 90.0, 100.0, 110.0, 100.0 };
    ASSERT_DBL(stats_cv_percent(spread, 4), 8.0, 1e-9); /* sd 7.0710678 / 100 */
}

TEST(stats_jitter_of_short_arrays_is_zero)
{
    /* A single probe has no interarrival gap, so jitter is 0 by definition.
     * Anything else makes the first frame of a run show phantom jitter. */
    const double one[1] = { 42.0 };
    ASSERT_DBL(stats_jitter(one, 1), 0.0, 0.0);
    ASSERT_DBL(stats_jitter(NULL, 0), 0.0, 0.0);

    /* RFC 3550 jitter: mean of |v[i] - v[i-1]|. */
    const double v[4] = { 10.0, 20.0, 15.0, 25.0 };
    ASSERT_DBL(stats_jitter(v, 4), (10.0 + 5.0 + 10.0) / 3.0, 1e-12);
    ASSERT_DBL(stats_jitter(v, 2), 10.0, 0.0);
}

TEST(stats_loss_percent_edge_cases)
{
    /* No probes sent is not 100% loss and not a division by zero. */
    ASSERT_DBL(stats_loss_percent(0, 0), 0.0, 0.0);
    ASSERT_DBL(stats_loss_percent(0, 10), 100.0, 0.0);
    ASSERT_DBL(stats_loss_percent(10, 10), 0.0, 0.0);
    ASSERT_DBL(stats_loss_percent(9, 10), 10.0, 1e-12);
    ASSERT_DBL(stats_loss_percent(1, 3), 66.66666666666667, 1e-9);

    /* A counter that over-reports is a counting bug upstream, not negative
     * loss. The documented answer is 0, and the grade table agrees. */
    ASSERT_DBL(stats_loss_percent(11, 10), 0.0, 0.0);
    ASSERT(stats_grade_loss(stats_loss_percent(11, 10)) == 'A');
}

TEST(stats_every_function_is_finite_on_an_all_nan_array)
{
    /* A stalled socket can hand back nothing but NaN. Nothing below may be NaN
     * or infinite, because every one of these values is multiplied into a
     * column width. Exact 0 is the contract; ASSERT_DBL with tolerance 0
     * rejects NaN and any stray value at the same time. */
    const double bad[3] = { NAN, NAN, NAN };

    ASSERT_DBL(stats_mean(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_median(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_percentile(bad, 3, 0.95), 0.0, 0.0);
    ASSERT_DBL(stats_min(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_max(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_stddev(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_jitter(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_cv_percent(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_trimmed_mean(bad, 3), 0.0, 0.0);
    ASSERT_DBL(stats_bufferbloat(bad, 3, 10.0), 0.0, 0.0);
}

TEST(stats_nan_samples_are_dropped_not_propagated)
{
    /* A dropped probe must not shift every rank: with {1, NaN, 3, NaN, 5} the
     * three survivors are the data, so min is 1 and max is 5. If NaN were
     * carried through, max/min become NaN and the axis top label is garbage. */
    const double v[5] = { 1.0, NAN, 3.0, NAN, 5.0 };

    ASSERT_DBL(stats_min(v, 5), 1.0, 0.0);
    ASSERT_DBL(stats_max(v, 5), 5.0, 0.0);
    ASSERT_DBL(stats_mean(v, 5), 3.0, 1e-12);
    ASSERT_DBL(stats_median(v, 5), 3.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 5, 0.0), 1.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 5, 1.0), 5.0, 0.0);
    ASSERT_DBL(stats_stddev(v, 5), 1.632993161855452, 1e-12);
    ASSERT_DBL(stats_cv_percent(v, 5), 54.43310539518178, 1e-10);
    ASSERT_DBL(stats_trimmed_mean(v, 5), 3.0, 1e-12);

    /* Bufferbloat shifts the survivors and takes their median: (3 - 1). */
    ASSERT_DBL(stats_bufferbloat(v, 5, 1.0), 2.0, 0.0);
}

TEST(stats_jitter_does_not_bridge_across_a_dropped_probe)
{
    /* The only finite adjacent pair here is 10 -> 40. Interpolating across the
     * NaNs would report 30 ms of jitter the network never showed, which is
     * worse than reporting nothing: the user would see a degraded call and
     * blame their own setup. Skipping the pair entirely gives 0. */
    const double v[5] = { NAN, 10.0, NAN, 40.0, NAN };
    ASSERT_DBL(stats_jitter(v, 5), 0.0, 0.0);
    ASSERT_DBL(stats_mean(v, 5), 25.0, 1e-12);
    ASSERT_DBL(stats_median(v, 5), 10.0, 0.0);
    ASSERT_DBL(stats_min(v, 5), 10.0, 0.0);
    ASSERT_DBL(stats_max(v, 5), 40.0, 0.0);
    ASSERT_DBL(stats_stddev(v, 5), 15.0, 1e-12);
    ASSERT_DBL(stats_cv_percent(v, 5), 60.0, 1e-10);
}

TEST(stats_empty_and_null_inputs_are_zero)
{
    const double empty[1] = { 1.0 };
    const double one[1] = { 7.0 };

    ASSERT_DBL(stats_mean(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_median(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_percentile(empty, 0, 0.5), 0.0, 0.0);
    ASSERT_DBL(stats_min(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_max(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_stddev(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_jitter(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_cv_percent(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_trimmed_mean(empty, 0), 0.0, 0.0);
    ASSERT_DBL(stats_bufferbloat(empty, 0, 10.0), 0.0, 0.0);

    /* NULL as well as n == 0: callers pass straight from their buffer, and a
     * NULL is a "no samples yet" state, not a programming error. */
    ASSERT_DBL(stats_mean(NULL, 0), 0.0, 0.0);
    ASSERT_DBL(stats_median(NULL, 4), 0.0, 0.0);
    ASSERT_DBL(stats_stddev(NULL, 4), 0.0, 0.0);
    ASSERT_DBL(stats_jitter(NULL, 4), 0.0, 0.0);
    ASSERT_DBL(stats_cv_percent(NULL, 4), 0.0, 0.0);
    ASSERT_DBL(stats_trimmed_mean(NULL, 4), 0.0, 0.0);
    ASSERT_DBL(stats_bufferbloat(NULL, 4, 10.0), 0.0, 0.0);

    /* A single sample is its own min, max, mean and median. */
    ASSERT_DBL(stats_min(one, 1), 7.0, 0.0);
    ASSERT_DBL(stats_max(one, 1), 7.0, 0.0);
    ASSERT_DBL(stats_mean(one, 1), 7.0, 0.0);
    ASSERT_DBL(stats_median(one, 1), 7.0, 0.0);
    ASSERT_DBL(stats_stddev(one, 1), 0.0, 0.0);
    ASSERT_DBL(stats_cv_percent(one, 1), 0.0, 0.0);
    ASSERT_DBL(stats_trimmed_mean(one, 1), 7.0, 0.0);
}

TEST(stats_bufferbloat_subtracts_the_idle_median)
{
    const double loaded[5] = { 60.0, 40.0, 50.0, 70.0, 80.0 };

    ASSERT_DBL(stats_bufferbloat(loaded, 5, 20.0), 30.0, 0.0); /* median 50 - 20 */
    ASSERT_DBL(stats_bufferbloat(loaded, 5, 50.0), 0.0, 0.0);  /* loaded == idle */

    /* No idle measurement means no answer, not a guess. */
    ASSERT_DBL(stats_bufferbloat(loaded, 5, 0.0), 0.0, 0.0);
    ASSERT_DBL(stats_bufferbloat(loaded, 5, -1.0), 0.0, 0.0);
    ASSERT_DBL(stats_bufferbloat(loaded, 5, NAN), 0.0, 0.0);
}

TEST(stats_grades_follow_the_documented_thresholds)
{
    /* Boundaries are tested on both sides: an off-by-one in the ladder turns a
     * clean 'A' into a 'B' and the user goes hunting for a fault they do not
     * have. */
    ASSERT(stats_grade_bufferbloat(0.0)  == 'A');
    ASSERT(stats_grade_bufferbloat(5.0)  == 'A');
    ASSERT(stats_grade_bufferbloat(5.01) == 'B');
    ASSERT(stats_grade_bufferbloat(30.0) == 'B');
    ASSERT(stats_grade_bufferbloat(30.01)== 'C');
    ASSERT(stats_grade_bufferbloat(50.0) == 'C');
    ASSERT(stats_grade_bufferbloat(50.01)== 'D');
    ASSERT(stats_grade_bufferbloat(100.0)== 'D');
    ASSERT(stats_grade_bufferbloat(100.01)=='E');
    ASSERT(stats_grade_bufferbloat(200.0)== 'E');
    ASSERT(stats_grade_bufferbloat(200.01)=='F');

    ASSERT(stats_grade_stability(0.0)  == 'A');
    ASSERT(stats_grade_stability(2.0)  == 'A');
    ASSERT(stats_grade_stability(2.01) == 'B');
    ASSERT(stats_grade_stability(4.0)  == 'B');
    ASSERT(stats_grade_stability(4.01) == 'C');
    ASSERT(stats_grade_stability(8.0)  == 'C');
    ASSERT(stats_grade_stability(15.0) == 'D');
    ASSERT(stats_grade_stability(25.0) == 'E');
    ASSERT(stats_grade_stability(25.01)==' F' - 1); /* 'F', spelled the long way */

    ASSERT(stats_grade_loss(0.0)  == 'A');
    ASSERT(stats_grade_loss(0.01) == 'B');
    ASSERT(stats_grade_loss(0.5)  == 'B');
    ASSERT(stats_grade_loss(0.51) == 'C');
    ASSERT(stats_grade_loss(1.0)  == 'C');
    ASSERT(stats_grade_loss(3.0)  == 'D');
    ASSERT(stats_grade_loss(8.0)  == 'E');
    ASSERT(stats_grade_loss(8.01) == 'F');

    /* A NaN fails every comparison and falls through to the worst grade, which
     * is the honest reading of a measurement that did not happen. */
    ASSERT(stats_grade_bufferbloat(NAN) == 'F');
    ASSERT(stats_grade_stability(NAN)   == 'F');
    ASSERT(stats_grade_loss(NAN)        == 'F');
}

TEST(stats_trimmed_mean_resists_a_single_outlier)
{
    /* The property the function exists for: one stalled transfer must not drag
     * the average. Plain mean sees 19.9; the trimmed mean is still 10.0. A
     * regression to stats_mean() would pass every other test in this file and
     * be invisible here, which is exactly why this case is pinned. */
    double v[100];
    for (int i = 0; i < 99; i++) v[i] = 10.0;
    v[99] = 1000.0;

    ASSERT_DBL(stats_mean(v, 100), 19.9, 1e-12);
    ASSERT_DBL(stats_trimmed_mean(v, 100), 10.0, 1e-12);

    /* The untailored case: 1..100 trims to the mean of 5..95, which is 50. */
    double ramp[100];
    ramp_1_to_100(ramp);
    ASSERT_DBL(stats_trimmed_mean(ramp, 100), 50.0, 1e-12);

    /* Below the 8-sample floor it must agree with the plain mean exactly,
     * otherwise the average a user sees for a short run disagrees with every
     * other average in the app. */
    const double seven[7] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 };
    ASSERT_DBL(stats_trimmed_mean(seven, 7), 4.0, 1e-12);
    ASSERT_DBL(stats_trimmed_mean(seven, 7), stats_mean(seven, 7), 0.0);
}

TEST(stats_large_sample_count_is_handled)
{
    /* 1000 samples pushes stats.c past its 256-entry stack scratch onto the
     * heap. Nothing here checks a hardcoded number beyond finiteness and
     * monotonicity, because the point is that the path runs and stays sane at
     * a size a real run actually reaches. */
    double *v = (double *)malloc(1000 * sizeof *v);
    double sorted[1000];

    if (v == NULL) { FAIL_MSG("out of memory"); return; }
    for (int i = 0; i < 1000; i++) v[i] = (double)(i * i % 997) + 1.0;
    memcpy(sorted, v, sizeof sorted);

    double mean = stats_mean(v, 1000);
    ASSERT_DBL(mean, stats_mean(sorted, 1000), 1e-9);

    double prev = stats_percentile(v, 1000, 0.0);
    for (int p = 5; p <= 100; p += 5) {
        double cur = stats_percentile(v, 1000, (double)p / 100.0);
        ASSERT(cur >= prev); /* a percentile ladder that goes backwards is wrong */
        ASSERT(cur <= stats_max(v, 1000));
        prev = cur;
    }
    ASSERT_DBL(stats_max(v, 1000), 997.0, 0.0);
    ASSERT_DBL(stats_stddev(v, 1000) >= 0.0 ? 1.0 : 0.0, 1.0, 0.0);
    ASSERT(stats_trimmed_mean(v, 1000) >= 1.0);

    free(v);
}

/* ========================================================================= chart */

#define CHART_SAMPLES 1000

static void chart_ramp(double *out, size_t n, double lo, double hi)
{
    for (size_t i = 0; i < n; i++)
        out[i] = lo + (hi - lo) * (n > 1 ? (double)i / (double)(n - 1) : 0.0);
}

TEST(chart_scale_fills_the_requested_column_count)
{
    double samples[CHART_SAMPLES];
    ChartScale cs;

    chart_ramp(samples, CHART_SAMPLES, 0.0, 940.0);
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, CHART_SAMPLES, 80, &cs);

    /* Exactly as many bars as columns asked for: one short and the trace stops
     * short of the right edge, one long and the renderer walks past h[]. */
    ASSERT(cs.bars == 80);
    ASSERT(cs.bars <= CHART_MAX_BARS);
    ASSERT_DBL(cs.peak, 940.0, 1e-9);
    ASSERT(cs.zeroed == 0);
    ASSERT(cs.mean > 0.0);
    ASSERT(cs.mean <= cs.peak);
    ASSERT(cs.p95 >= 0.0);
    ASSERT(cs.p95 <= cs.peak); /* a p95 above the peak would mean the axis lies */

    for (int i = 0; i < cs.bars; i++) {
        ASSERT_DBL(cs.h[i], cs.h[i], 0.0); /* finite: NaN != NaN fails here */
        ASSERT(cs.h[i] >= 0.0f);
        ASSERT(cs.h[i] <= 1.0f);
    }
    /* The ramp ends on its peak, so the final column is the tallest and must
     * reach the normalised top exactly. */
    ASSERT_DBL(cs.h[cs.bars - 1], 1.0, 1e-6);
    ASSERT(cs.h[0] < 1.0f);
}

TEST(chart_scale_of_an_all_zero_array_is_all_zero_not_nan)
{
    double samples[200];
    ChartScale cs;

    for (size_t i = 0; i < 200; i++) samples[i] = 0.0;
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 80, &cs);

    /* Zeroed is the caller's signal to draw a baseline without a top label.
     * The tempting mistake is normalising by a peak of 0, which gives 0/0 --
     * NaN -- and NaN heights render as an axis of garbage or a crash in the
     * glyph loop. Exact 0 with tolerance 0 rejects NaN here. */
    ASSERT(cs.zeroed == 1);
    ASSERT(cs.bars == 80);
    ASSERT_DBL(cs.peak, 0.0, 0.0);
    ASSERT_DBL(cs.mean, 0.0, 0.0);
    ASSERT_DBL(cs.p95, 0.0, 0.0);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], 0.0, 0.0);

    /* All-negative counts the same way: nothing was measured, not "negative". */
    for (size_t i = 0; i < 200; i++) samples[i] = -5.0;
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 80, &cs);
    ASSERT(cs.zeroed == 1);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], 0.0, 0.0);
}

TEST(chart_scale_clamps_a_width_beyond_the_bar_array)
{
    double samples[CHART_SAMPLES];
    ChartScale cs;

    chart_ramp(samples, CHART_SAMPLES, 0.0, 100.0);

    /* 2000 > CHART_MAX_BARS. An unclamped write here runs off the end of the
     * 2 KB h[] array inside the struct -- heap corruption, not a failed
     * assertion -- so this is the test that has to exist. A canary field after
     * h[] catches it too: peak/mean/p95 must still be their own values. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, CHART_SAMPLES, 2000, &cs);
    ASSERT(cs.bars == CHART_MAX_BARS);
    ASSERT(cs.bars <= CHART_MAX_BARS);
    ASSERT_DBL(cs.peak, 100.0, 1e-9);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], cs.h[i], 0.0);

    /* Far past the limit, to catch an off-by-one in the comparison. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, CHART_SAMPLES, 1000000, &cs);
    ASSERT(cs.bars == CHART_MAX_BARS);

    /* Fewer samples than columns: one column per sample, never one per
     * requested column, because that would index past the caller's array. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 10, 2000, &cs);
    ASSERT(cs.bars == 10);
    ASSERT(cs.bars <= CHART_MAX_BARS);
}

TEST(chart_scale_of_no_samples_does_not_crash)
{
    double samples[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ChartScale cs;

    /* bars == 0 is the "nothing to draw" signal, so h[] must be left alone
     * rather than filled. The sentinel proves the early return ran before any
     * write, which is what stops a zero-length call from reading past the
     * caller's (possibly NULL) buffer. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(NULL, 0, 80, &cs);
    ASSERT(cs.bars == 0);
    ASSERT(cs.peak == 0.0);
    ASSERT(cs.zeroed == 0);

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 0, 80, &cs);
    ASSERT(cs.bars == 0);

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 8, 0, &cs);
    ASSERT(cs.bars == 0);

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 8, -5, &cs);
    ASSERT(cs.bars == 0);

    /* A NULL destination is a no-op, not a fault: the renderer checks its own
     * state before deciding whether to draw. */
    chart_scale(samples, 8, 80, NULL);
}

TEST(chart_scale_buckets_keep_the_peak_not_the_mean)
{
    /* A speed trace exists to show the spike. Averaging it away is the one
     * thing it must not do, so one tall sample has to own its whole column.
     * With mean-bucketing this column would read 0.19 instead of 1.0 and a
     * stutter would be invisible -- which is the bug this pins. */
    double samples[200];
    ChartScale cs;

    for (int i = 0; i < 199; i++) samples[i] = 10.0;
    samples[100] = 100.0; /* bucket 10 spans [100, 110) */

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 20, &cs);

    ASSERT(cs.bars == 20);
    ASSERT_DBL(cs.peak, 100.0, 1e-9);
    ASSERT_DBL(cs.h[10], 1.0, 1e-6);  /* the spike fills its column */
    ASSERT_DBL(cs.h[0], 0.1, 1e-6);   /* the rest sit at 10/100 */
    ASSERT_DBL(cs.mean, 10.45, 1e-9); /* 2090/200 */

    /* Every sample lands in exactly one column and no column is left empty, so
     * the widths have to add up to n. Uneven bucketing shows up as a drifting
     * or doubled spike after a few hundred samples. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 7, &cs); /* 200/7 does not divide evenly */
    ASSERT(cs.bars == 7);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], cs.h[i], 0.0);
    ASSERT_DBL(cs.h[cs.bars - 1], 1.0, 1e-6);
}

TEST(chart_scale_of_a_nan_contaminated_trace_is_still_finite)
{
    /* One failed transfer yields NaN samples. They must not reach h[], or the
     * bar glyph loop multiplies NaN by a row count and indexes outside the
     * row buffer. Both bucketing paths are exercised here: n > bars and
     * n <= bars, which take different code. */
    double samples[64];
    ChartScale cs;

    for (int i = 0; i < 64; i++) samples[i] = (i % 4 == 0) ? NAN : 40.0;

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 64, 16, &cs); /* bucket path */
    ASSERT(cs.bars == 16);
    ASSERT_DBL(cs.peak, 40.0, 1e-9);
    ASSERT_DBL(cs.mean, 40.0, 1e-9);
    ASSERT(cs.p95 >= 0.0);
    ASSERT(cs.p95 <= cs.peak);
    for (int i = 0; i < cs.bars; i++) {
        ASSERT_DBL(cs.h[i], cs.h[i], 0.0);
        ASSERT(cs.h[i] >= 0.0f && cs.h[i] <= 1.0f);
    }

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 4, 80, &cs); /* one-sample-per-column path */
    ASSERT(cs.bars == 4);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], cs.h[i], 0.0);

    /* An infinity must not become an infinite peak, which would normalise
     * every real bar to zero height. */
    double inf[4] = { 1.0, INFINITY, 2.0, 3.0 };
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(inf, 4, 4, &cs);
    ASSERT_DBL(cs.peak, 3.0, 0.0);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], cs.h[i], 0.0);
}

TEST(chart_nice_ceiling_rounds_up_to_a_readable_axis)
{
    /* Known values. An axis labelled 87.3 tells the reader nothing; 100 does. */
    ASSERT_DBL(chart_nice_ceiling(87.3), 100.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(101.0), 200.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(1.0), 1.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(9.99), 10.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(2.5), 5.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(0.03), 0.05, 1e-12);
    ASSERT_DBL(chart_nice_ceiling(0.5), 0.5, 1e-12);

    /* Never zero, whatever it is handed: a zero axis top makes the height
     * normalisation a division by zero, so every bar becomes NaN. */
    ASSERT(chart_nice_ceiling(0.0) != 0.0);
    ASSERT(chart_nice_ceiling(-1.0) != 0.0);
    ASSERT(chart_nice_ceiling(NAN) != 0.0);
    ASSERT_DBL(chart_nice_ceiling(0.0), 1.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(NAN), 1.0, 0.0);

    /* Decade boundaries are where log10 rounding bites: 100 must not come out
     * as 1000, which is the single most visible possible axis bug. */
    ASSERT_DBL(chart_nice_ceiling(100.0), 100.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(1000.0), 1000.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(0.1), 0.1, 1e-12);
    ASSERT_DBL(chart_nice_ceiling(1e-9), 1e-9, 1e-20);

    /* It must also be a genuine ceiling: never below the raw peak, or the
     * tallest bar overflows its own axis. */
    for (double raw = 0.5; raw < 5000.0; raw *= 1.37) {
        double nice = chart_nice_ceiling(raw);
        ASSERT(nice >= raw);
        ASSERT(nice / raw <= 2.0 + 1e-9);
        ASSERT(isfinite(nice));
    }
}

/* One guard buffer for every chart_ticks overflow case. The 0xAA fill is the
 * canary: if chart_ticks writes past cap, the byte after the terminator is no
 * longer 0xAA and the whole run fails loudly instead of corrupting a stack
 * frame that happens to be unused. */
#define TICK_GUARD 256

static int chart_ticks_well_formed(double top, size_t cap, int *count_out)
{
    unsigned char buf[TICK_GUARD];
    size_t len = 0;
    int count;

    memset(buf, 0xAA, sizeof buf);
    count = chart_ticks(top, (char *)buf, cap);
    if (count_out) *count_out = count;
    if (count < 0) return 0;

    if (cap == 0) {
        /* No room means no bytes may change at all. */
        for (size_t i = 0; i < TICK_GUARD; i++)
            if (buf[i] != 0xAA) { FAIL_MSG("cap=0 wrote out[%zu]", i); return 0; }
        return count == 0;
    }

    while (len < cap && buf[len] != 0x00) len++;
    if (len >= cap) { FAIL_MSG("chart_ticks(%.10g, cap=%zu) left no terminator", top, cap); return 0; }
    if (len != strlen((const char *)buf)) { FAIL_MSG("chart_ticks(%.10g, cap=%zu) not NUL-terminated", top, cap); return 0; }

    for (size_t i = cap; i < TICK_GUARD; i++) {
        if (buf[i] != 0xAA) { FAIL_MSG("chart_ticks(%.10g, cap=%zu) overran to out[%zu]", top, cap, i); return 0; }
    }
    return 1;
}

TEST(chart_ticks_never_overrun_or_split_a_label)
{
    /* The three caps from the brief plus the ordinary one. A clipped label is
     * worse than a short axis, so the contract is a NUL-terminated prefix of
     * whole labels -- which means the last byte written is always the
     * terminator and everything after cap is untouched. */
    static const size_t caps[] = { 0, 1, 2, 3, 8, 16, TICK_GUARD };
    static const double tops[] = { 0.0, 0.5, 1.0, 87.3, 100.0, 101.0, 1000.0, 1e6 };

    for (size_t t = 0; t < sizeof tops / sizeof tops[0]; t++) {
        for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
            ASSERT(chart_ticks_well_formed(tops[t], caps[c], NULL));
        }
    }
}

TEST(chart_ticks_survives_non_finite_and_absurd_tops)
{
    /* These labels are printed beside the bars, so a NaN or an infinity here is
     * the same terminal-corrupting bug as one coming out of fmt. */
    ASSERT(chart_ticks_well_formed(NAN, TICK_GUARD, NULL));
    ASSERT(chart_ticks_well_formed(INFINITY, TICK_GUARD, NULL));
    ASSERT(chart_ticks_well_formed(-INFINITY, TICK_GUARD, NULL));
    ASSERT(chart_ticks_well_formed(DBL_MAX, TICK_GUARD, NULL));
    ASSERT(chart_ticks_well_formed(1e300, TICK_GUARD, NULL));
    ASSERT(chart_ticks_well_formed(1e-300, TICK_GUARD, NULL));

    char buf[TICK_GUARD];
    memset(buf, 0, sizeof buf);
    ASSERT(chart_ticks(NAN, buf, sizeof buf) >= 1);
    assert_no_inf(buf);

    memset(buf, 0, sizeof buf);
    ASSERT(chart_ticks(INFINITY, buf, sizeof buf) >= 1);
    assert_no_inf(buf);

    /* No room at all is a valid call, not a fault. */
    ASSERT(chart_ticks(100.0, NULL, 0) == 0);
    ASSERT(chart_ticks(100.0, buf, 0) == 0);
}

TEST(chart_ticks_produce_a_usable_numeric_axis)
{
    char buf[TICK_GUARD];
    int count;

    /* Known output for the canonical 0..100 axis. Step is chosen at or above
     * top/5, so 20 is the finest nice step that still fits in six labels; if
     * this comes out as "0\n25\n50\n75\n100" the ladder moved and every axis
     * on screen moves with it. */
    memset(buf, 0, sizeof buf);
    count = chart_ticks(100.0, buf, sizeof buf);
    ASSERT_STR(buf, "0\n20\n40\n60\n80\n100");
    ASSERT(count == 6);

    /* Structural properties for every reasonable top: strictly ascending,
     * starting at zero, four to six labels, every label a complete number. A
     * clipped or duplicate label is what a short buffer produces if the
     * push-if-it-fits rule is removed. */
    static const double tops[] = { 0.0, 0.25, 1.0, 5.0, 87.3, 100.0, 101.0, 250.0, 1e6 };
    for (size_t i = 0; i < sizeof tops / sizeof tops[0]; i++) {
        char *line = buf;
        double prev = -1.0;
        int labels = 0;

        memset(buf, 0, sizeof buf);
        ASSERT(chart_ticks_well_formed(tops[i], TICK_GUARD, &count));
        ASSERT(count >= 4 && count <= 6);

        while (line != NULL && *line != '\0') {
            char *end = NULL;
            double v = strtod(line, &end);

            if (end == line || *end != '\0') { FAIL_MSG("tick label is not a complete number: \"%s\"", line); break; }
            if (!isfinite(v)) { FAIL_MSG("tick label is not finite: \"%s\"", line); break; }
            if (v < prev) { FAIL_MSG("tick labels descend at %.10g: %.10g after %.10g", tops[i], v, prev); break; }
            if (labels == 0 && v != 0.0) { FAIL_MSG("axis does not start at 0 for %.10g", tops[i]); break; }
            prev = v;
            labels++;
            line = strchr(line, '\n');
            if (line) line++;
        }
        ASSERT(labels == count);
    }
}

/* =========================================================================== fmt */

/* The five inputs from the brief, plus the two doubles that are hardest to
 * render without overflowing the fixed-shape formats. DBL_MAX is here because
 * "%.1f" of it is a 310-character string: it must clip, not corrupt. */
static const double k_hostile[] = {
    NAN, INFINITY, -INFINITY, -1.0, 0.0, DBL_MAX, -DBL_MAX
};
#define N_HOSTILE (sizeof k_hostile / sizeof k_hostile[0])

TEST(fmt_no_formatter_ever_prints_nan_or_inf)
{
    /* The headline test. Every double-taking formatter, every hostile value.
     * A single "nan" or "inf" reaching a terminal is a visible corruption of
     * the user's screen and a silent lie about the measurement, and the only
     * defence is that it is impossible to produce one. */
    for (size_t i = 0; i < N_HOSTILE; i++) {
        const double v = k_hostile[i];
        char buf[64];

        buf[0] = '\0';
        fmt_rate(buf, sizeof buf, v);    assert_no_inf(buf);
        buf[0] = '\0';
        fmt_ms(buf, sizeof buf, v);      assert_no_inf(buf);
        buf[0] = '\0';
        fmt_pct(buf, sizeof buf, v);     assert_no_inf(buf);
        buf[0] = '\0';
        fmt_duration(buf, sizeof buf, v); assert_no_inf(buf);

        /* The truncated forms are what actually gets drawn, so they need the
         * same guarantee: an ellipsis is appended to an already-formed label. */
        snprintf(buf, sizeof buf, "%.1f%s", 1.0, "");
        fmt_truncate(buf, 8, buf, 3);
        assert_no_inf(buf);
    }
}

TEST(fmt_non_finite_doubles_render_as_the_missing_placeholder)
{
    char buf[64];

    /* "--" rather than "0": a missing sample that reads as a real zero is the
     * one formatting bug nobody notices, because the number looks plausible. */
    static const double bad[] = { NAN, INFINITY, -INFINITY, -1.0 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const double v = bad[i];

        memset(buf, 0, sizeof buf);
        fmt_rate(buf, sizeof buf, v); ASSERT_STR(buf, "--");
        memset(buf, 0, sizeof buf);
        fmt_ms(buf, sizeof buf, v);   ASSERT_STR(buf, "--");
        memset(buf, 0, sizeof buf);
        fmt_pct(buf, sizeof buf, v);  ASSERT_STR(buf, "--");
        memset(buf, 0, sizeof buf);
        fmt_duration(buf, sizeof buf, v); ASSERT_STR(buf, "--");
        memset(buf, 0, sizeof buf);
        fmt_stamp(buf, sizeof buf, (int64_t)(INFINITY)); ASSERT_STR(buf, "--");
    }

    /* Zero is a real measurement, not a missing one. */
    memset(buf, 0, sizeof buf);
    fmt_rate(buf, sizeof buf, 0.0); ASSERT_STR(buf, "0.0");
    memset(buf, 0, sizeof buf);
    fmt_ms(buf, sizeof buf, 0.0);   ASSERT_STR(buf, "0");
    memset(buf, 0, sizeof buf);
    fmt_pct(buf, sizeof buf, 0.0);  ASSERT_STR(buf, "0.0");
    memset(buf, 0, sizeof buf);
    fmt_duration(buf, sizeof buf, 0.0); ASSERT_STR(buf, "0s");

    /* fmt_pct keeps a negative sign: it is information about the measurement,
     * and hiding it would be a formatting choice dressed up as a fact. */
    memset(buf, 0, sizeof buf);
    fmt_pct(buf, sizeof buf, -2.5); ASSERT_STR(buf, "-2.5");
}

TEST(fmt_rate_keeps_one_fixed_shape)
{
    char buf[64];

    /* One shape for the whole range. The live tab used to drop the decimal
     * above 100 and switch to Gbps above 1000, so the same sample read "111
     * Mbps" on one screen and "110.5" on another. */
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 110.5);  ASSERT_STR(buf, "110.5");
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 0.0);     ASSERT_STR(buf, "0.0");
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 1.25);    ASSERT_STR(buf, "1.2");
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 1.26);    ASSERT_STR(buf, "1.3");
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 1000.0);  ASSERT_STR(buf, "1000.0");
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 12345.6); ASSERT_STR(buf, "12345.6");
}

TEST(fmt_ms_drops_the_decimal_on_whole_numbers_and_past_9999)
{
    char buf[64];

    /* A trailing zero costs a column and says nothing the unit does not. The
     * whole-number shapes ("9.4", "12", "47.3", "812") are what every column
     * in the app is aligned against. */
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 9.4);   ASSERT_STR(buf, "9.4");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 12.0);  ASSERT_STR(buf, "12");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 47.3);  ASSERT_STR(buf, "47.3");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 812.0); ASSERT_STR(buf, "812");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 9.0);   ASSERT_STR(buf, "9");

    /* Past this the figure stops being a latency and becomes a column width. */
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 9999.0);  ASSERT_STR(buf, "9999");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 10000.0); ASSERT_STR(buf, "10000");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 12000.0); ASSERT_STR(buf, "12000");
}

TEST(fmt_bytes_picks_the_right_binary_unit)
{
    /* Known values across every rung, including the documented examples. The
     * KB/MB/GB spelling with 1024-based divisors is deliberate, so the exact
     * strings are pinned rather than the magnitudes. */
    struct { uint64_t bytes; const char *want; } cases[] = {
        { 0,          "0 B"    },
        { 1,          "1 B"    },
        { 512,        "512 B"  },
        { 1023,       "1023 B" },
        { 1024,       "1 KB"   },
        { 812ull * 1024ull, "812 KB"  },
        { 5242880ull, "5.0 MB" },
        { 5452596ull, "5.2 MB" },
        { 1073741824ull, "1.0 GB" },
        { 1503238554ull, "1.4 GB" },
    };
    char buf[64];

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(buf, 0, sizeof buf);
        fmt_bytes(buf, sizeof buf, cases[i].bytes);
        if (strcmp(buf, cases[i].want) != 0)
            FAIL_MSG("fmt_bytes(%llu) = \"%s\", want \"%s\"",
                     (unsigned long long)cases[i].bytes, buf, cases[i].want);
        ASSERT_STR(buf, cases[i].want);
        assert_no_inf(buf);
    }

    /* The largest count must not round into the wrong bucket. 2^64-1 bytes is
     * 16 EiB, comfortably inside the GB rung, and an overflowing shift is the
     * classic way it would not be. */
    memset(buf, 0, sizeof buf);
    fmt_bytes(buf, sizeof buf, UINT64_MAX);
    ASSERT(strchr(buf, 'G') != NULL || strchr(buf, 'T') != NULL);
    assert_no_inf(buf);
}

TEST(fmt_stamp_rejects_times_it_cannot_print)
{
    char buf[64];

    /* Zero is the epoch, and printing 1970 for it would look like a real run
     * from 1970 rather than a run that never recorded a timestamp. */
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, 0);         ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, -1);        ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, INT64_MIN);  ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, INT64_MAX);  ASSERT_STR(buf, "--");

    /* Past 9999 the printed shape no longer holds, so it is unprintable
     * rather than guessed at. */
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, INT64_C(253402300800000));
    ASSERT_STR(buf, "--");

    /* A real timestamp: assert the shape, not the value. The value depends on
     * the machine's timezone, and a test that asserts one locks CI to whatever
     * zone the runner happens to have. The separators are the contract. */
    memset(buf, 0, sizeof buf);
    fmt_stamp(buf, sizeof buf, INT64_C(1791676800000)); /* 2026-10-12T00:00:00Z */
    ASSERT(strlen(buf) == 16);
    if (strlen(buf) == 16) {
        ASSERT(buf[4]  == '-');
        ASSERT(buf[7]  == '-');
        ASSERT(buf[10] == ' ');
        ASSERT(buf[13] == ':');
    }
    assert_no_inf(buf);
}

TEST(fmt_duration_drops_units_as_it_grows)
{
    char buf[64];

    /* Truncated like a stopwatch: 59.9 s of uptime is 59 seconds, because
     * rounding up would print a minute that has not happened yet. */
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 0.0);    ASSERT_STR(buf, "0s");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 38.0);   ASSERT_STR(buf, "38s");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 59.9);   ASSERT_STR(buf, "59s");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 60.0);   ASSERT_STR(buf, "1m 00s");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 251.0);  ASSERT_STR(buf, "4m 11s");

    /* Seconds drop once hours appear: past an hour the reader is asking how
     * long the run took, not what it is doing this second. */
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 3599.0); ASSERT_STR(buf, "59m 59s");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 3600.0); ASSERT_STR(buf, "1h 00m");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 3720.0); ASSERT_STR(buf, "1h 02m");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 86399.0); ASSERT_STR(buf, "23h 59m");

    /* A negative elapsed time is clock noise, not a duration worth a minus. */
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, -5.0); ASSERT_STR(buf, "0s");
    /* Clamped rather than converted: the narrowing cast out of double range
     * is undefined behaviour, not a wrap. */
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 1e300); ASSERT_STR(buf, "11410h 10m");
}

TEST(fmt_truncate_handles_degenerate_caps_and_widths)
{
    char buf[64];

    /* Short strings are copied verbatim; an exact fit does not gain an
     * ellipsis, or every full-width cell in the table ends in one. */
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, "ab", 8);    ASSERT_STR(buf, "ab");
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, "abcdef", 6); ASSERT_STR(buf, "abcdef");
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, "abcdef", 5);
    ASSERT_STR(buf, "abcdef\x65");  /* exact fit: no marker */

    /* Far longer than the width: U+2026 is one column and three bytes, so
     * "abcdefghij" at width 5 keeps four characters and appends it. */
    memset(buf, 0, sizeof buf);
    fmt_truncate(buf, sizeof buf, "abcdefghij", 5);
    ASSERT_STR(buf, "abcd\xe2\x80\xa6");
    ASSERT(strlen(buf) == 7);

    /* w == 0 asks for nothing. */
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, "abcdef", 0);
    ASSERT_STR(buf, "");
    /* NULL is a field that was never filled in, not a crash. */
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, NULL, 8);
    ASSERT_STR(buf, "");

    /* cap == 1 holds only the terminator, and must not write the second byte.
     * This is the smallest cap that can silently corrupt the stack if the
     * room arithmetic is off by one. */
    memset(buf, 0xAA, sizeof buf); fmt_truncate(buf, 1, "abcdef", 5);
    ASSERT(buf[0] == '\0');
    ASSERT(buf[1] == (char)0xAA);

    /* Not even the marker fits: giving up a column beats emitting half a
     * glyph, which renders as a replacement character. */
    memset(buf, 0, sizeof buf); fmt_truncate(buf, 3, "abcdef", 5);
    ASSERT_STR(buf, "");
    memset(buf, 0, sizeof buf); fmt_truncate(buf, 2, "abcdef", 5);
    ASSERT_STR(buf, "");
}

TEST(fmt_truncate_never_splits_a_multibyte_glyph)
{
    char buf[64];

    /* Two U+00E9: bytes C3 A9 C3 A9. Width 2 leaves room for one column of
     * content, which lands on byte 1 -- a continuation byte. Copying it would
     * put half a character on screen, which is a visible tofu box, so the
     * content gives up its column and only the marker is emitted. */
    memset(buf, 0, sizeof buf);
    fmt_truncate(buf, sizeof buf, "\xc3\xa9\xc3\xa9", 2);
    ASSERT_STR(buf, "\xe2\x80\xa6");
    ASSERT(strlen(buf) == 3);

    /* A truncation that lands on a lead byte keeps it whole. */
    memset(buf, 0, sizeof buf);
    fmt_truncate(buf, sizeof buf, "\xc3\xa9\xc3\xa9\xc3\xa9", 3);
    ASSERT_STR(buf, "\xc3\xa9\xe2\x80\xa6");

    /* And the cap must clamp the content, not the ellipsis. */
    memset(buf, 0, sizeof buf);
    fmt_truncate(buf, 5, "abcdefghijklmnop", 12);
    ASSERT(strlen(buf) <= 4);
    ASSERT(buf[0] == 'a');
}

TEST(fmt_never_writes_when_cap_is_zero)
{
    /* Every formatter takes a caller-sized buffer and a byte cap, and a layout
     * that measures a column as zero is a real case (a minimised window, a
     * hidden pane). cap == 0 must mean "touch nothing", not "write one byte
     * anyway" -- which would be a one-byte overrun past the end of whatever
     * the caller allocated. */
    unsigned char buf[32];
    size_t i;

    memset(buf, 0xAA, sizeof buf);
    fmt_rate((char *)buf, 0, 1.0);
    fmt_ms((char *)buf, 0, 1.0);
    fmt_pct((char *)buf, 0, 1.0);
    fmt_bytes((char *)buf, 0, 1);
    fmt_stamp((char *)buf, 0, 1);
    fmt_duration((char *)buf, 0, 1.0);
    fmt_truncate((char *)buf, 0, "x", 4);

    for (i = 0; i < sizeof buf; i++) {
        if (buf[i] != 0xAA) { FAIL_MSG("a formatter wrote out[%zu] with cap=0", i); return; }
    }

    /* A NULL destination is a no-op rather than a fault. */
    fmt_rate(NULL, 8, 1.0);
    fmt_ms(NULL, 8, 1.0);
    fmt_pct(NULL, 8, 1.0);
    fmt_bytes(NULL, 8, 1);
    fmt_stamp(NULL, 8, 1);
    fmt_duration(NULL, 8, 1.0);
    fmt_truncate(NULL, 8, "x", 4);
}

TEST(fmt_always_terminates_within_cap)
{
    /* A one-byte-short buffer is the normal case in a packed table layout: the
     * caller sizes the buffer to the column and the string has to fit. Each
     * formatter gets a canary buffer one byte larger than the cap it is
     * given; if anything writes past cap, the canary byte changes. */
    struct { const char *name; size_t need; } shapes[] = {
        { "rate",     0 }, { "ms",   0 }, { "pct",  0 },
        { "bytes",    0 }, { "stamp", 0 }, { "duration", 0 }, { "truncate", 0 },
    };
    (void)shapes;

    for (size_t cap = 1; cap <= 8; cap++) {
        unsigned char buf[32];
        char *p = (char *)buf;

        memset(buf, 0xAA, sizeof buf);
        fmt_rate(p, cap, 12345.6);
        fmt_ms(p, cap, 12345.6);
        fmt_pct(p, cap, 12345.6);
        fmt_bytes(p, cap, 5452596ull);
        fmt_stamp(p, cap, INT64_C(1791676800000));
        fmt_duration(p, cap, 3720.0);
        fmt_truncate(p, cap, "a fairly long label that will not fit", 20);

        ASSERT(strlen(p) < cap);
        assert_no_inf(p);
        for (size_t i = cap; i < sizeof buf; i++) {
            if (buf[i] != 0xAA) {
                FAIL_MSG("cap=%zu overran to out[%zu] (last write was \"%s\")", cap, i, p);
                break;
            }
        }
    }
}

/* ======================================================================= actions */

/* The one invariant that matters in this module: an out-of-range sel reaching
 * the renderer is an out-of-bounds read, and a sel that lands on a disabled
 * row means the user pressed enter and nothing happened. Checked after every
 * op, in every state, on every step of the fuzz. */
static int menu_invariant_holds(const ActionMenu *m, int running, int paused)
{
    if (m->sel < 0) return 0;
    if (m->count < 0 || m->count > ITEM_COUNT) return 0;

    for (int i = 0; i < m->count; i++)
        if (actions_enabled((MenuItem)m->items[i], running, paused) != 1) return 0;

    if (!m->open) return m->sel == 0; /* closed: nothing highlighted */
    if (m->count <= 0) return 0;       /* open and empty can never happen */
    if (m->sel >= m->count) return 0;
    return actions_enabled((MenuItem)m->items[m->sel], running, paused) == 1;
}

static void assert_menu_invariant(const ActionMenu *m, int running, int paused,
                                  const char *where)
{
    if (menu_invariant_holds(m, running, paused)) return;

    FAIL_MSG("%s: sel=%d open=%d count=%d running=%d paused=%d -- %s",
             where, m->sel, m->open, m->count, running, paused,
             (m->open && m->sel >= 0 && m->sel < m->count)
                 ? "highlight sits on a DISABLED item"
                 : "highlight out of range");
    ASSERT(menu_invariant_holds(m, running, paused));
}

/* The four engine states a menu can be built for. running && paused is the one
 * that matters most: it is the only state where the enabled set gains an item
 * at the front (Resume) and loses one (Pause), so it is the only state where a
 * stale highlight can be left pointing at the wrong row. */
static const int k_states[][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
#define N_STATES (sizeof k_states / sizeof k_states[0])

static const char *state_name(const int st[2])
{
    return st[1] ? (st[0] ? "running+paused" : "paused-not-running")
                 : (st[0] ? "running" : "idle");
}

TEST(actions_enabled_set_matches_the_state)
{
    /* Resume and Pause leave the list entirely rather than being greyed out,
     * and items[] keeps MenuItem order so the menu never reshuffles itself as
     * the engine moves. That ordering is what keeps a stored sel meaning the
     * same row across a state change. */
    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];
        ActionMenu m;

        actions_init(&m);
        actions_apply(&m, ACTMENU_OPEN, running, paused);

        int n = 0;
        for (int i = 0; i < ITEM_COUNT; i++)
            if (actions_enabled((MenuItem)i, running, paused)) n++;
        ASSERT(m.count == n);
        ASSERT(m.count >= 1);   /* the menu is never empty, so sel always fits */
        ASSERT(m.count <= ITEM_COUNT);
        ASSERT(m.count <= ACTIONS_MAX_ITEMS);

        int k = 0;
        for (int i = 0; i < ITEM_COUNT; i++) {
            if (!actions_enabled((MenuItem)i, running, paused)) continue;
            ASSERT(m.items[k] == i);
            k++;
        }
        /* Slots past count are poisoned, not left stale: a view that trusts the
         * array instead of the count gets an obviously invalid row rather than
         * an item the user already ruled out. */
        for (int i = m.count; i < ITEM_COUNT; i++) ASSERT(m.items[i] == ITEM_COUNT);

        if (running && paused)       ASSERT(m.items[0] == ITEM_RESUME);
        else if (running)            ASSERT(m.items[0] == ITEM_PAUSE);
        else                         ASSERT(m.items[0] == ITEM_RERUN);
    }

    /* The two state-dependent rows, on both sides of each boundary. */
    ASSERT(actions_enabled(ITEM_RESUME, 1, 1) == 1);
    ASSERT(actions_enabled(ITEM_RESUME, 1, 0) == 0);
    ASSERT(actions_enabled(ITEM_RESUME, 0, 1) == 0);
    ASSERT(actions_enabled(ITEM_PAUSE, 1, 0) == 1);
    ASSERT(actions_enabled(ITEM_PAUSE, 1, 1) == 0);
    ASSERT(actions_enabled(ITEM_PAUSE, 0, 0) == 0);

    /* Everything else is available in every state, which is what guarantees
     * the menu is never empty. */
    for (size_t s = 0; s < N_STATES; s++)
        for (int i = ITEM_RERUN; i < ITEM_COUNT; i++)
            ASSERT(actions_enabled((MenuItem)i, k_states[s][0], k_states[s][1]) == 1);

    /* ITEM_COUNT is not a MenuItem and must not be offered. */
    ASSERT(actions_enabled(ITEM_COUNT, 1, 0) == 0);
    ASSERT(actions_enabled((MenuItem)999, 1, 0) == 0);
}

TEST(actions_labels_are_never_null)
{
    /* The view dereferences these unconditionally. A NULL here is a crash in
     * the middle of a redraw, and the header promises it cannot happen. */
    for (int i = 0; i <= ITEM_COUNT; i++) {
        const char *label = actions_label((MenuItem)i);
        ASSERT(label != NULL);
        if (label) ASSERT(label[0] != '\0');
        ASSERT(strlen(label) < 64);
        ASSERT(contains_ci(label, "nan") == 0);
    }
    /* Distinct rows must be distinct strings or the user cannot tell them
     * apart in the menu. */
    for (int i = 0; i < ITEM_COUNT; i++)
        for (int j = i + 1; j < ITEM_COUNT; j++)
            ASSERT(strcmp(actions_label((MenuItem)i), actions_label((MenuItem)j)) != 0);
}

TEST(actions_every_op_in_every_state_keeps_the_invariant)
{
    /* The full cross product: 4 states x 5 ops, applied to a freshly opened
     * menu and to a closed one. Each case checks the invariant after the op,
     * the state of `open`, and the return value. */
    static const MenuOp ops[] = {
        ACTMENU_NONE, ACTMENU_OPEN, ACTMENU_NAVIGATE, ACTMENU_TOGGLE, ACTMENU_CANCEL
    };
    const char *op_names[] = { "NONE", "OPEN", "NAVIGATE", "TOGGLE", "CANCEL" };

    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];

        for (size_t o = 0; o < sizeof ops / sizeof ops[0]; o++) {
            ActionMenu open_menu, closed_menu;
            MenuItem got_open, got_closed;

            actions_init(&open_menu);
            actions_apply(&open_menu, ACTMENU_OPEN, running, paused);
            got_open = actions_apply(&open_menu, ops[o], running, paused);
            if (!menu_invariant_holds(&open_menu, running, paused))
                FAIL_MSG("%s + %s from open", state_name(k_states[s]), op_names[o]);
            ASSERT(menu_invariant_holds(&open_menu, running, paused));

            actions_init(&closed_menu);
            got_closed = actions_apply(&closed_menu, ops[o], running, paused);
            if (!menu_invariant_holds(&closed_menu, running, paused))
                FAIL_MSG("%s + %s from closed", state_name(k_states[s]), op_names[o]);
            ASSERT(menu_invariant_holds(&closed_menu, running, paused));

            switch (ops[o]) {
                case ACTMENU_NONE:
                    /* No key. The enabled set is still rebuilt: an open menu
                     * has to track an engine that moved underneath it. */
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(open_menu.open == 1);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(closed_menu.open == 0);
                    break;
                case ACTMENU_OPEN:
                    /* Opens, and starts at the top: reopening after a cancel
                     * should not resume a selection the user never made. */
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(open_menu.open == 1);
                    ASSERT(open_menu.sel == 0);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(closed_menu.open == 1);
                    ASSERT(closed_menu.sel == 0);
                    break;
                case ACTMENU_NAVIGATE:
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(open_menu.open == 1);
                    ASSERT(open_menu.sel == 1); /* sel was 0, count >= 7 */
                    /* A closed menu has nothing to move. */
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(closed_menu.sel == 0);
                    break;
                case ACTMENU_TOGGLE:
                    /* Choosing closes the menu either way; a keypress that
                     * chose nothing must not leave an overlay with no obvious
                     * way back. */
                    ASSERT(got_open == open_menu.items[0]);
                    ASSERT(open_menu.open == 0);
                    ASSERT(open_menu.sel == 0);
                    /* Nothing chosen on a closed menu. */
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(closed_menu.open == 0);
                    break;
                case ACTMENU_CANCEL:
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(open_menu.open == 0);
                    ASSERT(open_menu.sel == 0);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(closed_menu.open == 0);
                    break;
                case ACTMENU_NAVIGATE + 100: /* never taken; keeps -Wswitch quiet */
                    break;
            }
        }
    }
}

TEST(actions_toggle_on_a_closed_menu_chooses_nothing)
{
    /* The explicit case: enter on a closed menu must not fire whatever row the
     * struct happens to remember. Acting on a stale index here would restart a
     * run or quit the app with no menu on screen to explain it. */
    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];
        ActionMenu m;

        actions_init(&m);
        ASSERT(actions_apply(&m, ACTMENU_TOGGLE, running, paused) == ITEM_COUNT);
        ASSERT(m.open == 0);
        assert_menu_invariant(&m, running, paused, "TOGGLE closed");

        /* Also after a real selection and cancel: the remembered row must not
         * survive a close. */
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        (void)actions_apply(&m, ACTMENU_TOGGLE, running, paused);
        ASSERT(actions_apply(&m, ACTMENU_TOGGLE, running, paused) == ITEM_COUNT);

        actions_apply(&m, ACTMENU_OPEN, running, paused);
        actions_apply(&m, ACTMENU_NAVIGATE, running, paused);
        actions_apply(&m, ACTMENU_CANCEL, running, paused);
        ASSERT(m.sel == 0);
        ASSERT(actions_apply(&m, ACTMENU_TOGGLE, running, paused) == ITEM_COUNT);
    }

    /* A NULL menu is a caller that has not built one yet, not a crash. */
    ASSERT(actions_apply(NULL, ACTMENU_TOGGLE, 1, 0) == ITEM_COUNT);
    actions_init(NULL);
}

TEST(actions_navigate_wraps_and_never_lands_on_a_disabled_row)
{
    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];
        ActionMenu m;

        actions_init(&m);
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        ASSERT(m.sel == 0);

        /* Walk the whole list and off the end: a cursor stuck on the last row
         * reads as a dead key, so it wraps to 0. */
        for (int step = 1; step <= m.count; step++) {
            actions_apply(&m, ACTMENU_NAVIGATE, running, paused);
            ASSERT(m.sel == step % m.count);
            assert_menu_invariant(&m, running, paused, "after NAVIGATE");
        }
        ASSERT(m.sel == 0); /* wrapped exactly back to the top */

        /* Every row it can reach is enabled, and it reaches all of them. */
        unsigned char seen[ITEM_COUNT + 1];
        memset(seen, 0, sizeof seen);
        for (int step = 0; step < m.count; step++) {
            seen[m.sel] = 1;
            ASSERT(actions_enabled((MenuItem)m.items[m.sel], running, paused) == 1);
            actions_apply(&m, ACTMENU_NAVIGATE, running, paused);
        }
        for (int i = 0; i < m.count; i++) ASSERT(seen[i] == 1);
    }
}

TEST(actions_sel_survives_an_engine_state_change_under_an_open_menu)
{
    /* The real-world hazard: a run finishes or pauses while the menu is open,
     * so the enabled set changes underneath a stored sel. The set always
     * changes size (Pause leaves, Resume arrives), so a sel that is not
     * re-clamped on the next keypress indexes off the end of the new set. */
    for (size_t a = 0; a < N_STATES; a++) {
        for (size_t b = 0; b < N_STATES; b++) {
            ActionMenu m;

            actions_init(&m);
            actions_apply(&m, ACTMENU_OPEN, k_states[a][0], k_states[a][1]);
            for (int step = 0; step < m.count; step++) {
                /* Transition a -> b with no key in between, then look. */
                actions_apply(&m, ACTMENU_NONE, k_states[b][0], k_states[b][1]);
                if (!menu_invariant_holds(&m, k_states[b][0], k_states[b][1]))
                    FAIL_MSG("%s -> %s with sel=%d", state_name(k_states[a]),
                             state_name(k_states[b]), m.sel);
                ASSERT(menu_invariant_holds(&m, k_states[b][0], k_states[b][1]));
                actions_apply(&m, ACTMENU_NAVIGATE, k_states[b][0], k_states[b][1]);
                ASSERT(menu_invariant_holds(&m, k_states[b][0], k_states[b][1]));
            }
        }
    }
}

TEST(actions_garbage_and_zero_initialised_menus_are_repaired)
{
    /* The view is allowed to hand the model whatever is in the struct. An
     * unclamped sel here indexes items[] out of bounds, so every entry point
     * has to rebuild the fields from the engine state rather than trust them. */
    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];
        static const MenuOp ops[] = {
            ACTMENU_NONE, ACTMENU_OPEN, ACTMENU_NAVIGATE, ACTMENU_TOGGLE, ACTMENU_CANCEL
        };

        for (int fill = 0; fill < 2; fill++) {
            for (size_t o = 0; o < sizeof ops / sizeof ops[0]; o++) {
                ActionMenu m;

                if (fill) memset(&m, 0xFF, sizeof m); /* every field maximally wrong */
                else     memset(&m, 0x00, sizeof m); /* never initialised at all */

                (void)actions_apply(&m, ops[o], running, paused);
                if (!menu_invariant_holds(&m, running, paused))
                    FAIL_MSG("fill=%d op=%d state=%s: sel=%d count=%d open=%d",
                             fill, (int)ops[o], state_name(k_states[s]), m.sel, m.count, m.open);
                ASSERT(menu_invariant_holds(&m, running, paused));

                /* `open` is normalised to exactly 0 or 1, so a view that
                 * branches on it never sees a stray truthy value. */
                ASSERT(m.open == 0 || m.open == 1);
                ASSERT(m.count >= 0 && m.count <= ITEM_COUNT);
                if (m.open) {
                    ASSERT(m.sel >= 0 && m.sel < m.count);
                } else {
                    ASSERT(m.sel == 0);
                }
            }
        }
    }
}

TEST(actions_fuzz_60000_navigates_holds_the_invariant_every_time)
{
    /* The property test the module's own header claims is the reason it was
     * split out of the view: hammer the highlight for tens of thousands of
     * keypresses with no screen attached. The engine state is flipped
     * mid-loop on purpose, because a highlight that only ever moves in a fixed
     * enabled set never has to survive the set shrinking underneath it.
     *
     * Seeded explicitly so a failure reproduces exactly on CI.
     *
     * The invariant check is done in a non-printing predicate and reported once
     * on the first violation: the harness prints every failed assertion, and a
     * broken clamp would otherwise produce sixty thousand identical lines and
     * bury the iteration number that actually matters. */
    enum { STEPS = 60000 };
    ActionMenu m;
    int running = 0, paused = 0;
    unsigned char seen_sel[ITEM_COUNT + 1];
    unsigned char seen_count[ITEM_COUNT + 1];
    int moves = 0, previous = -1;
    int prev_count = -1;

    memset(seen_sel, 0, sizeof seen_sel);
    memset(seen_count, 0, sizeof seen_count);

    g_rng = FUZZ_SEED; /* explicit: never depend on preceding tests */
    actions_init(&m);
    actions_apply(&m, ACTMENU_OPEN, running, paused);

    for (int i = 0; i < STEPS; i++) {
        uint64_t r = rng_next();

        /* One op in four changes the engine underneath the open menu. */
        if ((r & 3u) == 0u) {
            running = (int)((r >> 8) & 1u);
            paused   = (int)((r >> 9) & 1u);
        }

        actions_apply(&m, ACTMENU_NAVIGATE, running, paused);

        if (!menu_invariant_holds(&m, running, paused)) {
            FAIL_MSG("navigate fuzz broke at step %d: sel=%d open=%d count=%d "
                     "running=%d paused=%d", i, m.sel, m.open, m.count, running, paused);
            return;
        }
        if (m.sel != previous) moves++;
        previous = m.sel;
        seen_sel[m.sel] = 1;
        if (m.count != prev_count) seen_count[m.count] = 1;
        prev_count = m.count;
    }

    ASSERT(menu_invariant_holds(&m, running, paused));

    /* Guard against a fuzz that passes because it did nothing: 60000 steps
     * that never move the highlight, or never reach more than one row, would
     * satisfy the invariant trivially. */
    ASSERT(STEPS >= 20000);
    ASSERT(moves > STEPS / 2);
    for (int i = 0; i < m.count; i++) ASSERT(seen_sel[i] == 1);

    /* Both enabled-set sizes must have occurred, or the state machine never
     * actually varied and the clamp was never exercised. Idle and
     * running-not-paused differ by exactly one item. */
    int distinct_counts = 0;
    for (int i = 0; i <= ITEM_COUNT; i++) if (seen_count[i]) distinct_counts++;
    ASSERT(distinct_counts >= 2);
    ASSERT(seen_count[7] == 1); /* idle: Pause and Resume both absent */
    ASSERT(seen_count[8] == 1); /* running (paused or not): exactly one absent */
}

TEST(actions_fuzz_all_ops_in_random_order_holds_the_invariant)
{
    /* The navigate-only fuzz cannot catch a clamp that only exists on the
     * NAVIGATE path. This one interleaves every op, including OPEN (which
     * zeroes sel) and TOGGLE/CANCEL (which close the menu), so the highlight
     * is re-entered at every offset in the list and re-validated on every
     * kind of transition. */
    enum { STEPS = 30000 };
    static const MenuOp ops[] = {
        ACTMENU_NONE, ACTMENU_OPEN, ACTMENU_NAVIGATE, ACTMENU_TOGGLE, ACTMENU_CANCEL
    };
    ActionMenu m;
    int running = 0, paused = 0;
    int toggles_with_a_choice = 0;

    g_rng = FUZZ_SEED ^ 0xD1B54A32D192ED03ULL;
    actions_init(&m);

    for (int i = 0; i < STEPS; i++) {
        uint64_t r = rng_next();
        MenuOp op = ops[(r >> 3) % (sizeof ops / sizeof ops[0])];
        MenuItem got;

        if ((r & 7u) == 0u) {
            running = (int)((r >> 16) & 1u);
            paused   = (int)((r >> 17) & 1u);
        }

        got = actions_apply(&m, op, running, paused);

        if (!menu_invariant_holds(&m, running, paused)) {
            FAIL_MSG("op fuzz broke at step %d (op=%d): sel=%d open=%d count=%d "
                     "running=%d paused=%d", i, (int)op, m.sel, m.open, m.count,
                     running, paused);
            return;
        }

        /* A returned item must be a real, enabled MenuItem -- never the
         * ITEM_COUNT sentinel or a row that is not offered. */
        if (got != ITEM_COUNT) {
            if (op == ACTMENU_TOGGLE && m.count > 0) toggles_with_a_choice++;
            ASSERT(got >= ITEM_RESUME && got < ITEM_COUNT);
            ASSERT(actions_enabled(got, running, paused) == 1);
        }
        /* Closed means nothing highlighted. */
        if (!m.open) ASSERT(m.sel == 0);
    }

    ASSERT(STEPS >= 20000);
    /* Otherwise the op mix never produced a real selection and the "returned
     * item is enabled" branch above never ran. */
    ASSERT(toggles_with_a_choice > 0);
}

/* ======================================================================== main */

int main(void)
{
    return cfs_test_run_all("units");
}
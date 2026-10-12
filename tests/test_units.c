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
/* Includes are relative to this file rather than bare. A quoted include is
 * resolved against the directory of the including file before any -I path is
 * consulted, so the suite builds both from "make test" (which passes no -Isrc)
 * and from a direct command line that does. */
#include "../src/test.h"

#include "../src/actions.h"
#include "../src/chart.h"
#include "../src/fmt.h"
#include "../src/stats.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== helpers */

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring search written by hand rather than with
 * strcasestr or tolower: strcasestr is POSIX-only and tolower is UB on a
 * negative char, and this file has to build identically on MinGW and on
 * glibc/macOS. */
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
 * fmt.c's output is written straight to a tty. C's printf renders a
 * non-finite double as "nan" or "inf", which is the kind of string that makes
 * a terminal draw something other than a number, and it is also what a reader
 * mistakes for a measurement. So every formatter, for every hostile input, has
 * to produce something finite or the "--" placeholder -- never those two
 * three-letter words. A plain strstr would only catch the lowercase spelling,
 * so scan case-insensitively for both. */
static void assert_no_inf(const char *s)
{
    if (s == NULL) {
        FAIL_MSG("formatter returned NULL");
        return;
    }
    if (contains_ci(s, "nan")) FAIL_MSG("nan in output: \"%s\"", s);
    if (contains_ci(s, "inf")) FAIL_MSG("inf in output: \"%s\"", s);
}

/* Deterministic 64-bit xorshift, seeded explicitly per test so a CI failure
 * reproduces exactly. rand() would not: its sequence differs between the CRT
 * implementations this project is built with. */
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

TEST(stats_percentiles_match_the_nearest_rank_definition)
{
    /* Nearest rank, ceil(p*n) clamped to 1..n. These are known values rather
     * than values read back out of the implementation, so switching to linear
     * interpolation -- a defensible choice many libraries make -- is caught
     * here instead of silently changing every latency number on screen. */
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

    /* A NaN percentile is not a percentile. It must land somewhere sane, not
     * fall through a comparison as though it were p == 0. */
    ASSERT(stats_percentile(v, 10, NAN) >= 1.0);
}

TEST(stats_median_even_length_is_the_documented_nearest_rank_p50)
{
    /* stats.h defines percentiles as nearest-rank, and stats.c defines median
     * as p50 under that same rule on purpose, so the chart legend and the
     * summary can never disagree. The consequence on an even-length array is
     * that median is the LOWER of the two middle values, not their average.
     *
     * This is worth pinning rather than inferring: the classic averaging median
     * would give 25.0 here and the module gives 20.0. If the averaging
     * definition is ever wanted, it belongs in stats.c -- and this expectation
     * changes with it, deliberately. */
    const double v[4] = { 10.0, 20.0, 30.0, 40.0 };
    ASSERT_DBL(stats_median(v, 4), 20.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 4, 0.5), 20.0, 0.0);

    /* Odd length has no such ambiguity: the middle value is the median. */
    const double odd[5] = { 50.0, 10.0, 30.0, 20.0, 40.0 };
    ASSERT_DBL(stats_median(odd, 5), 30.0, 0.0);
}

TEST(stats_mean_and_stddev_on_a_known_array)
{
    /* The textbook data set: mean 5, population sd 2, variance 4. */
    const double v[8] = { 2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0 };
    ASSERT_DBL(stats_mean(v, 8), 5.0, 1e-12);
    ASSERT_DBL(stats_stddev(v, 8), 2.0, 1e-12);
    ASSERT_DBL(stats_min(v, 8), 2.0, 0.0);
    ASSERT_DBL(stats_max(v, 8), 9.0, 0.0);
}

TEST(stats_stddev_survives_a_large_constant_offset)
{
    /* The textbook one-pass sum-of-squares form (E[x^2] - E[x]^2) computes
     * 1e16 minus 1e16 here and loses every significant bit -- and square
     * rooting the negative residue gives NaN, which then poisons the chart.
     * Latency samples sit far from zero in exactly this way (a 12 ms idle
     * offset carrying 1 ms of jitter), so this is the realistic case rather
     * than a synthetic one. stats.c uses a two-pass mean, so it is exact. */
    const double v[4] = { 1.0e8, 1.0e8 + 1.0, 1.0e8 + 2.0, 1.0e8 + 3.0 };
    ASSERT_DBL(stats_mean(v, 4), 1.0e8 + 1.5, 1e-6);
    ASSERT_DBL(stats_stddev(v, 4), 1.118033988749895, 1e-9);

    /* One decade up: doubles carry 53 bits, so at 1e9 the two squares share
     * only their leading digits and the residue is even worse. */
    const double w[4] = { 1.0e9, 1.0e9 + 1.0, 1.0e9 + 2.0, 1.0e9 + 3.0 };
    ASSERT_DBL(stats_stddev(w, 4), 1.118033988749895, 1e-9);
}

TEST(stats_stddev_of_an_all_equal_array_is_exactly_zero)
{
    /* Exact, not "near zero". An all-equal run is the common case at the
     * start of a speed test before any variance appears, and a stability
     * grade must read 'A' for it rather than a rounding artefact. Tolerance 0
     * makes any nonzero residue -- or a NaN from a negative variance -- fail. */
    double v[16];
    for (int i = 0; i < 16; i++) v[i] = 5.0;

    ASSERT_DBL(stats_mean(v, 16), 5.0, 0.0);
    ASSERT_DBL(stats_stddev(v, 16), 0.0, 0.0);
}

TEST(stats_cv_of_an_all_equal_array_is_zero_and_not_nan)
{
    /* This is the specific NaN trap in the module. CV is stddev/mean, and an
     * all-equal array is exactly the input where that division degenerates.
     * A NaN CV reaches the UI as a stability bar with no height and a letter
     * grade of 'F' for a perfectly steady link.
     *
     * ASSERT_DBL treats NaN as a failure by construction (the harness checks
     * is_nan() before it compares anything), so these lines fail on NaN even
     * though "got != want" would also be true of it. */
    double v[8];
    for (int i = 0; i < 8; i++) v[i] = 100.0;
    ASSERT_DBL(stats_cv_percent(v, 8), 0.0, 1e-12);

    /* The same division, reached through a mean that is genuinely 0. */
    const double zeros[4] = { 0.0, 0.0, 0.0, 0.0 };
    ASSERT_DBL(stats_cv_percent(zeros, 4), 0.0, 1e-12);

    /* And the ordinary case, so a function that always returned 0 could not
     * pass the two lines above. */
    const double spread[4] = { 90.0, 100.0, 110.0, 100.0 };
    ASSERT_DBL(stats_cv_percent(spread, 4), 7.0710678118654756, 1e-12);
}

TEST(stats_jitter_of_short_arrays_is_zero)
{
    /* A single probe has no interarrival gap, so jitter is 0 by definition.
     * Anything else makes the first frame of a run show phantom jitter. */
    const double one[1] = { 42.0 };
    ASSERT_DBL(stats_jitter(one, 1), 0.0, 0.0);
    ASSERT_DBL(stats_jitter(NULL, 0), 0.0, 0.0);

    /* RFC 3550 jitter: the mean of |v[i] - v[i-1]|. */
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
    ASSERT_DBL(stats_loss_percent(1, 3), 200.0 / 3.0, 1e-9);

    /* A counter that over-reports is a counting bug upstream, not negative
     * loss. The documented answer is 0 and the grade table agrees. */
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

TEST(stats_survives_inputs_that_overflow_the_accumulator)
{
    /* sane() is the module's last line of defence. The file header promises
     * every function returns a finite result "or exactly 0.0", and the comment
     * on sane() gives the reason: "NaN is the only value that renders as
     * garbage; inf draws as a full bar." The finite filter upstream keeps NaN
     * out of the inputs, but it cannot stop an accumulator from overflowing --
     * two samples of 1e308 sum to inf, and inf/2 is still inf. Every other
     * totality test here feeds in NaN, which the filter already handles, so
     * without this one the guarantee would rest on untested code. */
    const double huge[2] = { 1.0e308, 1.0e308 };

    ASSERT_DBL(stats_mean(huge, 2), 0.0, 0.0);            /* sum overflows */
    ASSERT_DBL(stats_stddev(huge, 2), 0.0, 0.0);          /* squares overflow */
    ASSERT_DBL(stats_jitter(huge, 2), 0.0, 0.0);
    ASSERT_DBL(stats_cv_percent(huge, 2), 0.0, 0.0);
    ASSERT_DBL(stats_trimmed_mean(huge, 2), 0.0, 0.0);

    /* Rank statistics read an element rather than an accumulated sum, so they
     * stay exact and must NOT be flattened to 0: a genuine extreme sample is a
     * measurement, not a missing one. */
    ASSERT_DBL(stats_median(huge, 2), 1.0e308, 0.0);
    ASSERT_DBL(stats_min(huge, 2), 1.0e308, 0.0);
    ASSERT_DBL(stats_max(huge, 2), 1.0e308, 0.0);
    ASSERT_DBL(stats_percentile(huge, 2, 0.5), 1.0e308, 0.0);
    ASSERT_DBL(stats_bufferbloat(huge, 2, 1.0), 1.0e308, 0.0);
}

TEST(stats_nan_samples_are_dropped_not_propagated)
{
    /* A dropped probe must not shift every rank. With {1, NaN, 3, NaN, 5} the
     * three survivors are the data, so min is 1 and max is 5. Carrying the NaN
     * through would make both NaN, and the axis top label garbage. */
    const double v[5] = { 1.0, NAN, 3.0, NAN, 5.0 };

    ASSERT_DBL(stats_min(v, 5), 1.0, 0.0);
    ASSERT_DBL(stats_max(v, 5), 5.0, 0.0);
    ASSERT_DBL(stats_mean(v, 5), 3.0, 1e-12);
    ASSERT_DBL(stats_median(v, 5), 3.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 5, 0.0), 1.0, 0.0);
    ASSERT_DBL(stats_percentile(v, 5, 1.0), 5.0, 0.0);
    ASSERT_DBL(stats_stddev(v, 5), 1.632993161855452, 1e-12);
    ASSERT_DBL(stats_cv_percent(v, 5), 54.43310539518174, 1e-10);
    ASSERT_DBL(stats_trimmed_mean(v, 5), 3.0, 1e-12);

    /* Bufferbloat shifts the survivors and takes their median: the survivors
     * are {1,3,5}, minus an idle median of 1, giving {0,2,4} and a median 2. */
    ASSERT_DBL(stats_bufferbloat(v, 5, 1.0), 2.0, 0.0);
}

TEST(stats_jitter_does_not_bridge_across_a_dropped_probe)
{
    /* The only finite adjacent pair here is 10 -> 40. Interpolating across the
     * NaNs would report 30 ms of jitter the network never showed, which is
     * worse than reporting nothing: the user sees a degraded call and blames
     * their own setup. Skipping the pair entirely gives 0. */
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

    /* NULL as well as n == 0: callers pass straight from their sample buffer,
     * and a NULL is the "no samples yet" state, not a programming error. */
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

    /* Sorted {40,50,60,70,80}; p50 nearest-rank is 60. Shifted by an idle
     * median of 20 that is {20,30,40,50,60}, whose p50 is 40. */
    ASSERT_DBL(stats_bufferbloat(loaded, 5, 20.0), 40.0, 0.0);
    /* Shifting by 50 gives {-10, 0, 10, 20, 30}, whose p50 nearest rank is
     * the third element, 10 -- not 30. A negative result is legitimate here:
     * it means the idle median was higher than the loaded median. */
    ASSERT_DBL(stats_bufferbloat(loaded, 5, 50.0), 10.0, 0.0);

    /* No usable idle measurement means no answer, not a guess. */
    ASSERT_DBL(stats_bufferbloat(loaded, 5, 0.0), 0.0, 0.0);
    ASSERT_DBL(stats_bufferbloat(loaded, 5, -1.0), 0.0, 0.0);
    ASSERT_DBL(stats_bufferbloat(loaded, 5, NAN), 0.0, 0.0);
}

TEST(stats_grades_follow_the_documented_thresholds)
{
    /* Both sides of every boundary. An off-by-one in the ladder turns a clean
     * 'A' into a 'B' and sends the user hunting for a fault they do not have. */
    ASSERT(stats_grade_bufferbloat(0.0)   == 'A');
    ASSERT(stats_grade_bufferbloat(5.0)   == 'A');
    ASSERT(stats_grade_bufferbloat(5.01)  == 'B');
    ASSERT(stats_grade_bufferbloat(30.0)  == 'B');
    ASSERT(stats_grade_bufferbloat(30.01) == 'C');
    ASSERT(stats_grade_bufferbloat(50.0)  == 'C');
    ASSERT(stats_grade_bufferbloat(50.01) == 'D');
    ASSERT(stats_grade_bufferbloat(100.0) == 'D');
    ASSERT(stats_grade_bufferbloat(100.01) == 'E');
    ASSERT(stats_grade_bufferbloat(200.0) == 'E');
    ASSERT(stats_grade_bufferbloat(200.01) == 'F');

    ASSERT(stats_grade_stability(0.0)   == 'A');
    ASSERT(stats_grade_stability(2.0)   == 'A');
    ASSERT(stats_grade_stability(2.01)  == 'B');
    ASSERT(stats_grade_stability(4.0)   == 'B');
    ASSERT(stats_grade_stability(4.01)  == 'C');
    ASSERT(stats_grade_stability(8.0)   == 'C');
    ASSERT(stats_grade_stability(15.0)  == 'D');
    ASSERT(stats_grade_stability(25.0)  == 'E');
    ASSERT(stats_grade_stability(25.01) == 'F');

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
    for (int i = 0; i < 100; i++) ramp[i] = (double)(i + 1);
    ASSERT_DBL(stats_trimmed_mean(ramp, 100), 50.0, 1e-12);

    /* Below the 8-sample floor it must agree with the plain mean exactly,
     * otherwise the average shown for a short run disagrees with every other
     * average in the app. */
    const double seven[7] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 };
    ASSERT_DBL(stats_trimmed_mean(seven, 7), 4.0, 1e-12);
    ASSERT_DBL(stats_trimmed_mean(seven, 7), stats_mean(seven, 7), 0.0);
}

TEST(stats_large_sample_count_is_handled)
{
    /* 1000 samples pushes stats.c past its 256-entry stack scratch onto the
     * heap, so this is the path a real run of a minute or more reaches. The
     * assertions are structural rather than numeric because the point is that
     * the path runs and stays sane at a size a speed test actually hits. */
    double *v = (double *)malloc(1000 * sizeof *v);
    double *sorted = (double *)malloc(1000 * sizeof *sorted);

    if (v == NULL || sorted == NULL) {
        FAIL_MSG("out of memory");
        free(v);
        free(sorted);
        return;
    }
    for (int i = 0; i < 1000; i++) v[i] = (double)((i * i) % 997) + 1.0;
    memcpy(sorted, v, 1000 * sizeof *v);

    /* Order of the input must not matter: stats.h promises the caller need not
     * sort, and a stats function that only works on sorted data would break
     * the moment the engine hands over an unsorted ring buffer. */
    ASSERT_DBL(stats_mean(v, 1000), stats_mean(sorted, 1000), 0.0);
    ASSERT_DBL(stats_median(v, 1000), stats_median(sorted, 1000), 0.0);
    ASSERT_DBL(stats_stddev(v, 1000), stats_stddev(sorted, 1000), 0.0);
    ASSERT_DBL(stats_max(v, 1000), 997.0, 0.0);
    ASSERT_DBL(stats_min(v, 1000), 1.0, 0.0);

    /* A percentile ladder that goes backwards is a broken rank lookup, and it
     * is the kind of bug that only shows on a full ring buffer. */
    double prev = stats_percentile(v, 1000, 0.0);
    for (int p = 5; p <= 100; p += 5) {
        double cur = stats_percentile(v, 1000, (double)p / 100.0);
        ASSERT(cur >= prev);
        ASSERT(cur <= 997.0);
        prev = cur;
    }
    ASSERT(stats_stddev(v, 1000) >= 0.0);
    ASSERT(stats_trimmed_mean(v, 1000) >= 1.0);

    free(v);
    free(sorted);
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

    /* Exactly as many bars as columns requested: one short and the trace stops
     * before the right edge, one long and the renderer walks past h[]. */
    ASSERT(cs.bars == 80);
    ASSERT(cs.bars <= CHART_MAX_BARS);
    ASSERT(cs.zeroed == 0);
    ASSERT_DBL(cs.peak, 940.0, 1e-9);
    ASSERT(cs.mean > 0.0);
    ASSERT(cs.mean <= cs.peak);
    ASSERT(cs.p95 >= 0.0);
    ASSERT(cs.p95 <= cs.peak); /* a p95 above the peak means the axis lies */

    for (int i = 0; i < cs.bars; i++) {
        ASSERT_DBL(cs.h[i], cs.h[i], 0.0); /* finite: NaN != NaN fails here */
        ASSERT(cs.h[i] >= 0.0f);
        ASSERT(cs.h[i] <= 1.0f);
    }
    /* Heights normalise against the nice ceiling, not the raw peak, so the
     * tallest bar stops just short of the top edge (940 -> axis 1000 -> 0.94)
     * and leaves room for the label that marks 1000. */
    ASSERT_DBL(cs.h[cs.bars - 1], 0.94, 1e-6);

    /* Column 0 is a peak-of-bucket over samples[0..11], not sample[0]: the
     * integer bucket edges put twelve samples in each of eighty columns. The
     * exact expected height pins that edge arithmetic (lo = 0*1000/80 = 0,
     * hi = 1*1000/80 = 12), which is where bucket maths drifts and a spike
     * ends up smeared over two columns. Tolerance covers the float storage. */
    ASSERT_DBL(cs.h[0], (940.0 * 11.0 / 999.0) / 1000.0, 1e-7);

    /* A monotonic input must produce a monotonic chart. Non-decreasing
     * heights are what prove no bucket was dropped or double-counted. */
    for (int i = 1; i < cs.bars; i++) ASSERT(cs.h[i] >= cs.h[i - 1]);
}

TEST(chart_scale_of_an_all_zero_array_is_all_zero_not_nan)
{
    double samples[200];
    ChartScale cs;

    for (size_t i = 0; i < 200; i++) samples[i] = 0.0;
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 80, &cs);

    /* zeroed is the caller's signal to draw a baseline with no top label. The
     * tempting mistake is normalising by a peak of 0, which gives 0/0 -- NaN --
     * and NaN heights render as an axis of garbage or a crash in the glyph
     * loop. Exact 0 with tolerance 0 rejects NaN here. */
    ASSERT(cs.zeroed == 1);
    ASSERT(cs.bars == 80);
    ASSERT_DBL(cs.peak, 0.0, 0.0);
    ASSERT_DBL(cs.mean, 0.0, 0.0);
    ASSERT_DBL(cs.p95, 0.0, 0.0);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], 0.0, 0.0);

    /* All-negative counts the same way: nothing was measured, as opposed to
     * "negative", which is not a thing a bar chart can show. mean and p95 are
     * forced to zero here too, not left at the raw -5.0 -- a negative mean
     * printed beside a zero-topped axis is a lie about the measurement, and
     * it is the only observable difference between these two paths. */
    for (size_t i = 0; i < 200; i++) samples[i] = -5.0;
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 80, &cs);
    ASSERT(cs.zeroed == 1);
    ASSERT_DBL(cs.peak, 0.0, 0.0);
    ASSERT_DBL(cs.mean, 0.0, 0.0);
    ASSERT_DBL(cs.p95, 0.0, 0.0);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], 0.0, 0.0);
}

TEST(chart_scale_clamps_a_width_beyond_the_bar_array)
{
    double samples[CHART_SAMPLES];
    ChartScale cs;

    chart_ramp(samples, CHART_SAMPLES, 0.0, 100.0);

    /* 2000 > CHART_MAX_BARS. An unclamped write runs off the end of the 2 KB
     * h[] array inside the struct -- heap corruption, not a failed assertion --
     * so this is the test that has to exist. The sentinel fill also proves the
     * scalar fields came out of chart_scale rather than surviving untouched. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, CHART_SAMPLES, 2000, &cs);
    ASSERT(cs.bars == CHART_MAX_BARS);
    ASSERT(cs.bars <= CHART_MAX_BARS);
    ASSERT_DBL(cs.peak, 100.0, 1e-9);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], cs.h[i], 0.0);

    /* Far past the limit, to catch an off-by-one in the comparison itself. */
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
    double samples[8] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
    ChartScale cs;

    /* bars == 0 is the "nothing to draw" signal. The sentinel proves the early
     * return ran before any write, which is what stops a zero-length call from
     * reading past a caller's (possibly NULL) buffer. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(NULL, 0, 80, &cs);
    ASSERT(cs.bars == 0); /* the documented "nothing to draw at all" signal */
    ASSERT_DBL(cs.peak, 0.0, 0.0);
    ASSERT_DBL(cs.mean, 0.0, 0.0);
    ASSERT_DBL(cs.p95, 0.0, 0.0);
    /* chart.c also sets zeroed on this path, on the grounds that "0 samples"
     * and "a run of zeros" must not both look like data. chart.h still
     * documents the empty-call signal as bars == 0 and describes zeroed as
     * "1 when every sample was <= 0", which is vacuously true of an empty
     * set. Both readings are self-consistent, so pin only that the flag is
     * clean rather than a raw sentinel byte. */
    ASSERT(cs.zeroed == 0 || cs.zeroed == 1);

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
     * stutter would be invisible, which is the bug this pins. */
    double samples[200];
    ChartScale cs;

    for (int i = 0; i < 200; i++) samples[i] = 10.0;
    samples[100] = 100.0; /* bucket 10 spans [100, 110) */

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 20, &cs);

    ASSERT(cs.bars == 20);
    ASSERT_DBL(cs.peak, 100.0, 1e-9);
    ASSERT_DBL(cs.h[10], 1.0, 1e-6); /* bucket 10 spans [100, 110) */
    ASSERT_DBL(cs.h[0], 0.1, 1e-6);  /* the rest sit at 10/100 */
    ASSERT_DBL(cs.h[19], 0.1, 1e-6);
    ASSERT_DBL(cs.mean, 10.45, 1e-9); /* (199*10 + 100) / 200 */

    /* Every sample must land in exactly one column and none may be dropped, so
     * uneven bucket edges have to hold up. 200 does not divide by 7, which is
     * where integer edge arithmetic drifts and a spike ends up smeared across
     * two columns or lost altogether. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 7, &cs);
    ASSERT(cs.bars == 7);
    for (int i = 0; i < cs.bars; i++) ASSERT_DBL(cs.h[i], cs.h[i], 0.0);
    ASSERT_DBL(cs.h[3], 1.0, 1e-6); /* bucket 3 spans [85, 114) */

    /* The tightest case: one column per sample, so every bucket is a single
     * element and any off-by-one in the edge maths shows immediately. */
    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 200, 200, &cs);
    ASSERT(cs.bars == 200);
    ASSERT_DBL(cs.h[100], 1.0, 1e-6);
}

TEST(chart_scale_of_a_nan_contaminated_trace_is_still_finite)
{
    /* One failed transfer yields NaN samples. They must not reach h[], or the
     * bar glyph loop multiplies NaN by a row count and indexes outside the row
     * buffer. Both bucketing paths are exercised: n > bars, and n <= bars,
     * which take different code. */
    double samples[64];
    ChartScale cs;

    for (int i = 0; i < 64; i++) samples[i] = (i % 4 == 0) ? NAN : 40.0;

    memset(&cs, 0xAA, sizeof cs);
    chart_scale(samples, 64, 16, &cs); /* bucketed path */
    ASSERT(cs.bars == 16);
    ASSERT(cs.zeroed == 0);
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
    ASSERT_DBL(chart_nice_ceiling(2.5), 5.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(9.99), 10.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(0.03), 0.05, 1e-12);
    ASSERT_DBL(chart_nice_ceiling(0.5), 0.5, 1e-12);

    /* Never zero, whatever it is handed. A zero axis top makes the height
     * normalisation a division by zero, so every bar becomes NaN. */
    ASSERT(chart_nice_ceiling(0.0) != 0.0);
    ASSERT(chart_nice_ceiling(-1.0) != 0.0);
    ASSERT(chart_nice_ceiling(NAN) != 0.0);
    ASSERT_DBL(chart_nice_ceiling(0.0), 1.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(NAN), 1.0, 0.0);

    /* Decade boundaries are where log10 rounding bites: 100 must not come out
     * as 1000, which is the single most visible axis bug available. */
    ASSERT_DBL(chart_nice_ceiling(100.0), 100.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(1000.0), 1000.0, 0.0);
    ASSERT_DBL(chart_nice_ceiling(0.1), 0.1, 1e-12);
    ASSERT_DBL(chart_nice_ceiling(1e-9), 1e-9, 1e-20);

    /* It must be a genuine ceiling over a wide spread: never below the raw peak
     * (the tallest bar would overflow its own axis) and never more than a
     * factor of 2.5 above it. 2.5, not 2, is the exact supremum of a 1/2/5
     * ladder -- a raw just above the "2" mantissa, say 2.01, rounds up to 5. */
    for (double raw = 0.5; raw < 5000.0; raw *= 1.37) {
        double nice = chart_nice_ceiling(raw);
        ASSERT(isfinite(nice));
        ASSERT(nice >= raw);
        ASSERT(nice / raw <= 2.5 + 1e-9);
    }
}

/* One guard buffer for every chart_ticks overflow case. The 0xAA fill is the
 * canary: if chart_ticks writes past cap, the byte after the terminator is no
 * longer 0xAA and the run fails loudly instead of quietly corrupting a stack
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
    if (len >= cap) {
        FAIL_MSG("chart_ticks(%.10g, cap=%zu) left no terminator", top, cap);
        return 0;
    }
    if (len != strlen((const char *)buf)) {
        FAIL_MSG("chart_ticks(%.10g, cap=%zu) is not NUL-terminated", top, cap);
        return 0;
    }
    for (size_t i = cap; i < TICK_GUARD; i++) {
        if (buf[i] != 0xAA) {
            FAIL_MSG("chart_ticks(%.10g, cap=%zu) overran to out[%zu]", top, cap, i);
            return 0;
        }
    }
    return 1;
}

TEST(chart_ticks_never_overrun_or_split_a_label)
{
    /* The three caps from the brief plus the neighbouring ones. A clipped
     * label is worse than a short axis, so the contract is a NUL-terminated
     * prefix of whole labels: the byte at cap-1 is always the terminator and
     * everything from cap on is untouched. */
    static const size_t caps[] = { 0, 1, 2, 3, 8, 16, TICK_GUARD };
    static const double tops[] = { 0.0, 0.5, 1.0, 87.3, 100.0, 101.0, 1000.0, 1e6 };

    for (size_t t = 0; t < sizeof tops / sizeof tops[0]; t++) {
        for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
            if (!chart_ticks_well_formed(tops[t], caps[c], NULL))
                FAIL_MSG("top=%.10g cap=%zu", tops[t], caps[c]);
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

    /* A 1 KB buffer for a 6-label axis must not be the thing that saves us:
     * check the return value is a real label count and the output is bounded. */
    memset(buf, 0, sizeof buf);
    ASSERT(chart_ticks(100.0, buf, sizeof buf) == 6);
    ASSERT(strlen(buf) < sizeof buf);

    /* No room at all is a valid call, not a fault. */
    ASSERT(chart_ticks(100.0, NULL, 0) == 0);
    ASSERT(chart_ticks(100.0, buf, 0) == 0);
}

TEST(chart_ticks_produce_a_usable_numeric_axis)
{
    char buf[TICK_GUARD];

    /* Known output for the canonical 0..100 axis. step = nice_ceiling(100/5)
     * = 20, giving 0/20/40/60/80 and an anchor on 100. If this comes out as
     * "0\n25\n50\n75\n100" the ladder moved, and every axis on screen moves
     * with it. */
    memset(buf, 0, sizeof buf);
    ASSERT(chart_ticks(100.0, buf, sizeof buf) == 6);
    ASSERT_STR(buf, "0\n20\n40\n60\n80\n100");

    /* Structural properties for every reasonable top: four to six labels, each
     * a complete finite number, ascending, starting at zero. A clipped or
     * duplicated label is exactly what a short buffer produces if the
     * append-only-if-it-fits rule is removed. */
    static const double tops[] = { 0.0, 0.25, 1.0, 5.0, 87.3, 100.0, 101.0, 250.0, 1e6 };
    for (size_t i = 0; i < sizeof tops / sizeof tops[0]; i++) {
        double prev = -1.0;
        int labels = 0;
        int count;

        /* Fill this buffer, then parse the same buffer -- comparing the label
         * count against what the text actually contains is the point. */
        memset(buf, 0, sizeof buf);
        count = chart_ticks(tops[i], buf, sizeof buf);
        if (!chart_ticks_well_formed(tops[i], TICK_GUARD, NULL)) continue;
        ASSERT(count >= 4 && count <= 6);

        {
            char *line = buf;
            while (line != NULL && *line != '\0') {
                char *end = NULL;
                double v = strtod(line, &end);

                /* strtod stops at the separating newline, so a label is complete when end
                 * points at either the newline or the final terminator. Anything
                 * else means the renderer would print half a number. */
                if (end == line || (*end != '\0' && *end != '\n')) {
                    FAIL_MSG("truncated tick label at %ld: \"%s\"", (long)(end - line), line);
                    break;
                }
                if (!isfinite(v)) { FAIL_MSG("non-finite tick label: \"%s\"", line); break; }
                if (v < prev) { FAIL_MSG("ticks descend at %.10g: %.10g after %.10g", tops[i], v, prev); break; }
                if (labels == 0 && v != 0.0) { FAIL_MSG("axis does not start at 0 for %.10g", tops[i]); break; }
                prev = v;
                labels++;
                line = strchr(line, '\n');
                if (line) line++;
            }
        }
        ASSERT(labels == count);
    }
}

/* =========================================================================== fmt */

/* The five inputs from the brief, plus the two doubles hardest to render in a
 * fixed shape. DBL_MAX is here because "%.1f" of it is a 310-character string:
 * it has to clip, not corrupt. */
static const double k_hostile[] = {
    NAN, INFINITY, -INFINITY, -1.0, 0.0, DBL_MAX, -DBL_MAX
};
#define N_HOSTILE (sizeof k_hostile / sizeof k_hostile[0])

TEST(fmt_no_formatter_ever_prints_nan_or_inf)
{
    /* The headline test. Every double-taking formatter, every hostile value. A
     * single "nan" or "inf" reaching a terminal is a visible corruption of the
     * user's screen and a silent lie about the measurement, and the only real
     * defence is that it is impossible to produce one. */
    for (size_t i = 0; i < N_HOSTILE; i++) {
        const double v = k_hostile[i];
        char buf[64];

        buf[0] = '\0';
        fmt_rate(buf, sizeof buf, v);     assert_no_inf(buf);
        buf[0] = '\0';
        fmt_ms(buf, sizeof buf, v);       assert_no_inf(buf);
        buf[0] = '\0';
        fmt_pct(buf, sizeof buf, v);      assert_no_inf(buf);
        buf[0] = '\0';
        fmt_duration(buf, sizeof buf, v); assert_no_inf(buf);

        /* Clipping a real label for a narrow column must not produce one either:
         * the truncated form is the one that actually reaches the screen. */
        buf[0] = '\0';
        fmt_rate(buf, sizeof buf, 1.0e300);
        fmt_truncate(buf, sizeof buf, buf, 4);
        assert_no_inf(buf);
    }

    /* fmt_stamp takes an integer, but a narrowing cast at the call site can
     * turn a bad double into whatever integer the cast produces. */
    char buf[64];
    buf[0] = '\0';
    fmt_stamp(buf, sizeof buf, (int64_t)INFINITY);
    assert_no_inf(buf);
}

TEST(fmt_non_finite_doubles_render_as_the_missing_placeholder)
{
    char buf[64];

    /* "--" rather than "0": a missing sample that reads as a real zero is the
     * one formatting bug nobody notices, because the number looks plausible.
     * Only the genuinely unmeasurable values get it. */
    static const double bad[] = { NAN, INFINITY, -INFINITY };
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
    }

    /* A negative is a different case and each formatter treats it on purpose.
     * fmt_rate and fmt_ms treat it as unmeasurable (a speed or a latency is
     * not negative). fmt_pct keeps the sign, because a negative percentage is
     * information about the measurement and hiding it would be a formatting
     * choice dressed up as a fact. fmt_duration clamps it to zero, because a
     * duration is documented as never negative. Pinning all three stops a
     * future "harmonisation" from silently dropping a sign. */
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, -1.0);     ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, -1.0);       ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_pct(buf, sizeof buf, -1.0);      ASSERT_STR(buf, "-1.0");
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, -1.0); ASSERT_STR(buf, "0s");

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

    /* One shape across the whole range. The live tab used to drop the decimal
     * above 100 and switch to Gbps above 1000, so the same sample read "111
     * Mbps" on one screen and "110.5" on another. */
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 110.5);  ASSERT_STR(buf, "110.5");
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 0.0);     ASSERT_STR(buf, "0.0");
    memset(buf, 0, sizeof buf); fmt_rate(buf, sizeof buf, 1.24);    ASSERT_STR(buf, "1.2");
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

    /* Past this the figure stops being a latency and becomes a column width;
     * the unit and the decimal are both noise at that magnitude. */
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 9999.0);  ASSERT_STR(buf, "9999");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 10000.0); ASSERT_STR(buf, "10000");
    memset(buf, 0, sizeof buf); fmt_ms(buf, sizeof buf, 12000.0); ASSERT_STR(buf, "12000");
}

TEST(fmt_bytes_picks_the_right_binary_unit)
{
    /* Known values across every rung, including the three documented examples.
     * The KB/MB/GB spelling with 1024-based divisors is deliberate, so the
     * exact strings are pinned rather than just the magnitudes. */
    static const struct { uint64_t bytes; const char *want; } cases[] = {
        { 0,                 "0 B"    },
        { 1,                 "1 B"    },
        { 512,               "512 B"  },
        { 1023,              "1023 B" },
        { 1024,              "1 KB"   },
        { 812ull * 1024ull,  "812 KB" },
        { 5242880ull,        "5.0 MB" },
        { 5452596ull,        "5.2 MB" },
        { 1073741824ull,     "1.0 GB" },
        { 1503238554ull,     "1.4 GB" },
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

    /* The largest count must not round into the wrong bucket or overflow a
     * shift on the way there. 2^64-1 bytes is 16 EiB, well inside the GB rung. */
    memset(buf, 0, sizeof buf);
    fmt_bytes(buf, sizeof buf, UINT64_MAX);
    ASSERT(strchr(buf, 'G') != NULL);
    assert_no_inf(buf);
}

TEST(fmt_stamp_rejects_times_it_cannot_print)
{
    char buf[64];

    /* Zero is the epoch, and printing 1970 for it would look like a real run
     * from 1970 rather than a run that never recorded a timestamp. */
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, 0); ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, -1); ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, INT64_MIN); ASSERT_STR(buf, "--");
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, INT64_MAX); ASSERT_STR(buf, "--");

    /* Past 9999 the printed shape no longer holds, so it is unprintable rather
     * than guessed at (and a 32-bit time_t would wrap to an unrelated date). */
    memset(buf, 0, sizeof buf); fmt_stamp(buf, sizeof buf, INT64_C(253402300800000));
    ASSERT_STR(buf, "--");

    /* A real timestamp: assert the shape, not the value. The value depends on
     * the runner's timezone, and a test that pins one locks CI to whatever zone
     * the machine happens to have. The separators and field widths are the
     * contract. */
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

    /* Clamped, not converted: the narrowing cast out of double range is
     * undefined behaviour rather than a wrap. 1e9 s is the documented ceiling
     * and formats as 277777h 46m. */
    memset(buf, 0, sizeof buf); fmt_duration(buf, sizeof buf, 1e300);
    ASSERT_STR(buf, "277777h 46m");
}

TEST(fmt_truncate_handles_degenerate_caps_and_widths)
{
    char buf[64];

    /* Short strings are copied verbatim, and an exact fit does not gain an
     * ellipsis -- otherwise every full-width cell in the table ends in one. */
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, "ab", 8);    ASSERT_STR(buf, "ab");
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, "abcdef", 6); ASSERT_STR(buf, "abcdef");
    memset(buf, 0, sizeof buf); fmt_truncate(buf, sizeof buf, "abcde", 5); ASSERT_STR(buf, "abcde");

    /* Far longer than the width. U+2026 is one column and three bytes, so ten
     * characters at width 5 keep four and append it. */
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

    /* cap == 1 holds only the terminator and must not write the second byte.
     * This is the smallest cap that can silently corrupt the stack if the room
     * arithmetic is off by one, so the canary byte is checked too. */
    memset(buf, 0xAA, sizeof buf); fmt_truncate(buf, 1, "abcdef", 5);
    ASSERT(buf[0] == '\0');
    ASSERT(buf[1] == (char)0xAA);

    /* Not even the marker fits: giving up a column beats emitting half a glyph,
     * which renders as a replacement character. */
    memset(buf, 0, sizeof buf); fmt_truncate(buf, 3, "abcdef", 5);
    ASSERT_STR(buf, "");
    memset(buf, 0, sizeof buf); fmt_truncate(buf, 2, "abcdef", 5);
    ASSERT_STR(buf, "");
}

TEST(fmt_truncate_never_splits_a_multibyte_glyph)
{
    char buf[64];

    /* Two U+00E9, so bytes C3 A9 C3 A9. Width 2 leaves one column of content,
     * which lands on byte 1: a continuation byte. Copying it would put half a
     * character on screen as a tofu box, so the content gives up its column and
     * only the marker is emitted. */
    memset(buf, 0, sizeof buf);
    fmt_truncate(buf, sizeof buf, "\xc3\xa9\xc3\xa9", 2);
    ASSERT_STR(buf, "\xe2\x80\xa6");
    ASSERT(strlen(buf) == 3);

    /* A cut that lands on a lead byte keeps the whole glyph. */
    memset(buf, 0, sizeof buf);
    fmt_truncate(buf, sizeof buf, "\xc3\xa9\xc3\xa9\xc3\xa9", 3);
    ASSERT_STR(buf, "\xc3\xa9\xe2\x80\xa6");

    /* cap clamps the content, not the marker, and always leaves room for the
     * terminator. */
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
     * anyway" -- which is a one-byte overrun past whatever the caller had. */
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
     * caller sizes the buffer to the column and the string has to fit. Every
     * formatter gets a canary buffer one byte larger than the cap it is given;
     * if anything writes past cap, a canary byte changes. */
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
                FAIL_MSG("cap=%zu overran to out[%zu], last write was \"%s\"", cap, i, p);
                break;
            }
        }
    }
}

/* ======================================================================= actions */

/* The one invariant that matters in this module: an out-of-range sel reaching
 * the renderer is an out-of-bounds read, and a sel that lands on a disabled row
 * means the user pressed enter and nothing happened. Checked after every op, in
 * every state, on every step of both fuzzes. */
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
        ASSERT(m.count >= 1);   /* never empty, so sel always has somewhere to be */
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

        if (running && paused) ASSERT(m.items[0] == ITEM_RESUME);
        else if (running)      ASSERT(m.items[0] == ITEM_PAUSE);
        else                   ASSERT(m.items[0] == ITEM_RERUN);
    }

    /* The two state-dependent rows, on both sides of each boundary. */
    ASSERT(actions_enabled(ITEM_RESUME, 1, 1) == 1);
    ASSERT(actions_enabled(ITEM_RESUME, 1, 0) == 0);
    ASSERT(actions_enabled(ITEM_RESUME, 0, 1) == 0);
    ASSERT(actions_enabled(ITEM_PAUSE, 1, 0) == 1);
    ASSERT(actions_enabled(ITEM_PAUSE, 1, 1) == 0);
    ASSERT(actions_enabled(ITEM_PAUSE, 0, 0) == 0);

    /* Everything else is available in every state, which is what guarantees the
     * menu is never empty. */
    for (size_t s = 0; s < N_STATES; s++)
        for (int i = ITEM_RERUN; i < ITEM_COUNT; i++)
            ASSERT(actions_enabled((MenuItem)i, k_states[s][0], k_states[s][1]) == 1);

    /* ITEM_COUNT is not a MenuItem and must not be offered. */
    ASSERT(actions_enabled(ITEM_COUNT, 1, 0) == 0);
    ASSERT(actions_enabled((MenuItem)999, 1, 0) == 0);
}

TEST(actions_open_starts_at_the_top_regardless_of_where_the_highlight_was)
{
    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];
        ActionMenu m;

        actions_init(&m);
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        for (int i = 0; i < 3; i++) actions_apply(&m, ACTMENU_NAVIGATE, running, paused);
        ASSERT(m.sel == 3 % m.count);

        /* Close with CANCEL, then reopen: the highlight goes back to the top
         * rather than resuming a row the user never picked. A menu that
         * reopens where it was left reads as the app ignoring the key. */
        actions_apply(&m, ACTMENU_CANCEL, running, paused);
        ASSERT(m.open == 0);
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        ASSERT(m.open == 1);
        ASSERT(m.sel == 0);

        /* Same after choosing an item, and after a TOGGLE that followed a
         * longer walk down the list. */
        for (int i = 0; i < 5; i++) actions_apply(&m, ACTMENU_NAVIGATE, running, paused);
        ASSERT(m.sel != 0);
        (void)actions_apply(&m, ACTMENU_TOGGLE, running, paused);
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        ASSERT(m.sel == 0);
        assert_menu_invariant(&m, running, paused, "after reopen");

        /* OPEN on an already-open menu. CANCEL and TOGGLE both zero sel before
         * a reopen, so the reset can only be observed here: without it the
         * reopen is a no-op on the highlight and the menu appears to ignore
         * the key. */
        for (int i = 0; i < 4; i++) actions_apply(&m, ACTMENU_NAVIGATE, running, paused);
        ASSERT(m.sel != 0);
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        ASSERT(m.open == 1);
        ASSERT(m.sel == 0);
        assert_menu_invariant(&m, running, paused, "reopen while open");
    }
}

TEST(actions_stale_items_are_poisoned_when_the_set_shrinks)
{
    /* Poisoning the slots past count is only observable once the set shrinks.
     * actions_init() already fills the array with ITEM_COUNT, so the very first
     * rebuild hides a missing poison step entirely. Growing the enabled set
     * from idle (7 items) to running (8) and back again leaves index 7 holding
     * a real, enabled item unless rebuild clears it -- and a view that trusts
     * the array instead of the count would then draw a row the user already
     * ruled out. This is the one sequence that separates the two behaviours. */
    ActionMenu m;

    actions_init(&m);
    actions_apply(&m, ACTMENU_OPEN, 0, 0); /* idle: 7 items, {2..8} */
    ASSERT(m.count == 7);
    ASSERT(m.items[0] == ITEM_RERUN);

    actions_apply(&m, ACTMENU_NONE, 1, 0); /* running: 8 items, {1..8} */
    ASSERT(m.count == 8);
    ASSERT(m.items[0] == ITEM_PAUSE);

    actions_apply(&m, ACTMENU_NONE, 0, 0); /* idle again: 7 items, {2..8} */
    ASSERT(m.count == 7);

    for (int i = m.count; i < ITEM_COUNT; i++) {
        if (m.items[i] != ITEM_COUNT)
            FAIL_MSG("stale item at slot %d after the set shrank: %d", i, m.items[i]);
        ASSERT(m.items[i] == ITEM_COUNT);
    }
}

TEST(actions_labels_are_never_null)
{
    /* The view dereferences these unconditionally. A NULL here is a crash in
     * the middle of a redraw, and the header promises it cannot happen. */
    for (int i = 0; i <= ITEM_COUNT; i++) {
        const char *label = actions_label((MenuItem)i);
        ASSERT(label != NULL);
        if (label == NULL) continue;
        /* Only a real row owes the user a readable name. ITEM_COUNT is the
         * "nothing was chosen" sentinel, and actions.c returns "" for it on
         * purpose -- the promise is never-NULL, not never-empty. */
        if (i < ITEM_COUNT) ASSERT(label[0] != '\0');
        ASSERT(strlen(label) < 64);
    }
    /* Distinct rows must be distinct strings, or the user cannot tell them
     * apart in the menu. */
    for (int i = 0; i < ITEM_COUNT; i++)
        for (int j = i + 1; j < ITEM_COUNT; j++)
            ASSERT(strcmp(actions_label((MenuItem)i), actions_label((MenuItem)j)) != 0);
}

TEST(actions_every_op_in_every_state_keeps_the_invariant)
{
    /* The full cross product: 4 states x 5 ops, applied both to a freshly
     * opened menu and to a closed one. Each case checks the invariant, the
     * state of `open`, and the return value. */
    static const MenuOp ops[] = {
        ACTMENU_NONE, ACTMENU_OPEN, ACTMENU_NAVIGATE, ACTMENU_NAVIGATE_BACK,
        ACTMENU_TOGGLE, ACTMENU_CANCEL
    };
    static const char *op_names[] = { "NONE", "OPEN", "NAVIGATE", "NAVIGATE_BACK", "TOGGLE", "CANCEL" };

    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];

        for (size_t o = 0; o < sizeof ops / sizeof ops[0]; o++) {
            ActionMenu from_open, from_closed;
            MenuItem got_open, got_closed;

            actions_init(&from_open);
            actions_apply(&from_open, ACTMENU_OPEN, running, paused);
            got_open = actions_apply(&from_open, ops[o], running, paused);
            if (!menu_invariant_holds(&from_open, running, paused))
                FAIL_MSG("%s + %s from open: sel=%d open=%d count=%d",
                         state_name(k_states[s]), op_names[o],
                         from_open.sel, from_open.open, from_open.count);

            actions_init(&from_closed);
            got_closed = actions_apply(&from_closed, ops[o], running, paused);
            if (!menu_invariant_holds(&from_closed, running, paused))
                FAIL_MSG("%s + %s from closed: sel=%d open=%d count=%d",
                         state_name(k_states[s]), op_names[o],
                         from_closed.sel, from_closed.open, from_closed.count);

            switch (ops[o]) {
                case ACTMENU_NONE:
                    /* No key. The enabled set is still rebuilt: an open menu
                     * has to track an engine that moved underneath it. */
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(from_open.open == 1);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(from_closed.open == 0);
                    break;
                case ACTMENU_OPEN:
                    /* Opens and starts at the top: reopening after a cancel
                     * should not resume a selection the user never made. */
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(from_open.open == 1);
                    ASSERT(from_open.sel == 0);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(from_closed.open == 1);
                    ASSERT(from_closed.sel == 0);
                    break;
                case ACTMENU_NAVIGATE:
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(from_open.open == 1);
                    ASSERT(from_open.sel == 1); /* sel was 0 and count >= 7 */
                    /* A closed menu has nothing to move. */
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(from_closed.sel == 0);
                    break;
                case ACTMENU_NAVIGATE_BACK:
                    /* Backwards from the top row wraps to the last, which is
                     * the same wrap the forward direction uses. Without this
                     * case the two directions would be indistinguishable and a
                     * NAVIGATE_BACK that silently moved forward would pass. */
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(from_open.open == 1);
                    ASSERT(from_open.sel == from_open.count - 1);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(from_closed.sel == 0);
                    break;
                case ACTMENU_TOGGLE:
                    /* Choosing closes the menu either way: a keypress that
                     * chose nothing must not leave an overlay with no obvious
                     * way back. */
                    ASSERT(got_open == (MenuItem)from_open.items[0]);
                    ASSERT(from_open.open == 0);
                    ASSERT(from_open.sel == 0);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(from_closed.open == 0);
                    break;
                case ACTMENU_CANCEL:
                    ASSERT(got_open == ITEM_COUNT);
                    ASSERT(from_open.open == 0);
                    ASSERT(from_open.sel == 0);
                    ASSERT(got_closed == ITEM_COUNT);
                    ASSERT(from_closed.open == 0);
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
        assert_menu_invariant(&m, running, paused, "TOGGLE on closed");

        /* Also after a real selection: the remembered row must not survive
         * the close. */
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        (void)actions_apply(&m, ACTMENU_TOGGLE, running, paused);
        ASSERT(actions_apply(&m, ACTMENU_TOGGLE, running, paused) == ITEM_COUNT);

        /* And after a cancel from a navigated position. */
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

TEST(actions_navigate_back_moves_the_highlight_the_other_way)
{
    ActionMenu m;
    actions_init(&m);
    actions_apply(&m, ACTMENU_OPEN, 1, 0);
    ASSERT(m.open == 1);
    ASSERT(m.count >= 4);
    int n = m.count;

    int at = m.sel;
    for (int i = 0; i < n; i++) {
        actions_apply(&m, ACTMENU_NAVIGATE_BACK, 1, 0);
        int want = (at + n - 1) % n;
        if (m.sel != want)
            FAIL_MSG("back from %d gave %d, want %d", at, m.sel, want);
        at = m.sel;
    }
    /* A full lap backwards returns to where it started. */
    ASSERT(at == 0);

    /* And the forward direction still steps the opposite way from the top. */
    actions_apply(&m, ACTMENU_NAVIGATE_BACK, 1, 0);
    ASSERT(m.sel == n - 1);
    actions_apply(&m, ACTMENU_NAVIGATE, 1, 0);
    ASSERT(m.sel == 0);
}

TEST(actions_navigate_wraps_and_never_lands_on_a_disabled_row)
{
    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];
        ActionMenu m;
        unsigned char seen[ITEM_COUNT + 1];

        actions_init(&m);
        actions_apply(&m, ACTMENU_OPEN, running, paused);
        ASSERT(m.sel == 0);

        /* Walk the whole list and off the end: a cursor stuck on the last row
         * reads as a dead key, so it wraps to the top. */
        for (int step = 1; step <= m.count; step++) {
            actions_apply(&m, ACTMENU_NAVIGATE, running, paused);
            ASSERT(m.sel == step % m.count);
            assert_menu_invariant(&m, running, paused, "after NAVIGATE");
        }
        ASSERT(m.sel == 0); /* wrapped exactly back to the top */

        /* Every row it can reach is enabled, and it reaches all of them. */
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
                    FAIL_MSG("%s -> %s with sel=%d open=%d count=%d",
                             state_name(k_states[a]), state_name(k_states[b]),
                             m.sel, m.open, m.count);
                actions_apply(&m, ACTMENU_NAVIGATE, k_states[b][0], k_states[b][1]);
                if (!menu_invariant_holds(&m, k_states[b][0], k_states[b][1]))
                    FAIL_MSG("%s -> %s after NAVIGATE, sel=%d count=%d",
                             state_name(k_states[a]), state_name(k_states[b]), m.sel, m.count);
            }
        }
    }
}

TEST(actions_garbage_and_zero_initialised_menus_are_repaired)
{
    /* The view is allowed to hand the model whatever is in the struct. An
     * unclamped sel here indexes items[] out of bounds, so every entry point
     * has to rebuild the fields from the engine state rather than trust them.
     * 0xFF makes `open` truthy, sel enormous, count nonsense and every item
     * slot a value that is not a MenuItem. */
    static const MenuOp ops[] = {
        ACTMENU_NONE, ACTMENU_OPEN, ACTMENU_NAVIGATE, ACTMENU_NAVIGATE_BACK,
        ACTMENU_TOGGLE, ACTMENU_CANCEL
    };

    for (size_t s = 0; s < N_STATES; s++) {
        const int running = k_states[s][0], paused = k_states[s][1];

        for (int fill = 0; fill < 2; fill++) {
            for (size_t o = 0; o < sizeof ops / sizeof ops[0]; o++) {
                ActionMenu m;

                if (fill) memset(&m, 0xFF, sizeof m);
                else       memset(&m, 0x00, sizeof m);

                (void)actions_apply(&m, ops[o], running, paused);
                if (!menu_invariant_holds(&m, running, paused))
                    FAIL_MSG("fill=%d state=%s: sel=%d open=%d count=%d",
                             fill, state_name(k_states[s]), m.sel, m.open, m.count);

                /* `open` is normalised to exactly 0 or 1, so a view branching
                 * on it never sees a stray truthy value. */
                ASSERT(m.open == 0 || m.open == 1);
                ASSERT(m.count >= 0 && m.count <= ITEM_COUNT);
                if (m.open) ASSERT(m.sel >= 0 && m.sel < m.count);
                else        ASSERT(m.sel == 0);
            }
        }
    }
}

TEST(actions_fuzz_60000_navigates_holds_the_invariant_every_time)
{
    /* The property test the module's own header claims is the reason it was
     * split out of the view: hammer the highlight for tens of thousands of
     * keypresses with no screen attached. The engine state is flipped
     * mid-loop on purpose, because a highlight that only ever moves inside a
     * fixed enabled set never has to survive the set shrinking underneath it.
     *
     * Seeded explicitly, so a failure reproduces exactly on CI.
     *
     * The invariant is checked through a non-printing predicate and reported
     * once, at the first violation. The harness prints every failed assertion,
     * and a broken clamp would otherwise produce sixty thousand identical lines
     * that bury the step number which is the only useful part. */
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

        /* One step in four changes the engine underneath the open menu. */
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

    /* Guard against a fuzz that passes because it did nothing. Sixty thousand
     * steps that never move the highlight, or never reach more than one row,
     * satisfy the invariant trivially. */
    ASSERT(STEPS >= 20000);
    ASSERT(moves > STEPS / 2);
    for (int i = 0; i < m.count; i++) ASSERT(seen_sel[i] == 1);

    /* Both enabled-set sizes must have occurred, or the state machine never
     * actually varied and the clamp was never exercised. Idle drops both
     * Resume and Pause (7 items); either running state drops exactly one (8). */
    int distinct_counts = 0;
    for (int i = 0; i <= ITEM_COUNT; i++) if (seen_count[i]) distinct_counts++;
    ASSERT(distinct_counts >= 2);
    ASSERT(seen_count[7] == 1);
    ASSERT(seen_count[8] == 1);
}

TEST(actions_fuzz_all_ops_in_random_order_holds_the_invariant)
{
    /* The navigate-only fuzz cannot catch a clamp that only exists on the
     * NAVIGATE path. This one interleaves every op, including OPEN (which
     * zeroes sel) and TOGGLE/CANCEL (which close the menu), so the highlight
     * is re-entered at every offset in the list and re-validated on every kind
     * of transition. */
    enum { STEPS = 30000 };
    static const MenuOp ops[] = {
        ACTMENU_NONE, ACTMENU_OPEN, ACTMENU_NAVIGATE, ACTMENU_NAVIGATE_BACK,
        ACTMENU_TOGGLE, ACTMENU_CANCEL
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
         * ITEM_COUNT sentinel, and never a row that is not on offer. */
        if (got != ITEM_COUNT) {
            if (op == ACTMENU_TOGGLE && m.count > 0) toggles_with_a_choice++;
            ASSERT(got >= ITEM_RESUME && got < ITEM_COUNT);
            ASSERT(actions_enabled(got, running, paused) == 1);
        }
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
/* Test harness runtime. See test.h for the design notes.
 *
 * Everything here is single threaded and runs after main() starts: the
 * constructors that populate the table fire before main, and cfs_test_run_all
 * walks it afterwards. No locking, because there is no window in which two
 * threads could reach the table.
 *
 * All diagnostics go to stdout rather than stderr. Splitting the streams
 * invites the two halves of a report to interleave wrongly once stdout is
 * redirected to a file, and a test report whose failure lines are detached
 * from their test is close to useless.
 */
#include "test.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct cfs_test_registrar *g_tests;
static int g_test_count;
static int g_test_cap;

static int g_assert_failures;   /* over the whole run */
static int g_cur_failures;      /* within the test currently running */
static const char *g_cur_name;  /* for messages that carry no __FILE__ */

/* Written by the IEEE-754 idiom rather than isnan() so the check survives a
 * toolchain that would route isnan through libm. Note this is also defeated by
 * -ffast-math, which is why the harness must not be built with it: the whole
 * point of the NaN case here is that NaN does not compare equal to itself. */
static int is_nan(double v) { return v != v; }

static int is_inf(double v) { return v == HUGE_VAL || v == -HUGE_VAL; }

void cfs_test_register(const struct cfs_test_registrar *r)
{
    if (g_test_count == g_test_cap) {
        int cap = g_test_cap ? g_test_cap * 2 : 32;
        struct cfs_test_registrar *grown =
            realloc(g_tests, (size_t)cap * sizeof *grown);
        /* There is no useful way to report this from a constructor, and
         * carrying on would silently drop tests and turn the suite green. */
        if (grown == NULL) {
            fputs("cfs_test: out of memory registering tests\n", stderr);
            abort();
        }
        g_tests = grown;
        g_test_cap = cap;
    }
    g_tests[g_test_count++] = *r;
}

static void note_failure(void)
{
    g_cur_failures++;
    g_assert_failures++;
}

static void print_quoted(const char *s)
{
    /* NULL is a real value for ASSERT_STR, not a crash. */
    if (s == NULL) {
        fputs("(null)", stdout);
        return;
    }
    fputc('"', stdout);
    fputs(s, stdout);
    fputc('"', stdout);
}

void cfs_assert_impl(int cond, const char *expr, const char *file, int line)
{
    if (cond) return;
    note_failure();
    printf("  %s:%d: ASSERT(%s) failed\n", file, line, expr);
    fflush(stdout);
}

void cfs_assert_dbl_impl(double got, double want, double tol,
                         const char *expr, const char *file, int line)
{
    int ok;

    if (is_nan(got) || is_nan(want)) {
        /* The plain "fabs(got - want) <= tol" test is true here, because the
         * subtraction yields NaN and every comparison against NaN is false,
         * so the negation would let a pair of NaNs through as a pass. */
        ok = 0;
    } else if (is_inf(got) || is_inf(want)) {
        /* Same trap from the other side: inf - inf is NaN, so an exact
         * infinity match would fail without this branch. */
        ok = (got == want);
    } else {
        /* Compared against both bounds rather than against fabs() so that a
         * negative or NaN tolerance fails instead of passing everything. */
        double d = got - want;
        ok = (d <= tol && d >= -tol);
    }

    if (ok) return;
    note_failure();
    printf("  %s:%d: ASSERT_DBL(%s) failed: got %.10g, want %.10g, tol %.10g\n",
           file, line, expr, got, want, tol);
    fflush(stdout);
}

void cfs_assert_str_impl(const char *got, const char *want,
                         const char *expr, const char *file, int line)
{
    /* NULL == NULL is a legitimate expectation, and one side being NULL is a
     * legitimate bug, so neither is treated as a special error. */
    if (got == want) return;
    if (got != NULL && want != NULL && strcmp(got, want) == 0) return;

    note_failure();
    printf("  %s:%d: ASSERT_STR(%s) failed: got ", file, line, expr);
    print_quoted(got);
    printf(", want ");
    print_quoted(want);
    putchar('\n');
    fflush(stdout);
}

void cfs_fail_msg(const char *fmt, ...)
{
    va_list ap;

    note_failure();
    printf("  FAIL_MSG in %s: ", g_cur_name ? g_cur_name : "(outside a test)");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

int cfs_test_run_all(const char *suite_name)
{
    const char *suite = (suite_name != NULL && *suite_name != '\0') ? suite_name : "tests";
    int passed = 0;
    int failed = 0;
    int i;

    printf("=== %s: %d registered test(s) ===\n", suite, g_test_count);

    /* A suite that found nothing is almost always a link problem or a
     * registration that silently did not happen, and reporting that as a pass
     * would make CI green on a test run that tested nothing. 2 rather than 1
     * so the log distinguishes it from an ordinary assertion failure. */
    if (g_test_count == 0) {
        printf("=== %s: nothing ran, result is meaningless ===\n", suite);
        printf("RESULT: FAIL (no tests registered; is the test object linked in?)\n");
        fflush(stdout);
        return 2;
    }

    for (i = 0; i < g_test_count; i++) {
        const char *name = g_tests[i].name;

        g_cur_name = name;
        g_cur_failures = 0;

        printf("[ RUN      ] %s\n", name);
        fflush(stdout);

        g_tests[i].fn();

        if (g_cur_failures == 0) {
            passed++;
            printf("[      OK ] %s\n", name);
        } else {
            failed++;
            printf("[    FAIL ] %s (%d failed assertion%s)\n",
                   name, g_cur_failures, g_cur_failures == 1 ? "" : "s");
        }
        fflush(stdout);
    }
    g_cur_name = NULL;

    printf("\n");
    printf("=== %s: %d test%s, %d passed, %d failed | %d failed assertion%s ===\n",
           suite, g_test_count, g_test_count == 1 ? "" : "s",
           passed, failed, g_assert_failures, g_assert_failures == 1 ? "" : "s");
    printf("RESULT: %s\n", failed == 0 ? "PASS" : "FAIL");
    fflush(stdout);

    return failed == 0 ? 0 : 1;
}
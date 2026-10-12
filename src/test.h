/* Self-registering test harness.
 *
 * A test declares itself with TEST(name) and is collected through a load time
 * constructor, so adding a test never means editing a list somewhere else.
 * A hand-maintained list is the one part of a test harness that can silently
 * go stale -- a test nobody runs looks exactly like a test that passes.
 *
 * Asserts record a failure and return rather than aborting, so a test reports
 * every problem it finds instead of only the first one it trips over. That
 * matters more here than usual: most of the modules under test do formatting
 * and parsing, where one bad byte usually breaks several separate rules at
 * once.
 *
 * libc only. math.h is included for HUGE_VAL, which is a constant rather than
 * a libm symbol, so no extra link flag is needed.
 *
 * Toolchains: GCC and Clang, including MinGW-w64, are the supported targets
 * and are what CI builds (MSYS2 gcc on Windows, clang on macOS). The MSVC
 * branch below follows the documented .CRT$XCA/.CRT$XCZ mechanism but is not
 * exercised by this repository's CI, so treat it as unverified there.
 */
#ifndef CFS_TEST_H
#define CFS_TEST_H

/* So that ASSERT_STR(p, NULL), the usual shape for an optional return, works
 * in a test file that includes nothing but this header. */
#include <stddef.h>

/* Record emitted by TEST(). The harness copies these by value into its own
 * table rather than threading a linked list through the originals: TEST()
 * declares its registrar `static const`, and once its function pointer has
 * been relocated the linker is free to place it in read-only memory, which
 * turns a write into a fault. Copying also makes the run order deterministic,
 * though it is the linker's constructor order and not source order. */
struct cfs_test_registrar {
    const char *name;
    void (*fn)(void);
};

/* Called from the constructor that TEST() emits. Not for hand use: calling it
 * as well registers the same test twice and it runs twice. */
void cfs_test_register(const struct cfs_test_registrar *r);

/* Compiler glue, kept out of TEST() so the macro reads the same everywhere. */
#if defined(_MSC_VER)
/* MSVC has no __attribute__((constructor)); the documented equivalent is a
 * function pointer parked in the CRT's initialiser section. Two entries are
 * needed because /OPT:REF discards unreferenced statics and only the ".XA"
 * one is the canonical initialiser slot. */
#define CFS_TEST_CTOR(reg)                                          \
    static void cfs_ctor_fn_##reg(void);                            \
    __declspec(allocate(".CRT$XCA")) static void (*cfs_ctor_##reg)(void) = cfs_ctor_fn_##reg;   \
    __declspec(allocate(".CRT$XCZ")) static void (*cfs_ctor_##reg##_z)(void) = cfs_ctor_fn_##reg; \
    static void cfs_ctor_fn_##reg(void) { cfs_test_register(&reg); }
#else
#define CFS_TEST_CTOR(reg)                                          \
    __attribute__((constructor))                                    \
    static void cfs_ctor_##reg(void) { cfs_test_register(&reg); }
#endif

/* Define a test. Registers itself at load; no list to maintain. */
#define TEST(name) \
    static void name(void); \
    static const struct cfs_test_registrar name##_reg = { #name, name }; \
    CFS_TEST_CTOR(name##_reg) \
    static void name(void)

/* Assertions. Each records a failure and CONTINUES so one test reports
   every problem it finds, not just the first. */
void cfs_assert_impl(int cond, const char *expr, const char *file, int line);
void cfs_assert_dbl_impl(double got, double want, double tol,
                         const char *expr, const char *file, int line);
void cfs_assert_str_impl(const char *got, const char *want,
                         const char *expr, const char *file, int line);
void cfs_fail_msg(const char *fmt, ...);

#define ASSERT(cond)          cfs_assert_impl((cond) ? 1 : 0, #cond, __FILE__, __LINE__)
#define ASSERT_DBL(got, want, tol) \
    cfs_assert_dbl_impl((got), (want), (tol), #got, __FILE__, __LINE__)
#define ASSERT_STR(got, want)  cfs_assert_str_impl((got), (want), #got, __FILE__, __LINE__)
#define FAIL_MSG(...)         cfs_fail_msg(__VA_ARGS__)

/* Runs every registered test. Returns the process exit code: 0 only if all
   passed. Prints a summary to stdout. */
int cfs_test_run_all(const char *suite_name);
#endif
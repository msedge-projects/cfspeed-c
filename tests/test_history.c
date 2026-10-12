/* Integration tests for the history round trip: record a run, persist it, read
 * it back.
 *
 * history.c was unit-tested against stubs and cli.c's JSON was checked on its
 * own, but nothing ever proved that a run the app recorded is readable again.
 * The defects this file is here to catch have all been of the same shape: a
 * field the writer emits and the reader quietly drops, which costs nothing at
 * save time and shows up weeks later as a HISTORY column full of "-". The two
 * that actually happened were LatStats.samples (the LOSS column gates on it in
 * views.c line 710) and Throughput.nsamples (DETAILS gates on it), and a third
 * was the two modules spelling the same document two incompatible ways.
 *
 * So the assertions are written field by field, with a name attached to each
 * one, rather than as a single "the struct looks about right" comparison. A
 * round trip that loses one field and keeps the other forty-nine looks exactly
 * like a round trip that works.
 *
 * Nothing here may be "fixed" by relaxing an assertion. If a test in this file
 * fails, history.c is wrong, and the fix belongs in history.c.
 *
 * Sandbox: history_path() has no override and caches on first use, reading
 * LOCALAPPDATA/USERPROFILE on Windows and HOME/XDG_DATA_HOME elsewhere. The
 * suite therefore redirects those variables to a per-process temporary
 * directory before touching history_path() once, and no test ever reads or
 * writes the developer's real history file.
 *
 * No engine_* or net_* stub is needed: history.c calls none of them (verified
 * by grep over the file), so it links against nothing but libc.
 *
 * Build:
 *   cc -std=gnu11 -O2 -Wall -Wextra -o t tests/test_history.c \
 *      src/test.c src/history.c
 */

/* Quoted includes resolve against this file's directory before any -I path is
 * consulted, so the suite builds both from "make test" (no -Isrc) and from a
 * direct command line that passes one. */
#include "../src/history.h"
#include "../src/test.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <process.h>
#  include <windows.h>
#  define CFS_MKDIR(p) _mkdir(p)
#  define CFS_RMDIR(p) _rmdir(p)
#  define CFS_GETPID()  _getpid()
#  define CFS_SEP       '\\'
/* SetEnvironmentVariableA would be the obvious choice here and it is wrong:
   it edits the Win32 environment block, while MinGW's getenv reads the CRT's
   private copy of it, which is snapshotted once at startup. Only _putenv_s
   reaches the same table history.c's getenv will see. */
#  define CFS_SETENV(k, v) _putenv_s((k), (v))
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define CFS_MKDIR(p) mkdir((p), 0777)
#  define CFS_RMDIR(p) rmdir(p)
#  define CFS_GETPID()  ((int)getpid())
#  define CFS_SEP       '/'
#  define CFS_SETENV(k, v) setenv((k), (v), 1)
#endif

/* history.c keeps this private; the tests need the same number. */
#define EDGE_N ((int)(sizeof(((Meta *)0)->edge_ips) / sizeof(((Meta *)0)->edge_ips[0])))

/* Timestamps are chosen so a record's identity can be read off a diff. */
#define TS_BASE 1700000000000ULL

/* ================================================================= sandbox */

static char g_dir[512];
static int  g_sandbox_ready;

static void make_dirs(const char *path)
{
    char buf[512];
    size_t n = strlen(path);
    size_t i = 1;

    if (n == 0 || n >= sizeof buf) return;
    memcpy(buf, path, n + 1);
#if defined(_WIN32)
    if (n >= 3 && buf[1] == ':') i = 3; /* "C:\" must stay in one piece */
#endif
    for (; i < n; i++) {
        if (buf[i] != CFS_SEP || buf[i - 1] == CFS_SEP) continue;
        buf[i] = '\0';
        CFS_MKDIR(buf);
        buf[i] = CFS_SEP;
    }
    buf[n] = '\0';
    CFS_MKDIR(buf);
}

/* Returns 0 if the redirect failed, in which case nothing may be run. */
static int sandbox_init(void)
{
    const char *base = NULL;
#if defined(_WIN32)
    base = getenv("TEMP");
    if (!base || !*base) base = getenv("TMP");
#else
    base = getenv("TMPDIR");
#endif
    if (!base || !*base) base = ".";

    /* Per-process, not randomised: the name only has to be unique between two
     * concurrent runs of this suite, and a deterministic name keeps the output
     * reproducible. */
    snprintf(g_dir, sizeof g_dir, "%s%ccfs_hist_sb%d", base, CFS_SEP, CFS_GETPID());
    make_dirs(g_dir);

    /* base_dir() checks LOCALAPPDATA then USERPROFILE on Windows, and HOME
     * then XDG_DATA_HOME elsewhere. Both names are redirected, because a
     * service account or a stripped-down CI shell may only have one of them
     * and history.c would then fall through to the real home directory. */
#if defined(_WIN32)
    CFS_SETENV("LOCALAPPDATA", g_dir);
    CFS_SETENV("USERPROFILE", g_dir);
#else
    CFS_SETENV("HOME", g_dir);
    CFS_SETENV("XDG_DATA_HOME", g_dir);
#endif

    g_sandbox_ready = 1;

    /* history_path() caches on first call and reads the environment at that
     * moment, so this is the one and only moment at which the redirect can be
     * put in place. Calling it here also means every later call is a lookup of
     * an already-cached path rather than a re-read of the environment. */
    printf("[sandbox] history file: %s\n", history_path());

    /* Refuse to run at all if the redirect did not take. Every test in this
     * file deletes and rewrites whatever history_path() names, so continuing
     * with the developer's real path would destroy their history. Getting this
     * wrong is a silent catastrophe, not a test failure, so it is checked
     * here where no test has run yet. */
    {
        const char *p = history_path();
        size_t n = strlen(g_dir);
        if (p == NULL || strncmp(p, g_dir, n) != 0 || p[n] != CFS_SEP) {
            fprintf(stderr,
                    "FATAL: history_path() is \"%s\", which is not inside the "
                    "test sandbox \"%s\".\n"
                    "Refusing to run: these tests delete and rewrite that "
                    "file.\n",
                    p ? p : "(null)", g_dir);
            return 0;
        }
    }
    return 1;
}

static void sandbox_reset(void)
{
    /* Every test starts from no file at all, which is the same state as a
     * first run: no test can observe what another one wrote. */
    remove(history_path());
}

static void sandbox_teardown(void)
{
    char sub[600];

    if (!g_sandbox_ready) return;
    remove(history_path());
    snprintf(sub, sizeof sub, "%s%ccfspeed", g_dir, CFS_SEP);
    CFS_RMDIR(sub);
    CFS_RMDIR(g_dir);
    printf("[sandbox] removed %s\n", g_dir);
}

/* ============================================================ file helpers */

static void write_bytes(const char *data, size_t n)
{
    FILE *f = fopen(history_path(), "wb");
    if (!f) {
        cfs_fail_msg("could not open %s for writing", history_path());
        return;
    }
    if (n > 0) fwrite(data, 1, n, f);
    fclose(f);
}

static void write_text(const char *s)
{
    write_bytes(s, strlen(s));
}

/* Appends. write_bytes opens "wb", which truncates, so a test that assembles a
 * file out of several pieces needs this rather than a second write_text. */
static void append_text(const char *s)
{
    FILE *f = fopen(history_path(), "ab");
    if (!f) {
        cfs_fail_msg("could not open %s for appending", history_path());
        return;
    }
    fputs(s, f);
    fclose(f);
}

static char *read_all(size_t *len_out)
{
    FILE *f = fopen(history_path(), "rb");
    char *buf;
    size_t len = 0;

    if (!f) return NULL;
    buf = (char *)malloc(1024u * 1024u);
    if (!buf) { fclose(f); return NULL; }
    len = fread(buf, 1, 1024u * 1024u - 1, f);
    fclose(f);
    buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}

static long file_size(void)
{
    FILE *f = fopen(history_path(), "rb");
    long n;

    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    fclose(f);
    return n;
}

/* ============================================================ field diffing */

static int g_fields;   /* fields compared by the current diff */
static int g_diffs;    /* fields that came back different */

static void diff_begin(void)
{
    g_fields = 0;
    g_diffs = 0;
}

static void d_u64(const char *field, unsigned long long got,
                  unsigned long long want)
{
    g_fields++;
    if (got == want) return;
    g_diffs++;
    cfs_fail_msg("field diff: %s = %llu, want %llu", field, got, want);
}

static void d_i64(const char *field, long long got, long long want)
{
    g_fields++;
    if (got == want) return;
    g_diffs++;
    cfs_fail_msg("field diff: %s = %lld, want %lld", field, got, want);
}

/* Exact equality, deliberately. Every value here is one or two decimal places
 * and the writer emits %.4f, so a correct round trip reproduces the very same
 * double bit for bit. A tolerance here would hide exactly the class of defect
 * this file exists to find. */
static void d_dbl(const char *field, double got, double want)
{
    g_fields++;
    if (got == want) return;
    g_diffs++;
    cfs_fail_msg("field diff: %s = %.10g, want %.10g", field, got, want);
}

static void d_str(const char *field, const char *got, const char *want)
{
    g_fields++;
    if (got != NULL && want != NULL && strcmp(got, want) == 0) return;
    g_diffs++;
    cfs_fail_msg("field diff: %s = \"%s\", want \"%s\"",
                 field, got ? got : "(null)", want ? want : "(null)");
}

static void diff_tput(const char *pfx, const Throughput *w, const Throughput *g)
{
    char f[64];

    snprintf(f, sizeof f, "%s.bytes", pfx);
    d_u64(f, (unsigned long long)g->bytes, (unsigned long long)w->bytes);
    snprintf(f, sizeof f, "%s.nsamples", pfx);
    d_i64(f, g->nsamples, w->nsamples);
    snprintf(f, sizeof f, "%s.avg", pfx);
    d_dbl(f, g->avg, w->avg);
    snprintf(f, sizeof f, "%s.p95", pfx);
    d_dbl(f, g->p95, w->p95);
    snprintf(f, sizeof f, "%s.max", pfx);
    d_dbl(f, g->max, w->max);
}

static void diff_lat(const char *pfx, const LatStats *w, const LatStats *g)
{
    char f[64];

    snprintf(f, sizeof f, "%s.avg", pfx);    d_dbl(f, g->avg, w->avg);
    snprintf(f, sizeof f, "%s.min", pfx);    d_dbl(f, g->min, w->min);
    snprintf(f, sizeof f, "%s.med", pfx);    d_dbl(f, g->med, w->med);
    snprintf(f, sizeof f, "%s.p25", pfx);    d_dbl(f, g->p25, w->p25);
    snprintf(f, sizeof f, "%s.p75", pfx);    d_dbl(f, g->p75, w->p75);
    snprintf(f, sizeof f, "%s.p95", pfx);    d_dbl(f, g->p95, w->p95);
    snprintf(f, sizeof f, "%s.p99", pfx);    d_dbl(f, g->p99, w->p99);
    snprintf(f, sizeof f, "%s.max", pfx);    d_dbl(f, g->max, w->max);
    snprintf(f, sizeof f, "%s.jitter", pfx); d_dbl(f, g->jitter, w->jitter);
    snprintf(f, sizeof f, "%s.loss", pfx);   d_dbl(f, g->loss, w->loss);
    snprintf(f, sizeof f, "%s.samples", pfx); d_i64(f, g->samples, w->samples);
}

/* Every field history.c promises to persist. down.samples/up.samples are
 * deliberately absent from the format and are asserted separately. */
static void diff_run(const RunResult *w, const RunResult *g)
{
    char f[64];

    d_u64("timestamp_ms", (unsigned long long)g->timestamp_ms,
          (unsigned long long)w->timestamp_ms);
    d_i64("valid", g->valid, w->valid);

    d_str("meta.colo", g->meta.colo, w->meta.colo);
    d_str("meta.ip", g->meta.ip, w->meta.ip);
    d_str("meta.asn", g->meta.asn, w->meta.asn);
    d_str("meta.asn_org", g->meta.asn_org, w->meta.asn_org);
    d_str("meta.loc", g->meta.loc, w->meta.loc);
    d_str("meta.tls", g->meta.tls, w->meta.tls);
    d_str("meta.tls_cipher", g->meta.tls_cipher, w->meta.tls_cipher);
    d_str("meta.http", g->meta.http, w->meta.http);
    d_str("meta.warp", g->meta.warp, w->meta.warp);
    d_dbl("meta.dns_ms", g->meta.dns_ms, w->meta.dns_ms);
    d_dbl("meta.tls_ms", g->meta.tls_ms, w->meta.tls_ms);
    d_i64("meta.edge_ip_count", g->meta.edge_ip_count, w->meta.edge_ip_count);

    /* All eight slots, not just the first edge_ip_count: a record whose count
     * is right but whose list is shifted or partly unwritten still fails. */
    for (int i = 0; i < EDGE_N; i++) {
        snprintf(f, sizeof f, "meta.edge_ips[%d]", i);
        d_str(f, g->meta.edge_ips[i], w->meta.edge_ips[i]);
    }

    diff_tput("down", &w->down, &g->down);
    diff_tput("up", &w->up, &g->up);
    diff_lat("idle", &w->idle, &g->idle);
    diff_lat("loaded_down", &w->loaded_down, &g->loaded_down);

    d_dbl("bufferbloat_ms", g->bufferbloat_ms, w->bufferbloat_ms);
    d_dbl("stability_cv", g->stability_cv, w->stability_cv);
    d_str("stab_grade", g->stab_grade, w->stab_grade);
    d_str("bloat_grade", g->bloat_grade, w->bloat_grade);
}

/* Prints the verdict for the diff currently in progress and fails the test if
 * anything came back different. This is the line that has to read zero. */
static void diff_report(const char *what)
{
    printf("  field diff (%s): %d fields compared, %d differ\n",
           what, g_fields, g_diffs);
    fflush(stdout);
    ASSERT(g_diffs == 0);
}

/* An assertion that names the field, for the ones a failure has to be
 * identifiable by without reading the source. */
#define FIELD_ASSERT(field, cond)                                        \
    do {                                                                 \
        if (!(cond))                                                     \
            cfs_fail_msg("field lost in round trip: %s (%s)",            \
                         (field), #cond);                                \
    } while (0)

/* ================================================================ fixtures */

static History *hist_new(void)
{
    /* History is a MAX_RUNS array of RunResult by value, several megabytes in
     * all. It goes on the heap or it blows the stack, which is the same reason
     * history.h documents. */
    History *h = (History *)malloc(sizeof *h);
    if (h != NULL) memset(h, 0, sizeof *h);
    return h;
}

/* Every persisted field, with a distinct value in each one. The strings name
 * themselves so a diff reads at a glance, and every run index `i` gets a
 * distinct timestamp and byte count so record order is checkable. */
static void fill_run(RunResult *r, int i)
{
    memset(r, 0, sizeof *r);

    r->timestamp_ms = TS_BASE + (uint64_t)i;
    r->valid = 1;

    snprintf(r->meta.colo,     sizeof r->meta.colo,     "COLOTEST");
    snprintf(r->meta.ip,       sizeof r->meta.ip,       "IPTEST");
    snprintf(r->meta.asn,      sizeof r->meta.asn,      "ASNTEST");
    snprintf(r->meta.asn_org,  sizeof r->meta.asn_org,  "ORGTEST");
    snprintf(r->meta.loc,      sizeof r->meta.loc,      "LOCTEST");
    snprintf(r->meta.tls,      sizeof r->meta.tls,      "TLSTEST");
    snprintf(r->meta.tls_cipher, sizeof r->meta.tls_cipher, "CIPHERTEST");
    snprintf(r->meta.http,     sizeof r->meta.http,     "HTTPTEST");
    snprintf(r->meta.warp,     sizeof r->meta.warp,     "WARPTEST");
    r->meta.dns_ms = 111.1;
    r->meta.tls_ms = 222.2;

    r->meta.edge_ip_count = 3;
    snprintf(r->meta.edge_ips[0], sizeof r->meta.edge_ips[0], "EDGE1-%d", i);
    snprintf(r->meta.edge_ips[1], sizeof r->meta.edge_ips[1], "EDGE2-%d", i);
    snprintf(r->meta.edge_ips[2], sizeof r->meta.edge_ips[2], "EDGE3-%d", i);

    r->down.bytes    = 1111111111ULL + (uint64_t)i * 1000ULL;
    r->down.nsamples = 1111;
    r->down.avg      = 333.3;
    r->down.p95      = 444.4;
    r->down.max      = 555.5;

    r->up.bytes      = 2222222222ULL + (uint64_t)i * 1000ULL;
    r->up.nsamples   = 1122;
    r->up.avg        = 666.6;
    r->up.p95        = 777.7;
    r->up.max        = 888.8;

    r->idle.avg     = 111.1;  r->idle.min     = 122.2;  r->idle.med     = 133.3;
    r->idle.p25     = 144.4;  r->idle.p75     = 155.5;  r->idle.p95     = 166.6;
    r->idle.p99     = 177.7;  r->idle.max     = 188.8;  r->idle.jitter  = 199.9;
    r->idle.loss    = 12.5;   r->idle.samples = 111;

    r->loaded_down.avg     = 211.1;  r->loaded_down.min     = 222.2;
    r->loaded_down.med     = 233.3;  r->loaded_down.p25     = 244.4;
    r->loaded_down.p75     = 255.5;  r->loaded_down.p95     = 266.6;
    r->loaded_down.p99     = 277.7;  r->loaded_down.max     = 288.8;
    r->loaded_down.jitter  = 299.9;  r->loaded_down.loss    = 34.5;
    r->loaded_down.samples = 222;

    r->bufferbloat_ms = 999.9;
    r->stability_cv   = 88.88;
    snprintf(r->stab_grade,   sizeof r->stab_grade,   "A+");
    snprintf(r->bloat_grade,  sizeof r->bloat_grade,  "B+");
}

static void fill_field(char *dst, size_t cap, char c)
{
    if (cap < 2) return;
    for (size_t i = 0; i + 1 < cap; i++) dst[i] = c;
    dst[cap - 1] = '\0';
}

/* A record whose string fields are filled to capacity-1 with a
 * field-specific letter, so an overflow shows up in two places at once: the
 * long field comes back truncated and its neighbour comes back wrong. */
static void fill_max_run(RunResult *r, int i)
{
    fill_run(r, i);

    fill_field(r->meta.colo,       sizeof r->meta.colo,       'C');
    fill_field(r->meta.ip,         sizeof r->meta.ip,         'I');
    fill_field(r->meta.asn,        sizeof r->meta.asn,        'A');
    fill_field(r->meta.asn_org,    sizeof r->meta.asn_org,    'O');
    fill_field(r->meta.loc,        sizeof r->meta.loc,        'L');
    fill_field(r->meta.tls,        sizeof r->meta.tls,        'T');
    fill_field(r->meta.tls_cipher, sizeof r->meta.tls_cipher, 'K');
    fill_field(r->meta.http,       sizeof r->meta.http,       'H');
    fill_field(r->meta.warp,       sizeof r->meta.warp,       'W');
    fill_field(r->stab_grade,      sizeof r->stab_grade,      'S');
    fill_field(r->bloat_grade,     sizeof r->bloat_grade,     'B');

    r->meta.edge_ip_count = EDGE_N;
    for (int k = 0; k < EDGE_N; k++)
        fill_field(r->meta.edge_ips[k], sizeof r->meta.edge_ips[k],
                   (char)('0' + k));
}

/* A bounded appender, so the hand-built records below read as the document
 * they describe instead of as one unbreakable format string. */
typedef struct { char *p; size_t len; size_t cap; int cut; } Buf;

static void app(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->cut) return;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) { b->cut = 1; return; }
    b->len += (size_t)n;
}

/* One record in history.c's own vocabulary, small enough that a few hundred
 * of them fit well under the loader's file cap. The index is baked into
 * timestamp_ms and into every identifying string, so a surviving record can
 * be named exactly. */
static size_t emit_record(char *dst, size_t cap, unsigned idx)
{
    Buf b;
    b.p = dst;
    b.len = 0;
    b.cap = cap;
    b.cut = 0;
    if (cap == 0) return 0;
    dst[0] = '\0';

    app(&b, "{\"timestamp_ms\":%llu,\"valid\":true,",
        (unsigned long long)(TS_BASE + idx));
    app(&b, "\"meta\":{\"colo\":\"R%04u\",\"ip\":\"10.0.%u.%u\","
             "\"asn\":\"AS%04u\",\"asn_org\":\"ORG%04u\",\"loc\":\"LOC%04u\","
             "\"tls\":\"TLS%04u\",\"tls_cipher\":\"CIPHER%04u\","
             "\"http\":\"HTTP%04u\",\"warp\":\"WARP%04u\","
             "\"dns_ms\":11.1100,\"tls_ms\":22.2200,"
             "\"edge_ip_count\":2,\"edge_ips\":[\"EA%04u\",\"EB%04u\"]},",
        idx, (idx / 250u) % 250u, idx % 250u,
        idx, idx, idx, idx, idx, idx, idx, idx);
    app(&b, "\"download\":{\"bytes\":%llu,\"nsamples\":1111,\"avg\":33.3300,"
             "\"p95\":44.4400,\"max\":55.5500},",
        (unsigned long long)(1111111111ULL + idx));
    app(&b, "\"upload\":{\"bytes\":%llu,\"nsamples\":1122,\"avg\":66.6600,"
             "\"p95\":77.7700,\"max\":88.8800},",
        (unsigned long long)(2222222222ULL + idx));
    app(&b, "\"latency\":{");
    app(&b, "\"idle\":{\"avg\":11.1100,\"min\":22.2200,\"med\":33.3300,"
             "\"p25\":44.4400,\"p75\":55.5500,\"p95\":66.6600,"
             "\"p99\":77.7700,\"max\":88.8800,\"jitter\":99.9900,"
             "\"loss\":12.5000,\"samples\":111},");
    app(&b, "\"loaded\":{\"avg\":11.1100,\"min\":22.2200,\"med\":33.3300,"
             "\"p25\":44.4400,\"p75\":55.5500,\"p95\":66.6600,"
             "\"p99\":77.7700,\"max\":88.8800,\"jitter\":99.9900,"
             "\"loss\":34.5000,\"samples\":222}},");
    app(&b, "\"bufferbloat_ms\":99.9900,\"stability_cv\":88.8800,"
             "\"grades\":{\"stability\":\"A\",\"bufferbloat\":\"B\"}}");

    if (b.cut) {
        cfs_fail_msg("emit_record(%u) did not fit in %lu bytes", idx,
                     (unsigned long)cap);
        return 0;
    }
    return b.len;
}

/* ================================================== 1. the round trip itself */

TEST(history_round_trip_every_field)
{
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    fill_run(&w->runs[0], 0);
    w->count = 1;

    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == 1);
    if (g->count == 1) {
        diff_begin();
        diff_run(&w->runs[0], &g->runs[0]);
        diff_report("one fully populated run");
    }

    free(w);
    free(g);
}

/* ============================================ 2. count and order are preserved */

TEST(history_multiple_runs_keep_order_and_count)
{
    const int N = 5;
    History *w = hist_new();
    History *g = hist_new();
    int bad = 0;

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    for (int i = 0; i < N; i++) fill_run(&w->runs[i], i);
    w->count = N;

    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == N);

    for (int i = 0; i < g->count && i < N; i++) {
        char what[32];
        snprintf(what, sizeof what, "run %d of %d", i, N);
        diff_begin();
        diff_run(&w->runs[i], &g->runs[i]);
        /* The diff above compares against w->runs[i]; a history that came back
         * reordered would compare clean against the wrong slot only if w were
         * also reordered, so order is pinned separately on the one field that
         * is unique per record. */
        if (g->runs[i].timestamp_ms != TS_BASE + (uint64_t)i) {
            bad++;
            cfs_fail_msg("run %d came back with timestamp_ms %llu, want %llu "
                         "(history is reordered)",
                         i, (unsigned long long)g->runs[i].timestamp_ms,
                         (unsigned long long)(TS_BASE + (uint64_t)i));
        }
        diff_report(what);
    }
    ASSERT(bad == 0);

    free(w);
    free(g);
}

/* ================================================================= 3. trim */

TEST(history_load_keeps_newest_max_runs)
{
    /* history_save can only ever be handed MAX_RUNS records through the
     * History struct, so a file with more than that has to be written by hand
     * to exercise the loader's trimming. */
    const int TOTAL = 250;
    const int FIRST_KEPT = TOTAL - MAX_RUNS;   /* the oldest record dropped */

    History *g = hist_new();
    char *buf;
    size_t cap = (size_t)TOTAL * 2048u + 64u;
    size_t len = 0;
    int emit_failed = 0;

    if (g == NULL) {
        cfs_fail_msg("out of memory");
        return;
    }
    sandbox_reset();

    buf = (char *)malloc(cap);
    if (buf == NULL) { cfs_fail_msg("out of memory"); free(g); return; }

    len += (size_t)snprintf(buf + len, cap - len, "[\n");
    for (int i = 0; i < TOTAL; i++) {
        size_t n = emit_record(buf + len, cap - len, (unsigned)i);
        if (n == 0) { emit_failed = 1; break; }
        len += n;
        buf[len++] = '\n';
        buf[len] = '\0';
    }
    len += (size_t)snprintf(buf + len, cap - len, "]\n");
    ASSERT(emit_failed == 0);
    if (emit_failed) { free(buf); free(g); return; }

    write_bytes(buf, len);
    free(buf);

    history_load(g);

    /* The headline claim: a file with more records than MAX_RUNS must load as
     * MAX_RUNS records -- the newest MAX_RUNS, not "whatever is left after
     * the oldest MAX_RUNS are thrown away". */
    ASSERT(g->count == MAX_RUNS);

    /* Oldest first, so slot i must be record FIRST_KEPT + i. Reported once
     * with the first divergence and the total, rather than once per slot:
     * fifty identical lines say no more than one that says which fifty. */
    {
        int bad = 0, first_bad = -1, want_first = 0;

        for (int i = 0; i < g->count && i < MAX_RUNS; i++) {
            int want = FIRST_KEPT + i;
            if (g->runs[i].timestamp_ms != TS_BASE + (uint64_t)want) {
                if (first_bad < 0) { first_bad = i; want_first = want; }
                bad++;
            }
        }
        if (bad > 0) {
            cfs_fail_msg(
                "of the %d loaded slots, %d hold the wrong record; first "
                "divergence at slot %d, which holds the file's record %d "
                "(timestamp_ms %llu, colo \"%s\") instead of record %d "
                "(timestamp_ms %llu). history_load kept %d records where it "
                "should keep MAX_RUNS=%d; %d of the %d records in the file "
                "were dropped.",
                g->count, bad, first_bad, first_bad,
                (unsigned long long)g->runs[first_bad].timestamp_ms,
                g->runs[first_bad].meta.colo, want_first,
                (unsigned long long)(TS_BASE + (uint64_t)want_first),
                g->count, MAX_RUNS, TOTAL - g->count, TOTAL);
        }
    }

    /* The newest record must be the last one in the file, and the oldest
     * surviving one must not be the first. */
    if (g->count > 0) {
        ASSERT(g->runs[g->count - 1].timestamp_ms ==
               TS_BASE + (uint64_t)(TOTAL - 1));
        ASSERT(g->runs[0].timestamp_ms != TS_BASE);   /* record 0 was dropped */
    }
    if (g->count == MAX_RUNS) {
        ASSERT(g->runs[0].timestamp_ms == TS_BASE + (uint64_t)FIRST_KEPT);
        ASSERT(g->runs[MAX_RUNS - 1].timestamp_ms ==
               TS_BASE + (uint64_t)(TOTAL - 1));
    }

    free(g);
}

/* The number of record lines in a saved file, and the timestamp_ms of the
 * `which`th of them, counted over the file's own lines rather than through the
 * loader. Tests that check what the writer did must not go through the reader:
 * the reader has trimming behaviour of its own under test. */
static int record_line_count(const char *text)
{
    int n = 0;
    const char *p = text;

    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        size_t k = 0;
        while (k < len && (p[k] == ' ' || p[k] == '\t' || p[k] == '\r')) k++;
        if (k < len && p[k] == '{') n++;
        if (e == NULL) break;
        p = e + 1;
    }
    return n;
}

static long long record_line_ts(const char *text, int which)
{
    const char *p = text;
    int seen = 0;

    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        size_t k = 0;
        while (k < len && (p[k] == ' ' || p[k] == '\t' || p[k] == '\r')) k++;
        if (k < len && p[k] == '{') {
            if (seen == which) {
                static const char KEY[] = "\"timestamp_ms\":";
                const char *t = strstr(p, KEY);
                if (t == NULL) return -1;
                return strtoll(t + (sizeof KEY - 1), NULL, 10);
            }
            seen++;
        }
        if (e == NULL) break;
        p = e + 1;
    }
    return -1;
}

TEST(history_save_trims_an_overfull_history)
{
    /* The save-side half of the same contract: history.h documents the format
     * as "trimmed to the newest MAX_RUNS on save". History.runs is a fixed
     * MAX_RUNS array, so reaching that branch needs a History whose count
     * claims more records than its own array holds, which means a block
     * larger than sizeof(History) viewed as one.
     *
     * Note the one thing this cannot have: History is
     * { RunResult runs[MAX_RUNS]; int count; }, so `count` always occupies the
     * first bytes of runs[MAX_RUNS]. Storing a count of 250 necessarily lands
     * on run 200's Throughput.bytes, and that is a property of the struct
     * rather than of history.c. The test therefore asserts on record count,
     * order and timestamps -- none of which live in that field -- and never on
     * run 200's byte count. */
    const int TOTAL = 250;
    const int DROPPED = TOTAL - MAX_RUNS;
    const size_t PAD = 4096;   /* keeps the struct's alignment intact */
    const size_t BLOCK = PAD + (size_t)TOTAL * sizeof(RunResult) + sizeof(int);

    char *block = (char *)malloc(BLOCK);
    History *w;
    char *text;
    size_t len = 0;
    int nrec;

    if (block == NULL) { cfs_fail_msg("out of memory"); return; }
    memset(block, 0, BLOCK);
    w = (History *)(block + PAD);
    sandbox_reset();

    /* Indexed as char* + i*sizeof(RunResult) rather than w->runs[i]: the index
     * deliberately runs past MAX_RUNS, and a RunResult[200] subscript past its
     * end is undefined and would be diagnosed as such. The arithmetic itself
     * is ordinary and stays inside the one allocation. */
    for (int i = 0; i < TOTAL; i++)
        fill_run((RunResult *)(void *)(block + PAD + (size_t)i * sizeof(RunResult)), i);
    w->count = TOTAL;

    ASSERT(history_save(w) == 0);

    text = read_all(&len);
    if (text == NULL) {
        cfs_fail_msg("could not read the file back");
        free(block);
        return;
    }

    nrec = record_line_count(text);

    /* This fixture claims more records than `runs` can hold, so what is being
     * pinned is the safety property, not the trim: history_save must never walk
     * past the end of the array it was handed. Iterating to `count` in this
     * state read ~1 MB beyond the block in the real struct, where runs is only
     * MAX_RUNS long - a gdb backtrace caught it faulting inside esc().
     *
     * So the window is the tail of what the array actually holds: from
     * count - MAX_RUNS up to the end of the array. For every count this program
     * can produce - main.c refuses to append past MAX_RUNS, and history_load
     * clamps on the way in - this window is the whole array and the two
     * formulations are identical.
     */
    ASSERT(nrec == MAX_RUNS - DROPPED);
    ASSERT(nrec <= MAX_RUNS);

    {
        long long got_first = record_line_ts(text, 0);
        long long got_last  = record_line_ts(text, nrec - 1);
        long long want_first = (long long)(TS_BASE + DROPPED);
        long long want_last  = (long long)(TS_BASE + MAX_RUNS - 1);

        if (got_first != want_first || got_last != want_last)
            cfs_fail_msg("history_save wrote %d records: first is run %lld "
                         "(timestamp_ms %lld, want %lld), last is run %lld "
                         "(timestamp_ms %lld, want %lld)",
                         nrec, got_first - (long long)TS_BASE, got_first,
                         want_first, got_last - (long long)TS_BASE, got_last,
                         want_last);

        ASSERT(got_first == want_first);
        ASSERT(got_last == want_last);
    }

    /* The oldest DROPPED records must not be in the file at all. */
    for (int i = 0; i < DROPPED; i++) {
        char needle[32];
        snprintf(needle, sizeof needle, "\"timestamp_ms\":%llu",
                 (unsigned long long)(TS_BASE + (uint64_t)i));
        if (strstr(text, needle) != NULL) {
            cfs_fail_msg("run %d should have been trimmed but is still in "
                         "the file", i);
            break;
        }
    }

    /* The writer takes a const History and must leave the caller's array
     * exactly as it found it. */
    ASSERT(((RunResult *)(void *)(block + PAD))->timestamp_ms == TS_BASE);
    ASSERT(((RunResult *)(void *)(block + PAD +
           (size_t)(TOTAL - 1) * sizeof(RunResult)))->timestamp_ms ==
           TS_BASE + (uint64_t)(TOTAL - 1));

    free(text);
    free(block);
}

/* ================================================= 4. no file is not an error */

TEST(history_missing_file_is_not_an_error)
{
    History *g = hist_new();
    FILE *probe;

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();

    probe = fopen(history_path(), "rb");
    ASSERT(probe == NULL);          /* the precondition, stated */
    if (probe != NULL) fclose(probe);

    history_load(g);
    ASSERT(g->count == 0);

    /* And the history is still usable afterwards, rather than left in whatever
     * state the failed open put it in. */
    fill_run(&g->runs[0], 0);
    g->count = 1;
    ASSERT(history_save(g) == 0);

    {
        History *again = hist_new();
        if (again == NULL) { cfs_fail_msg("out of memory"); free(g); return; }
        history_load(again);
        ASSERT(again->count == 1);
        free(again);
    }

    free(g);
}

TEST(history_path_points_at_the_test_sandbox)
{
    /* A guard on the guard: if the redirect ever stops working, every other
     * test in this file is about to overwrite the developer's real history. */
    const char *p = history_path();

    ASSERT(p != NULL && *p != '\0');
    if (p == NULL || !*p) return;
    ASSERT(strlen(p) > strlen(g_dir));
    ASSERT(strncmp(p, g_dir, strlen(g_dir)) == 0);
}

/* ================================================== 5. damage is survivable */

TEST(history_empty_file_loads_empty)
{
    History *g = hist_new();

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();
    write_bytes("", 0);

    history_load(g);
    ASSERT(g->count == 0);
    free(g);
}

TEST(history_whitespace_only_file_loads_empty)
{
    History *g = hist_new();

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();
    write_text("   \n\t\r\n  \n");

    history_load(g);
    ASSERT(g->count == 0);
    free(g);
}

TEST(history_invalid_json_loads_empty)
{
    History *g = hist_new();

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();
    /* Not JSON at all, and not even JSON-shaped. */
    write_text("this is not a history file, it is a shopping list\n"
               "milk\nbread\n[unclosed\n\"a stray quote\n");

    history_load(g);
    ASSERT(g->count == 0);
    free(g);
}

TEST(history_empty_array_loads_empty)
{
    History *g = hist_new();

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();
    write_text("[\n]\n");

    history_load(g);
    ASSERT(g->count == 0);
    free(g);
}

TEST(history_truncated_last_record_keeps_the_prefix)
{
    const int N = 3;
    History *w = hist_new();
    History *g = hist_new();
    char *buf;
    size_t len = 0, body_end, last_end, last_start, cut;

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    for (int i = 0; i < N; i++) fill_run(&w->runs[i], i);
    w->count = N;
    ASSERT(history_save(w) == 0);

    /* Cut the real writer's output in half through the last record, the way an
     * interrupted write or a full disk would leave it. */
    buf = read_all(&len);
    if (buf == NULL) {
        cfs_fail_msg("could not read the file back");
        free(w);
        free(g);
        return;
    }
    body_end = len;
    if (body_end >= 1 && buf[body_end - 1] == '\n') body_end--;
    if (body_end >= 1 && buf[body_end - 1] == ']')    body_end--;
    last_end = body_end;
    while (last_end > 0 && buf[last_end - 1] != '\n') last_end--;
    last_start = last_end;
    while (last_start > 0 && buf[last_start - 1] != '\n') last_start--;
    cut = last_start + (last_end - last_start) / 2;
    ASSERT(cut > 0 && cut < len);

    write_bytes(buf, cut);
    free(buf);

    history_load(g);

    /* The intact prefix must survive whole; the torn tail may be dropped or
     * half-recovered, but nothing above it may be affected. */
    ASSERT(g->count >= 2);
    ASSERT(g->count <= N);
    for (int i = 0; i < g->count && i < 2; i++) {
        char what[32];
        snprintf(what, sizeof what, "intact run %d after truncation", i);
        diff_begin();
        diff_run(&w->runs[i], &g->runs[i]);
        diff_report(what);
    }

    free(w);
    free(g);
}

TEST(history_missing_closing_bracket_loads_everything)
{
    const int N = 3;
    History *w = hist_new();
    History *g = hist_new();
    char *buf;
    size_t len = 0;

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    for (int i = 0; i < N; i++) fill_run(&w->runs[i], i);
    w->count = N;
    ASSERT(history_save(w) == 0);

    buf = read_all(&len);
    if (buf == NULL) {
        cfs_fail_msg("could not read the file back");
        free(w);
        free(g);
        return;
    }
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ']')) len--;
    write_bytes(buf, len);
    free(buf);

    /* The trailing "]\n" is not part of any record, so losing it must cost
     * nothing. */
    history_load(g);
    ASSERT(g->count == N);
    for (int i = 0; i < g->count && i < N; i++) {
        char what[32];
        snprintf(what, sizeof what, "run %d, array terminator chopped", i);
        diff_begin();
        diff_run(&w->runs[i], &g->runs[i]);
        diff_report(what);
    }

    free(w);
    free(g);
}

TEST(history_garbage_after_a_good_record_keeps_the_good_one)
{
    const int N = 3;
    History *w = hist_new();
    History *g = hist_new();
    char *buf;
    size_t len = 0, body_end;

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    for (int i = 0; i < N; i++) fill_run(&w->runs[i], i);
    w->count = N;
    ASSERT(history_save(w) == 0);

    buf = read_all(&len);
    if (buf == NULL) {
        cfs_fail_msg("could not read the file back");
        free(w);
        free(g);
        return;
    }
    body_end = len;
    if (body_end >= 1 && buf[body_end - 1] == '\n') body_end--;
    if (body_end >= 1 && buf[body_end - 1] == ']')    body_end--;
    buf[body_end] = '\0';

    /* Three good records, then a torn one, then junk. */
    write_text(buf);
    append_text("\n{\"timestamp_ms\":99,\"meta\":{\"colo\":\"HALF\"\n");
    append_text("<<<binary junk>>>\n");
    free(buf);

    history_load(g);
    ASSERT(g->count >= N);
    for (int i = 0; i < g->count && i < N; i++) {
        char what[32];
        snprintf(what, sizeof what, "run %d ahead of the damage", i);
        diff_begin();
        diff_run(&w->runs[i], &g->runs[i]);
        diff_report(what);
    }

    free(w);
    free(g);
}

TEST(history_braces_inside_strings_do_not_split_records)
{
    History *g = hist_new();

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();

    /* Braces are legal inside a quoted field: an ASN org name or a location is
     * free text the server chose, and history.c writes it through unescaped.
     * A record scanner that looks for the next '{' without skipping strings
     * re-cuts every record boundary after such a field. */
    write_text("[\n"
      "{\"timestamp_ms\":100,\"valid\":true,"
      "\"meta\":{\"colo\":\"S{IN}\",\"asn_org\":\"A\\\"B\\\"{C}\"},"
      "\"latency\":{\"idle\":{\"samples\":7,\"loss\":1.5}},"
      "\"download\":{\"nsamples\":3}},"
      "{\"timestamp_ms\":200,\"valid\":false,\"meta\":{\"colo\":\"XXX\"}},"
      "{\"timestamp_ms\":300,\"valid\":true,\"meta\":{\"colo\":\"ZZZ\"}}\n"
      "]\n");

    history_load(g);
    ASSERT(g->count == 3);
    if (g->count == 3) {
        ASSERT(g->runs[0].timestamp_ms == 100);
        ASSERT_STR(g->runs[0].meta.colo, "S{IN}");
        ASSERT_STR(g->runs[0].meta.asn_org, "A\"B\"{C}");
        ASSERT(g->runs[0].idle.samples == 7);
        ASSERT_DBL(g->runs[0].idle.loss, 1.5, 0.0);
        ASSERT(g->runs[0].down.nsamples == 3);
        ASSERT(g->runs[1].timestamp_ms == 200);
        ASSERT(g->runs[1].valid == 0);
        ASSERT(g->runs[2].timestamp_ms == 300);
        ASSERT_STR(g->runs[2].meta.colo, "ZZZ");
    }
    free(g);
}

/* ============================== 6. the fields the HISTORY view gates on */

TEST(history_view_critical_fields_survive)
{
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    fill_run(&w->runs[0], 0);
    w->count = 1;
    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == 1);
    if (g->count != 1) { free(w); free(g); return; }

    /* Each of these was silently dropped at some point, and each is worth a
     * column in the HISTORY tab. The exact predicate views.c evaluates is
     * asserted after the field itself, so "the value arrived" and "the view
     * can use it" are separately on record. */

    /* views.c:710 -- "if (r->idle.samples > 0) fmt_pct(...) else '-'".
     * Losing samples is why the LOSS column printed '-' for every historical
     * run. */
    FIELD_ASSERT("LatStats.samples (idle)", g->runs[0].idle.samples == 111);
    FIELD_ASSERT("views.c:710 idle.samples > 0", g->runs[0].idle.samples > 0);
    FIELD_ASSERT("LatStats.samples (loaded_down)",
                 g->runs[0].loaded_down.samples == 222);
    FIELD_ASSERT("views.c:500 loaded_down.samples > 0",
                 g->runs[0].loaded_down.samples > 0);

    /* The LOSS column has nothing to print without the percentage too. */
    FIELD_ASSERT("LatStats.loss (idle)", g->runs[0].idle.loss == 12.5);
    FIELD_ASSERT("LatStats.loss (loaded_down)",
                 g->runs[0].loaded_down.loss == 34.5);

    /* views.c:471 / :495 -- the DETAILS section is gated on nsamples. */
    FIELD_ASSERT("Throughput.nsamples (down)", g->runs[0].down.nsamples == 1111);
    FIELD_ASSERT("views.c:471 down.nsamples > 0", g->runs[0].down.nsamples > 0);
    FIELD_ASSERT("Throughput.nsamples (up)", g->runs[0].up.nsamples == 1122);
    FIELD_ASSERT("views.c:495 up.nsamples > 0", g->runs[0].up.nsamples > 0);

    /* views.c:534 prints CIPHER straight out of meta.tls_cipher. */
    FIELD_ASSERT("Meta.tls_cipher",
                 strcmp(g->runs[0].meta.tls_cipher, "CIPHERTEST") == 0);

    /* views.c:424 walks meta.edge_ip_count entries of meta.edge_ips. */
    FIELD_ASSERT("Meta.edge_ip_count", g->runs[0].meta.edge_ip_count == 3);
    FIELD_ASSERT("views.c:424 edge_ip_count > 0",
                 g->runs[0].meta.edge_ip_count > 0);
    FIELD_ASSERT("Meta.edge_ips[0]",
                 strcmp(g->runs[0].meta.edge_ips[0], "EDGE1-0") == 0);
    FIELD_ASSERT("Meta.edge_ips[1]",
                 strcmp(g->runs[0].meta.edge_ips[1], "EDGE2-0") == 0);
    FIELD_ASSERT("Meta.edge_ips[2]",
                 strcmp(g->runs[0].meta.edge_ips[2], "EDGE3-0") == 0);

    /* views.c:514 and :569 branch on r->valid. */
    FIELD_ASSERT("RunResult.valid", g->runs[0].valid == 1);
    FIELD_ASSERT("views.c:514 r->valid", g->runs[0].valid != 0);

    free(w);
    free(g);
}

TEST(history_count_fields_survive_at_their_ceilings)
{
    /* views.c clamps the count it reads to MAX_RUNS, and history.c clamps what
     * it reads to MAX_PROBES / MAX_SAMPLES / the edge array. Exactly at the
     * ceiling has to come back exactly at the ceiling, and one past it has to
     * be clamped rather than wrapped or refused. */
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    fill_run(&w->runs[0], 0);
    w->runs[0].idle.samples        = MAX_PROBES;   /* 400 */
    w->runs[0].loaded_down.samples = MAX_PROBES;
    w->runs[0].down.nsamples       = MAX_SAMPLES;  /* 1200 */
    w->runs[0].up.nsamples         = MAX_SAMPLES;
    w->count = 1;
    ASSERT(history_save(w) == 0);
    history_load(g);
    ASSERT(g->count == 1);
    if (g->count == 1) {
        FIELD_ASSERT("idle.samples at MAX_PROBES",
                     g->runs[0].idle.samples == MAX_PROBES);
        FIELD_ASSERT("loaded_down.samples at MAX_PROBES",
                     g->runs[0].loaded_down.samples == MAX_PROBES);
        FIELD_ASSERT("down.nsamples at MAX_SAMPLES",
                     g->runs[0].down.nsamples == MAX_SAMPLES);
        FIELD_ASSERT("up.nsamples at MAX_SAMPLES",
                     g->runs[0].up.nsamples == MAX_SAMPLES);
    }

    /* One past every ceiling, in the same file. */
    sandbox_reset();
    fill_run(&w->runs[0], 1);
    w->runs[0].idle.samples        = MAX_PROBES + 57;
    w->runs[0].loaded_down.samples = MAX_PROBES + 57;
    w->runs[0].down.nsamples       = MAX_SAMPLES + 57;
    w->runs[0].up.nsamples         = MAX_SAMPLES + 57;
    w->runs[0].meta.edge_ip_count  = EDGE_N + 3;
    w->count = 1;
    ASSERT(history_save(w) == 0);
    history_load(g);
    ASSERT(g->count == 1);
    if (g->count == 1) {
        FIELD_ASSERT("idle.samples clamped to MAX_PROBES",
                     g->runs[0].idle.samples == MAX_PROBES);
        FIELD_ASSERT("loaded_down.samples clamped to MAX_PROBES",
                     g->runs[0].loaded_down.samples == MAX_PROBES);
        FIELD_ASSERT("down.nsamples clamped to MAX_SAMPLES",
                     g->runs[0].down.nsamples == MAX_SAMPLES);
        FIELD_ASSERT("up.nsamples clamped to MAX_SAMPLES",
                     g->runs[0].up.nsamples == MAX_SAMPLES);
        FIELD_ASSERT("edge_ip_count clamped to the array",
                     g->runs[0].meta.edge_ip_count <= EDGE_N);
    }

    free(w);
    free(g);
}

TEST(history_sample_arrays_are_not_persisted)
{
    /* The format deliberately omits Throughput.samples: a thousand doubles a
     * phase would make the file unreadable. What has to survive is the count,
     * and what must not appear is the array itself. */
    History *w = hist_new();
    History *g = hist_new();
    long bytes;

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    fill_run(&w->runs[0], 0);
    for (int i = 0; i < MAX_SAMPLES; i++) {
        w->runs[0].down.samples[i] = 1.0 + (double)i;
        w->runs[0].up.samples[i]   = 2.0 + (double)i;
    }
    w->count = 1;

    ASSERT(history_save(w) == 0);

    bytes = file_size();
    /* 2 x MAX_SAMPLES doubles written in full would be tens of kilobytes for a
     * single record. A few hundred for one record means they are not there. */
    ASSERT(bytes > 0 && bytes < 8192);

    history_load(g);
    ASSERT(g->count == 1);
    if (g->count == 1) {
        FIELD_ASSERT("down.nsamples survives without the array",
                     g->runs[0].down.nsamples == 1111);
        FIELD_ASSERT("up.nsamples survives without the array",
                     g->runs[0].up.nsamples == 1122);
        for (int i = 0; i < MAX_SAMPLES; i++) {
            if (g->runs[0].down.samples[i] != 0.0 || g->runs[0].up.samples[i] != 0.0) {
                cfs_fail_msg("sample arrays came back populated at index %d "
                             "(down=%.4f up=%.4f); the file is carrying "
                             "raw samples", i, g->runs[0].down.samples[i],
                             g->runs[0].up.samples[i]);
                break;
            }
        }
    }

    free(w);
    free(g);
}

/* ======================================================= 7. valid:false ==== */

TEST(history_valid_false_stays_false)
{
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    fill_run(&w->runs[0], 0);
    w->runs[0].valid = 0;       /* a refused run: everything else populated */
    w->count = 1;

    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == 1);
    if (g->count != 1) { free(w); free(g); return; }

    /* A refused run that comes back looking successful is worse than a lost
     * one: views.c branches on this to decide whether to print a result at
     * all. It was a semantic collision before -- "valid" meant "the file
     * parsed" in one module and "the run produced a result" in the other. */
    FIELD_ASSERT("RunResult.valid:false survives", g->runs[0].valid == 0);

    diff_begin();
    diff_run(&w->runs[0], &g->runs[0]);
    diff_report("a valid:false run");

    free(w);
    free(g);
}

TEST(history_record_without_a_valid_field_defaults_to_valid)
{
    /* The other direction: a file written before the field existed. Every run
     * such a file can hold reached the end of a run, so absent means valid,
     * and the run must not be discarded. */
    History *g = hist_new();

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();

    write_text("[\n"
      "{\"timestamp_ms\":555,\"meta\":{\"colo\":\"LHR\"},"
      "\"download\":{\"bytes\":10,\"nsamples\":2,\"avg\":1.5}}\n]\n");

    history_load(g);
    ASSERT(g->count == 1);
    if (g->count == 1) {
        FIELD_ASSERT("absent valid defaults to true", g->runs[0].valid == 1);
        ASSERT(g->runs[0].timestamp_ms == 555);
        ASSERT_STR(g->runs[0].meta.colo, "LHR");
    }
    free(g);
}

TEST(history_valid_false_alongside_valid_true_records)
{
    const int N = 4;
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    for (int i = 0; i < N; i++) {
        fill_run(&w->runs[i], i);
        w->runs[i].valid = (i % 2);
    }
    w->count = N;
    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == N);
    for (int i = 0; i < g->count && i < N; i++) {
        char f[32];
        snprintf(f, sizeof f, "valid[%d]", i);
        FIELD_ASSERT(f, g->runs[i].valid == (i % 2));
    }

    free(w);
    free(g);
}

/* ========================================================== 8. escapes ==== */

TEST(history_strings_with_json_metacharacters_survive)
{
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    fill_run(&w->runs[0], 0);

    /* Quote, backslash, newline, carriage return, tab, backspace, form feed,
     * two control bytes the writer has to escape as \u00xx, and UTF-8 above
     * 0x7F plus a lone high byte. */
    snprintf(w->runs[0].meta.colo, sizeof w->runs[0].meta.colo, "A\"B\\C");
    snprintf(w->runs[0].meta.asn_org, sizeof w->runs[0].meta.asn_org,
             "OR\"G\\TE\nST\rTA\bFB\fC\xC3\xA9\xE2\x82\xAC" "E\xE9Z");
    snprintf(w->runs[0].meta.loc, sizeof w->runs[0].meta.loc, "X\tY\nZ\\W");
    snprintf(w->runs[0].meta.tls_cipher, sizeof w->runs[0].meta.tls_cipher,
             "AEAD-AES128-GCM\tSHA256\"v3\\4");
    snprintf(w->runs[0].meta.warp, sizeof w->runs[0].meta.warp, "on\x01\x02off");
    snprintf(w->runs[0].meta.edge_ips[0], sizeof w->runs[0].meta.edge_ips[0],
             "2001:db8::\"1\"\\2");
    w->count = 1;

    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == 1);
    if (g->count != 1) { free(w); free(g); return; }

    /* Byte for byte, not "looks about right". */
    ASSERT_STR(g->runs[0].meta.colo, "A\"B\\C");
    ASSERT_STR(g->runs[0].meta.asn_org,
               "OR\"G\\TE\nST\rTA\bFB\fC\xC3\xA9\xE2\x82\xAC" "E\xE9Z");
    ASSERT_STR(g->runs[0].meta.loc, "X\tY\nZ\\W");
    ASSERT_STR(g->runs[0].meta.tls_cipher, "AEAD-AES128-GCM\tSHA256\"v3\\4");
    ASSERT_STR(g->runs[0].meta.warp, "on\x01\x02off");
    ASSERT_STR(g->runs[0].meta.edge_ips[0], "2001:db8::\"1\"\\2");

    diff_begin();
    diff_run(&w->runs[0], &g->runs[0]);
    diff_report("strings needing escapes");

    free(w);
    free(g);
}

TEST(history_escaped_record_does_not_disturb_its_neighbours)
{
    /* One record with a torture string next to one without, so a record
     * boundary that is re-cut by an escape loses exactly one of the two. */
    const int N = 3;
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    for (int i = 0; i < N; i++) fill_run(&w->runs[i], i);
    snprintf(w->runs[1].meta.asn_org, sizeof w->runs[1].meta.asn_org,
             "A{\"colo\":\"DECOY\",\"valid\":false},\"timestamp_ms\":1}zzz");
    w->count = N;
    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == N);
    for (int i = 0; i < g->count && i < N; i++) {
        char what[48];
        snprintf(what, sizeof what, "run %d, neighbour carries a decoy", i);
        diff_begin();
        diff_run(&w->runs[i], &g->runs[i]);
        diff_report(what);
    }
    if (g->count == N) {
        ASSERT(g->runs[1].valid == 1);
        ASSERT_STR(g->runs[1].meta.colo, "COLOTEST");
    }

    free(w);
    free(g);
}

/* ================================================= 9. fields at their max == */

TEST(history_strings_at_field_maximum_do_not_overflow)
{
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    /* Every label filled to capacity-1. The writer has to escape each one into
     * a scratch buffer sized from the destination and interpolate it, and the
     * reader has to copy it back into the same-sized destination; either one
     * running past its cap shows up here as a truncated or smeared field. */
    fill_max_run(&w->runs[0], 0);
    w->count = 1;

    ASSERT(history_save(w) == 0);
    history_load(g);
    ASSERT(g->count == 1);
    if (g->count != 1) { free(w); free(g); return; }

    /* The exact lengths first: a record that got dropped or cut would show up
     * as an empty string rather than a short one. */
    ASSERT(strlen(g->runs[0].meta.colo)       == sizeof w->runs[0].meta.colo - 1);
    ASSERT(strlen(g->runs[0].meta.ip)         == sizeof w->runs[0].meta.ip - 1);
    ASSERT(strlen(g->runs[0].meta.asn)        == sizeof w->runs[0].meta.asn - 1);
    ASSERT(strlen(g->runs[0].meta.asn_org)    == sizeof w->runs[0].meta.asn_org - 1);
    ASSERT(strlen(g->runs[0].meta.loc)        == sizeof w->runs[0].meta.loc - 1);
    ASSERT(strlen(g->runs[0].meta.tls)        == sizeof w->runs[0].meta.tls - 1);
    ASSERT(strlen(g->runs[0].meta.tls_cipher) == sizeof w->runs[0].meta.tls_cipher - 1);
    ASSERT(strlen(g->runs[0].meta.http)       == sizeof w->runs[0].meta.http - 1);
    ASSERT(strlen(g->runs[0].meta.warp)       == sizeof w->runs[0].meta.warp - 1);
    ASSERT(strlen(g->runs[0].stab_grade)      == sizeof w->runs[0].stab_grade - 1);
    ASSERT(strlen(g->runs[0].bloat_grade)     == sizeof w->runs[0].bloat_grade - 1);
    for (int k = 0; k < EDGE_N; k++)
        ASSERT(strlen(g->runs[0].meta.edge_ips[k]) ==
               sizeof w->runs[0].meta.edge_ips[k] - 1);

    diff_begin();
    diff_run(&w->runs[0], &g->runs[0]);
    diff_report("every string at capacity-1, eight edge addresses");

    free(w);
    free(g);
}

TEST(history_escaped_strings_at_field_maximum_do_not_overflow)
{
    /* The same, with every character being one that expands on the way out:
     * a quote is two bytes in the file and two on the way back, so a scratch
     * buffer sized from the source length rather than from the destination
     * overflows here and nowhere else. */
    History *w = hist_new();
    History *g = hist_new();

    if (w == NULL || g == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        return;
    }
    sandbox_reset();

    fill_max_run(&w->runs[0], 0);
    for (size_t i = 0; i + 1 < sizeof w->runs[0].meta.asn_org; i++)
        w->runs[0].meta.asn_org[i] = (i % 2 == 0) ? '"' : '\\';
    w->runs[0].meta.asn_org[sizeof w->runs[0].meta.asn_org - 1] = '\0';
    for (size_t i = 0; i + 1 < sizeof w->runs[0].meta.tls_cipher; i++)
        w->runs[0].meta.tls_cipher[i] = '\t';
    w->runs[0].meta.tls_cipher[sizeof w->runs[0].meta.tls_cipher - 1] = '\0';
    w->count = 1;

    ASSERT(history_save(w) == 0);
    history_load(g);
    ASSERT(g->count == 1);
    if (g->count != 1) { free(w); free(g); return; }

    ASSERT(strlen(g->runs[0].meta.asn_org) == sizeof w->runs[0].meta.asn_org - 1);
    ASSERT(strlen(g->runs[0].meta.tls_cipher) == sizeof w->runs[0].meta.tls_cipher - 1);

    diff_begin();
    diff_run(&w->runs[0], &g->runs[0]);
    diff_report("capacity-1 strings of pure quote/backslash/tab");

    free(w);
    free(g);
}

/* ========================================================= 10. clear ====== */

TEST(history_clear_removes_the_file_and_empties_the_struct)
{
    const int N = 3;
    History *w = hist_new();
    History *g = hist_new();
    History *zero = hist_new();
    FILE *probe;

    if (w == NULL || g == NULL || zero == NULL) {
        cfs_fail_msg("out of memory");
        free(w);
        free(g);
        free(zero);
        return;
    }
    sandbox_reset();

    for (int i = 0; i < N; i++) fill_run(&w->runs[i], i);
    w->count = N;
    ASSERT(history_save(w) == 0);

    history_load(g);
    ASSERT(g->count == N);

    ASSERT(history_clear(g) == 0);

    ASSERT(g->count == 0);
    /* Every byte of the struct, not just the count: clear() is documented to
     * zero it, and a count of 0 over stale runs is what a reader would trip
     * on. zero is heap allocated like every other History here -- several
     * megabytes each, far past what a stack frame can hold. */
    ASSERT(memcmp(g, zero, sizeof *g) == 0);

    probe = fopen(history_path(), "rb");
    FIELD_ASSERT("history_clear removed the file", probe == NULL);
    if (probe != NULL) fclose(probe);

    /* And the cleared file really is a first run again, not a stale cache. */
    history_load(g);
    ASSERT(g->count == 0);

    /* Clearing an already-cleared history is not an error. */
    ASSERT(history_clear(g) == 0);
    ASSERT(g->count == 0);

    free(w);
    free(g);
    free(zero);
}

/* ============================== cli.c compatibility: one vocabulary, two files */

TEST(history_loads_a_cli_json_document_verbatim)
{
    /* The document cli.c's --json emits: one line, no array, and carrying four
     * keys history does not store. history.h claims the two writers share one
     * vocabulary, and this is where that claim either holds or does not -- the
     * two modules used to disagree completely, and a disagreement here shows up
     * as a HISTORY tab of empty rows. */
    History *g = hist_new();

    if (g == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();

    write_text("{\"timestamp\":\"2026-01-02T03:04:05Z\","
      "\"timestamp_ms\":1767322445000,"
      "\"mode\":\"full\",\"phase\":\"done\",\"valid\":true,\"error\":\"\","
      "\"meta\":{\"colo\":\"LHR\",\"ip\":\"203.0.113.7\",\"asn\":\"AS13335\","
      "\"asn_org\":\"CLOUDFLARENET\",\"loc\":\"London\",\"tls\":\"TLSv1.3\","
      "\"tls_cipher\":\"AEAD-AES128-GCM-SHA256\",\"http\":\"HTTP/2\","
      "\"warp\":\"off\",\"dns_ms\":12.3456,\"tls_ms\":78.9012,"
      "\"edge_ip_count\":3,\"edge_ips\":[\"198.51.100.1\",\"198.51.100.2\","
      "\"198.51.100.3\"]},"
      "\"download\":{\"avg\":910.5,\"p95\":930.25,\"max\":950.125,"
      "\"bytes\":456789,\"nsamples\":321},"
      "\"upload\":{\"avg\":210.5,\"p95\":230.25,\"max\":250.125,"
      "\"bytes\":65432,\"nsamples\":123},"
      "\"latency\":{\"idle\":{\"avg\":11.5,\"min\":9.25,\"med\":11.0,"
      "\"p25\":10.0,\"p75\":12.0,\"p95\":14.5,\"p99\":18.25,\"max\":21.0,"
      "\"jitter\":1.75,\"loss\":0.5,\"samples\":100},"
      "\"loaded\":{\"avg\":51.5,\"min\":45.25,\"med\":50.0,\"p25\":48.0,"
      "\"p75\":54.0,\"p95\":58.5,\"p99\":61.25,\"max\":66.0,\"jitter\":4.75,"
      "\"loss\":2.5,\"samples\":80}},"
      "\"bufferbloat_ms\":40.0,\"stability_cv\":0.05,"
      "\"grades\":{\"stability\":\"A\",\"bufferbloat\":\"A\"}}\n");

    history_load(g);
    ASSERT(g->count == 1);
    if (g->count != 1) { free(g); return; }

    {
        const RunResult *r = &g->runs[0];
        ASSERT(r->timestamp_ms == 1767322445000ULL);
        ASSERT(r->valid == 1);
        ASSERT_STR(r->meta.colo, "LHR");
        ASSERT_STR(r->meta.ip, "203.0.113.7");
        ASSERT_STR(r->meta.asn, "AS13335");
        ASSERT_STR(r->meta.asn_org, "CLOUDFLARENET");
        ASSERT_STR(r->meta.loc, "London");
        ASSERT_STR(r->meta.tls, "TLSv1.3");
        ASSERT_STR(r->meta.tls_cipher, "AEAD-AES128-GCM-SHA256");
        ASSERT_STR(r->meta.http, "HTTP/2");
        ASSERT_STR(r->meta.warp, "off");
        ASSERT_DBL(r->meta.dns_ms, 12.3456, 0.0);
        ASSERT_DBL(r->meta.tls_ms, 78.9012, 0.0);
        ASSERT(r->meta.edge_ip_count == 3);
        ASSERT_STR(r->meta.edge_ips[0], "198.51.100.1");
        ASSERT_STR(r->meta.edge_ips[2], "198.51.100.3");
        ASSERT(r->down.bytes == 456789);
        ASSERT(r->down.nsamples == 321);
        ASSERT_DBL(r->down.avg, 910.5, 0.0);
        ASSERT_DBL(r->down.p95, 930.25, 0.0);
        ASSERT_DBL(r->down.max, 950.125, 0.0);
        ASSERT(r->up.bytes == 65432);
        ASSERT(r->up.nsamples == 123);
        ASSERT_DBL(r->up.avg, 210.5, 0.0);
        ASSERT(r->idle.samples == 100);
        ASSERT_DBL(r->idle.med, 11.0, 0.0);
        ASSERT_DBL(r->idle.loss, 0.5, 0.0);
        ASSERT(r->loaded_down.samples == 80);
        ASSERT_DBL(r->loaded_down.avg, 51.5, 0.0);
        ASSERT_DBL(r->bufferbloat_ms, 40.0, 0.0);
        ASSERT_DBL(r->stability_cv, 0.05, 0.0);
        ASSERT_STR(r->stab_grade, "A");
        ASSERT_STR(r->bloat_grade, "A");

        /* views.c's own gates, against a record that came from --json. */
        ASSERT(r->idle.samples > 0);
        ASSERT(r->down.nsamples > 0);
        ASSERT(r->up.nsamples > 0);
    }
    free(g);
}

TEST(history_app_record_is_readable_by_the_same_scanner_as_cli_output)
{
    /* The other direction of the same claim, on the real writer's output: a
     * saved history must hold exactly the keys cli.c reads back. Compared as
     * key sets rather than as values, because the claim is about vocabulary. */
    History *w = hist_new();
    char *buf;
    size_t len = 0;

    if (w == NULL) { cfs_fail_msg("out of memory"); return; }
    sandbox_reset();

    fill_run(&w->runs[0], 0);
    w->count = 1;
    ASSERT(history_save(w) == 0);

    buf = read_all(&len);
    if (buf == NULL) { cfs_fail_msg("could not read the file back"); free(w); return; }

    static const char *const REQUIRED[] = {
        "\"timestamp_ms\"", "\"valid\"", "\"meta\"", "\"colo\"", "\"ip\"",
        "\"asn\"", "\"asn_org\"", "\"loc\"", "\"tls\"", "\"tls_cipher\"",
        "\"http\"", "\"warp\"", "\"dns_ms\"", "\"tls_ms\"",
        "\"edge_ip_count\"", "\"edge_ips\"", "\"download\"", "\"upload\"",
        "\"bytes\"", "\"nsamples\"", "\"p95\"", "\"latency\"", "\"idle\"",
        "\"loaded\"", "\"loss\"", "\"samples\"", "\"bufferbloat_ms\"",
        "\"stability_cv\"", "\"grades\"", "\"stability\"", "\"bufferbloat\""
    };
    for (size_t i = 0; i < sizeof REQUIRED / sizeof REQUIRED[0]; i++)
        FIELD_ASSERT(REQUIRED[i], strstr(buf, REQUIRED[i]) != NULL);

    free(buf);
    free(w);
}

/* ================================================================== driver */

int main(void)
{
    int rc;

    /* Before cfs_test_run_all, and therefore before any test can reach
     * history_path(): the redirect has to be in place at the moment the path
     * is first built, because history.c caches it from then on. */
    if (!sandbox_init()) return 2;

    rc = cfs_test_run_all("history");
    sandbox_teardown();
    return rc;
}
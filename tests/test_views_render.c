/* Render tests for the four secondary views: charts, details, history, help.
 *
 * WHY THIS FILE EXISTS
 *
 * ui_draw() once took a `tab` argument and ignored it, so all five tabs
 * rendered identical LIVE content - and every tab was still wired, hotkeyed
 * and clickable, so nothing about the navigation was visibly broken. The
 * failure had no symptom anywhere except in the pixels nobody had looked at.
 *
 * A dispatcher that always calls view_charts(), or a tab index that is off by
 * one, looks exactly the same in code review. It only shows up if you render
 * each tab and compare what comes out. That is what this file does.
 *
 * WHAT IS LINKED, AND WHY
 *
 * engine.c drags in net.c, libcurl and worker threads, so the real Engine is
 * not linkable here. It does not need to be: views.c reaches the engine
 * through exactly ten accessors (listed below) and never touches the struct,
 * so defining those ten against a struct of our own is enough to drive every
 * view from a fixed, deterministic RunResult. No curl, no threads, no clock.
 *
 * The same is true of ui.c: it only ever calls the ten accessors plus the four
 * view_* functions, so with CFS_WITH_UI defined this file also links ui.c and
 * asserts the dispatcher itself - the assertion the whole exercise exists for.
 *
 * THE THREE THINGS THAT ACTUALLY BITE
 *
 *   1. Two views rendering identical text. Asserted pairwise, all six pairs,
 *      with the first differing line reported on a collision.
 *   2. A view that silently draws nothing. Asserted: a blank screen is a
 *      failure, because a blank screen and a crash look the same to a user.
 *   3. A view writing one cell past the end of the buffer. No text comparison
 *      can see that, so the cell buffer is allocated with poisoned guard
 *      regions on BOTH sides and the guards are compared byte-for-byte after
 *      every render at every size - and a negative control proves the canary
 *      can actually trip before the positive result is believed.
 *
 * DETERMINISM
 *
 * Every number is written by hand. No clock, no randomness, no locale: the
 * throughput series are an integer recurrence, the epoch stamps are constants,
 * and the expected "HH:MM:SS" for each stamp is computed with the same
 * localtime() call views.c uses, so the test is correct under any TZ even
 * though main() also pins TZ=UTC0 for reproducible dumps.
 *
 * NOTHING HERE MAY BE "FIXED" BY RELAXING AN ASSERTION. If a test in this file
 * fails, views.c or ui.c is wrong, and the fix belongs in those files.
 *
 * Build (no curl, no threads):
 *   cc -std=gnu11 -O2 -Wall -Wextra -Isrc -o t tests/test_views_render.c \
 *      src/test.c src/views.c src/term.c src/history.c src/fmt.c \
 *      src/chart.c src/stats.c
 *
 *   ...and, to also exercise the dispatcher:
 *   ... -DCFS_WITH_UI ... src/ui.c
 */
#include "../src/test.h"

#include "../src/engine.h"
#include "../src/history.h"
#include "../src/term.h"
#include "../src/theme.h"
#include "../src/views.h"

#ifdef CFS_WITH_UI
#include "../src/ui.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ====================================================== engine stubs ==== */

/* views.c never dereferences an Engine - it only calls these ten. So the whole
 * of the test double is "a RunResult plus a few scalars", and a missing stub
 * shows up as a link error rather than as a subtly wrong render. */
struct Engine {
    RunResult    result;
    Phase        phase;
    uint64_t     elapsed_ms;
    double       instant;
    double       probe_instant;
    const double *down;   int ndown;
    const double *up;     int nup;
    const double *probe;  int nprobe;
    const char   *error;
};

const RunResult *engine_result(const Engine *e) { return &e->result; }
const Meta      *engine_meta(const Engine *e)    { return &e->result.meta; }
uint64_t engine_elapsed_ms(const Engine *e)       { return e->elapsed_ms; }
double   engine_instant(const Engine *e)          { return e->instant; }
double   engine_probe_instant(const Engine *e)    { return e->probe_instant; }
Phase    engine_phase(const Engine *e)            { return e->phase; }
const char *engine_error(const Engine *e)         { return e->error ? e->error : ""; }

const double *engine_down_samples(const Engine *e, int *n) {
    *n = e->ndown; return e->down;
}
const double *engine_up_samples(const Engine *e, int *n) {
    *n = e->nup; return e->up;
}
const double *engine_probe_samples(const Engine *e, int *n) {
    *n = e->nprobe; return e->probe;
}

/* Copied from engine.c so the CHARTS title strip reads the same here as it
 * does in the binary. */
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

/* ========================================================== fixtures ==== */

/* An integer recurrence, not sin(): no libm, and identical on every machine. */
#define SERIES_N 240

static double g_down[SERIES_N];
static double g_up[SERIES_N];
static double g_probe[SERIES_N / 2];

static void series_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < SERIES_N; i++) {
        g_down[i]  = 300.0 + (double)((i * 37) % 260);
        g_up[i]    =  40.0 + (double)((i * 53) % 95);
    }
    for (int i = 0; i < SERIES_N / 2; i++)
        g_probe[i] = 8.0 + (double)((i * 11) % 60);
}

/* Fixed Unix epoch milliseconds. Three dates months apart, so the rendered
 * HH:MM:SS differs under any timezone offset, including half-hour and DST
 * zones. The expected strings are derived at runtime (see stamp_of), never
 * hardcoded, because localtime() makes a hardcoded wall clock wrong outside
 * the machine that wrote it.
 *   0: 2024-01-15T12:34:56Z
 *   1: 2024-03-20T08:00:00Z
 *   2: 2024-11-05T23:59:59Z */
#define T_RUN0 1705322096000ull
#define T_RUN1 1710921600000ull
#define T_RUN2 1730851199000ull

static void stamp_of(uint64_t epoch_ms, char *out, size_t cap)
{
    time_t secs = (time_t)(epoch_ms / 1000ull);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &secs);
#else
    localtime_r(&secs, &tmv);
#endif
    snprintf(out, cap, "%02d:%02d:%02d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static void meta_set(Meta *m, const char *ip, const char *colo,
                     const char *org)
{
    memset(m, 0, sizeof *m);
    snprintf(m->ip,       sizeof m->ip,       "%s", ip);
    snprintf(m->colo,     sizeof m->colo,     "%s", colo);
    snprintf(m->loc,      sizeof m->loc,      "%s", "Testville, ZZ");
    snprintf(m->tls,      sizeof m->tls,      "%s", "TLSv1.3");
    snprintf(m->tls_cipher, sizeof m->tls_cipher, "%s", "TESTCIPHER-AEAD");
    snprintf(m->http,     sizeof m->http,     "%s", "HTTP/3");
    snprintf(m->warp,     sizeof m->warp,     "%s", "off");
    /* Bare number, no "AS" prefix: trace reports "13335" and both views.c and
 * cli.c add the prefix themselves. */
    snprintf(m->asn,      sizeof m->asn,      "%s", "64512");
    snprintf(m->asn_org,  sizeof m->asn_org,  "%s", org);
    snprintf(m->edge_ips[0], sizeof m->edge_ips[0], "%s", "192.0.2.10:443");
    snprintf(m->edge_ips[1], sizeof m->edge_ips[1], "%s", "192.0.2.11:443");
    snprintf(m->edge_ips[2], sizeof m->edge_ips[2], "%s", "[2606:4700::1111]:443");
    m->edge_ip_count = 3;
    m->dns_ms = 11.0;
    m->tls_ms = 44.0;
}

/* A complete RunResult: every field views.c reads is populated with a value
 * that cannot be mistaken for a default. */
static void result_set(RunResult *r, const char *ip, const char *colo,
                       const char *org, uint64_t stamp)
{
    memset(r, 0, sizeof *r);

    r->idle.avg = 9.75; r->idle.min = 6.50; r->idle.med = 9.25;
    r->idle.p25 = 8.00; r->idle.p75 = 10.50; r->idle.p95 = 14.00;
    r->idle.p99 = 17.25; r->idle.max = 21.00; r->idle.jitter = 2.75;
    r->idle.loss = 0.50; r->idle.samples = 120;

    r->loaded_down.avg = 38.00; r->loaded_down.min = 27.50;
    r->loaded_down.med = 37.25; r->loaded_down.p95 = 55.00;
    r->loaded_down.p99 = 61.50; r->loaded_down.max = 68.00;
    r->loaded_down.jitter = 5.25; r->loaded_down.loss = 0.25;
    r->loaded_down.samples = 120;

    r->down.bytes = 733003776ull;    /* 699.0 MiB */
    r->down.avg = 487.25; r->down.p95 = 512.00; r->down.max = 561.50;
    for (int i = 0; i < SERIES_N; i++) r->down.samples[i] = g_down[i];
    r->down.nsamples = SERIES_N;

    r->up.bytes = 94371840ull;      /* 90.0 MiB */
    r->up.avg = 92.50; r->up.p95 = 101.00; r->up.max = 118.25;
    for (int i = 0; i < SERIES_N; i++) r->up.samples[i] = g_up[i];
    r->up.nsamples = SERIES_N;

    r->bufferbloat_ms = 28.0;
    r->stability_cv = 3.50;
    snprintf(r->stab_grade,  sizeof r->stab_grade,  "%s", "A");
    snprintf(r->bloat_grade, sizeof r->bloat_grade, "%s", "B");
    meta_set(&r->meta, ip, colo, org);
    r->timestamp_ms = stamp;
    r->valid = 1;
}

/* MARKER_A is what test 3 asserts on: three strings that appear nowhere else
 * in the program, so finding them on screen can only mean DETAILS read the
 * struct rather than printing a label. MARKER_B is the negative control. */
#define IP_A      "1.2.3.4"
#define COLO_A    "COLOTEST"
#define ORG_A     "ORGTEST"
#define IP_B      "9.8.7.6"
#define COLO_B    "COLOBEE"
#define ORG_B     "ORGBEE"

static void engine_init(Engine *e, const char *ip, const char *colo,
                        const char *org)
{
    series_init();
    memset(e, 0, sizeof *e);
    result_set(&e->result, ip, colo, org, T_RUN0);
    e->phase         = PH_DONE;
    e->elapsed_ms    = 754000ull;          /* 12:34 -> stable title strip */
    e->instant       = 505.25;
    e->probe_instant = 13.50;
    e->down   = g_down;   e->ndown   = SERIES_N;
    e->up     = g_up;     e->nup     = SERIES_N;
    e->probe  = g_probe;  e->nprobe  = SERIES_N / 2;
    e->error  = "";
}

/* Three runs, distinguishable by throughput AND by loss, because either one
 * alone would let a view pass while printing the wrong run's numbers. */
static void history_seed(History *h)
{
    RunResult *r;

    memset(h, 0, sizeof *h);
    h->count = 3;

    r = &h->runs[0];
    result_set(r, IP_A, COLO_A, ORG_A, T_RUN0);
    r->down.avg = 111.0; r->down.max = 120.0;
    r->up.avg   =  44.5;
    r->idle.med =  7.25; r->idle.jitter = 1.25;
    r->idle.samples = 10; r->idle.loss = 0.50;
    r->stab_grade[0] = '\0'; r->bloat_grade[0] = '\0';

    r = &h->runs[1];
    result_set(r, IP_A, COLO_A, ORG_A, T_RUN1);
    r->down.avg = 222.0; r->down.max = 240.0;
    r->up.avg   =  55.5;
    r->idle.med = 12.50; r->idle.jitter = 2.50;
    r->idle.samples = 20; r->idle.loss = 1.25;

    r = &h->runs[2];
    result_set(r, IP_A, COLO_A, ORG_A, T_RUN2);
    r->down.avg = 333.0; r->down.max = 360.0;
    r->up.avg   =  66.5;
    r->idle.med = 31.25; r->idle.jitter = 3.75;
    r->idle.samples = 30; r->idle.loss = 2.75;

    /* The canary in the array itself. runs[3] is beyond count, so hist_run()
     * must never hand it out. A view that indexed past count prints "999M",
     * and this file asserts that it does not. */
    result_set(&h->runs[3], IP_B, COLO_B, ORG_B, T_RUN2);
    h->runs[3].down.avg = 999.0;
    h->runs[3].down.max = 999.0;
}

/* History is ~3.9 MB of RunResult by value. Never a local, never the stack:
 * history.h says so and it is true. */
static History *history_alloc(void)
{
    History *h = (History *)calloc(1, sizeof *h);
    if (h) history_seed(h);
    return h;
}

/* ======================================================== guarded screen == */

/* Poison on both sides, not just the tail: a view that indexes with a negative
 * row writes *below* the buffer and a trailing-only canary would call that
 * clean. */
#define GUARD_CELLS 32
#define GUARD_BYTE  0xA5

typedef struct {
    Screen  s;          /* s.cells points into base, at offset `lead` */
    Cell   *base;
    size_t  lead;       /* GUARD_CELLS */
    size_t  ncells;     /* w * h */
    size_t  total;      /* lead + ncells + GUARD_CELLS */
} GScreen;

static int gscreen_open(GScreen *g, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    g->total  = (size_t)GUARD_CELLS + (size_t)w * (size_t)h + GUARD_CELLS;
    g->lead   = GUARD_CELLS;
    g->ncells = (size_t)w * (size_t)h;
    g->base   = (Cell *)malloc(g->total * sizeof(Cell));
    if (!g->base) return 0;
    memset(g->base, GUARD_BYTE, g->total * sizeof(Cell));
    g->s.w = w;
    g->s.h = h;
    g->s.cells = g->base + g->lead;
    screen_clear(&g->s);
    return 1;
}

static void gscreen_close(GScreen *g)
{
    free(g->base);
    g->base = NULL;
    g->s.cells = NULL;
}

/* Returns the offset of the first poisoned byte that moved, or -1 for clean.
 * Reported as a byte offset rather than a bool so a failure says how far off
 * the end somebody actually went. */
static long gscreen_guard_damage(const GScreen *g)
{
    size_t i;
    for (i = 0; i < g->lead; i++)
        if ((unsigned char)g->base[i].utf8[0] != GUARD_BYTE)
            return -(long)(g->lead - i);
    for (i = g->lead + g->ncells; i < g->total; i++)
        if ((unsigned char)g->base[i].utf8[0] != GUARD_BYTE)
            return (long)(i - g->lead - g->ncells + 1);
    /* Whole-struct compare as well: the utf8 byte probe above only checks the
     * first byte of each Cell, which is enough to catch a write but not to
     * prove the guard region is byte-for-byte identical. */
    for (i = 0; i < g->lead; i++) {
        unsigned char raw[sizeof(Cell)];
        memset(raw, GUARD_BYTE, sizeof raw);
        if (memcmp(&g->base[i], raw, sizeof(Cell)) != 0)
            return -(long)(g->lead - i);
    }
    for (i = g->lead + g->ncells; i < g->total; i++) {
        unsigned char raw[sizeof(Cell)];
        memset(raw, GUARD_BYTE, sizeof raw);
        if (memcmp(&g->base[i], raw, sizeof(Cell)) != 0)
            return (long)(i - g->lead - g->ncells + 1);
    }
    return -1;
}

/* A view must not be able to change the geometry the dispatcher gave it. */
static int gscreen_geometry_intact(const GScreen *g, int w, int h)
{
    return g->s.w == w && g->s.h == h &&
           g->s.cells == g->base + g->lead;
}

/* ============================================================ text dump == */

/* Each cell folds to exactly one ASCII byte, so column N is byte N and a
 * rendered screen can be compared, sliced and eyeballed as a C string. The
 * box-drawing and block glyphs become their obvious ASCII stand-ins. */
static char cell_ascii(const Cell *c)
{
    unsigned char b0 = (unsigned char)c->utf8[0];
    unsigned char b1 = (unsigned char)c->utf8[1];
    unsigned char b2 = (unsigned char)c->utf8[2];

    if (b0 == 0x00) return ' ';
    if (b0 < 0x80)  return (char)b0;

    /* U+2588 FULL BLOCK    -> '#' */
    if (b0 == 0xE2 && b1 == 0x96 && b2 == 0x88) return '#';
    /* U+2500 BOX DRAWINGS LIGHT HORIZONTAL -> '-' */
    if (b0 == 0xE2 && b1 == 0x94 && b2 == 0x80) return '-';
    /* U+2502 BOX DRAWINGS LIGHT VERTICAL   -> '|' */
    if (b0 == 0xE2 && b1 == 0x94 && b2 == 0x82) return '|';
    /* U+2193 DOWN ARROW -> 'v'   U+2191 UP ARROW -> '^' */
    if (b0 == 0xE2 && b1 == 0x86 && b2 == 0x93) return 'v';
    if (b0 == 0xE2 && b1 == 0x86 && b2 == 0x91) return '^';
    /* U+00B7 MIDDLE DOT -> '.'   U+2026 HORIZONTAL ELLIPSIS -> '.' */
    if (b0 == 0xC2 && b1 == 0xB7) return '.';
    if (b0 == 0xE2 && b1 == 0x80 && b2 == 0xA6) return '.';
    return '?';
}

#define TEXT_CAP (128 * 1024)

/* Every screen becomes one '\n'-separated row per screen row, emitted at its
 * true column positions with trailing spaces stripped. That keeps byte N of
 * row i on screen column N, so a slice like line[40..46) really is the LOSS
 * column, while the rows stay directly comparable with strcmp. */
static void screen_rows(const Screen *s, char *out, size_t cap)
{
    size_t n = 0;
    for (int y = 0; y < s->h; y++) {
        int last = 0;
        for (int x = 0; x < s->w; x++)
            if (cell_ascii(&s->cells[(size_t)y * s->w + x]) != ' ') last = x + 1;
        for (int x = 0; x < last; x++) {
            if (n + 2 >= cap) break;
            out[n++] = cell_ascii(&s->cells[(size_t)y * s->w + x]);
        }
        if (y + 1 < s->h && n + 1 < cap) out[n++] = '\n';
    }
    if (n < cap) out[n] = '\0'; else if (cap) out[cap - 1] = '\0';
}

static int screen_ink(const Screen *s)
{
    int n = 0;
    for (int i = 0; i < s->w * s->h; i++)
        if (cell_ascii(&s->cells[i]) != ' ') n++;
    return n;
}

static int screen_blank(const Screen *s) { return screen_ink(s) == 0; }

/* Blank-screen check, factored so every test that renders a view gets it. A
 * view that silently draws nothing is worse than one that crashes: the user
 * sees a well-formed empty tab and concludes there is nothing to report. */
static void assert_not_blank(const Screen *s, const char *what)
{
    if (screen_blank(s))
        FAIL_MSG("%s drew a completely blank %dx%d screen", what, s->w, s->h);
}

/* Row `idx` of a rows-dump, into `out`. Returns 0 when the row does not exist. */
static int row_text(const char *rows, int idx, char *out, size_t cap)
{
    const char *p = rows;
    for (int i = 0; i < idx; i++) {
        const char *nl = strchr(p, '\n');
        if (!nl) { if (cap) out[0] = '\0'; return 0; }
        p = nl + 1;
    }
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/* The six pairs, in a fixed order, so the report is the same run to run. */
typedef void (*ViewFn)(Screen *, const Engine *, const History *);

static void hist_wrapper(Screen *s, const Engine *e, const History *h)
{
    view_history(s, e, h, 0);
}

static const struct {
    const char *name;
    ViewFn      fn;
    const char *title;
} VIEWS[4] = {
    { "CHARTS",  view_charts,  "CHARTS"  },
    { "DETAILS", view_details, "DETAILS" },
    { "HISTORY", hist_wrapper, "HISTORY" },
    { "HELP",    view_help,    "HELP"    },
};

/* Renders one view into a fresh guarded screen and returns the text. The
 * caller owns the returned buffer. */
static char *render(GScreen *g, ViewFn fn, const Engine *e, const History *h,
                    int w, int h2, int sel)
{
    char *buf;
    if (!gscreen_open(g, w, h2)) return NULL;
    if (sel >= 0) view_history(&g->s, e, h, sel);
    else          fn(&g->s, e, h);
    buf = (char *)malloc(TEXT_CAP);
    if (buf) screen_rows(&g->s, buf, TEXT_CAP);
    return buf;
}

static void guard_or_fail(GScreen *g, const char *what, int w, int h)
{
    long dmg;
    ASSERT(gscreen_geometry_intact(g, w, h));
    dmg = gscreen_guard_damage(g);
    if (dmg == -1) return;
    if (dmg > 0)
        FAIL_MSG("%s at %dx%d wrote %ld cells PAST the end of the buffer",
                 what, w, h, dmg);
    else
        FAIL_MSG("%s at %dx%d wrote %ld cells BEFORE the start of the buffer",
                 what, w, h, -dmg);
}

/* ================================================== 1. distinguishable ==== */

/* Reports a collision between two rendered screens and fails the test.
 *
 * A collision is the exact bug class this file targets: a tab that is wired,
 * hotkeyed and clickable, drawing somebody else's content. On a collision the
 * useful thing to print is evidence - the first line on which the two differ,
 * or, when they never differ, the first line with any ink on it, because that
 * is what the reader has to look at to see which view is in both places.
 */
static void report_collision(const char *label, const char *na,
                             const char *txt_a, const char *nb,
                             const char *txt_b)
{
    int line = 1, found = 0;
    char la[TEXT_CAP], lb[TEXT_CAP];

    while (row_text(txt_a, line - 1, la, sizeof la)) {
        if (!row_text(txt_b, line - 1, lb, sizeof lb)) break;
        if (strcmp(la, lb) != 0) { found = 1; break; }
        line++;
    }
    if (found) {
        FAIL_MSG("%s: %s and %s COLLIDE; first differing line is line %d:\n"
                 "      %-58s | %s", label, na, nb, line, la, lb);
        return;
    }

    /* Identical throughout. Walk to the first row with ink; if the whole
     * screen is blank there is nothing to quote and saying so is the point. */
    line = 1;
    while (row_text(txt_a, line - 1, la, sizeof la)) {
        if (la[0] != '\0') break;
        line++;
    }
    if (la[0] == '\0')
        FAIL_MSG("%s: %s and %s are byte-identical AND both blank - neither "
                 "view drew anything", label, na, nb);
    else
        FAIL_MSG("%s: %s and %s are byte-identical over the whole screen; "
                 "first row with ink is line %d:\n      %s",
                 label, na, nb, line, la);
}

TEST(views_render_distinct_from_each_other)
{
    static const int SIZES[][2] = { {120, 40}, {100, 30}, {200, 60} };
    Engine e;
    History *h = history_alloc();
    char *text[4];

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    for (unsigned si = 0; si < sizeof SIZES / sizeof SIZES[0]; si++) {
        int w = SIZES[si][0], hh = SIZES[si][1];

        for (int i = 0; i < 4; i++) {
            GScreen g;
            text[i] = render(&g, VIEWS[i].fn, &e, h, w, hh, -1);
            guard_or_fail(&g, VIEWS[i].name, w, hh);
            gscreen_close(&g);
            ASSERT(text[i] != NULL);
        }

        for (int a = 0; a < 4; a++) {
            for (int b = a + 1; b < 4; b++) {
                if (!text[a] || !text[b]) continue;
                if (strcmp(text[a], text[b]) != 0) continue;
                {
                    char label[32];
                    snprintf(label, sizeof label, "%dx%d", w, hh);
                    report_collision(label, VIEWS[a].name, text[a],
                                     VIEWS[b].name, text[b]);
                }
            }
        }

        printf("    pairwise %dx%d: ", w, hh);
        for (int a = 0; a < 4; a++)
            for (int b = a + 1; b < 4; b++)
                printf("%s/%s=%s ", VIEWS[a].name, VIEWS[b].name,
                       (text[a] && text[b] &&
                        strcmp(text[a], text[b]) != 0) ? "DIFF" : "SAME");
        printf("\n");

        for (int i = 0; i < 4; i++) free(text[i]);
    }
    free(h);
}

/* ========================================================= 2. non-empty == */

TEST(each_view_draws_something)
{
    Engine e;
    History *h = history_alloc();

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    for (int i = 0; i < 4; i++) {
        GScreen g;
        char rows[TEXT_CAP];
        int ink;

        ASSERT(gscreen_open(&g, 120, 40));
        VIEWS[i].fn(&g.s, &e, h);
        assert_not_blank(&g.s, VIEWS[i].name);
        ink = screen_ink(&g.s);
        screen_rows(&g.s, rows, sizeof rows);

        printf("    %-8s ink=%d cells, row0=\"%.90s\"\n",
               VIEWS[i].name, ink, rows);

        if (ink == 0)
            FAIL_MSG("%s drew a completely blank 120x40 screen", VIEWS[i].name);
        ASSERT(ink >= 5);
        ASSERT(strstr(rows, VIEWS[i].title) != NULL);
        guard_or_fail(&g, VIEWS[i].name, 120, 40);
        gscreen_close(&g);
    }
    free(h);
}

/* ======================================================== 3. DETAILS ==== */

TEST(details_shows_values_read_from_the_struct)
{
    Engine a, b;
    History *h = history_alloc();
    GScreen g;
    char ta[TEXT_CAP], tb[TEXT_CAP];

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&a, IP_A, COLO_A, ORG_A);
    engine_init(&b, IP_B, COLO_B, ORG_B);

    ASSERT(gscreen_open(&g, 120, 40));
    view_details(&g.s, &a, h);
    screen_rows(&g.s, ta, sizeof ta);
    guard_or_fail(&g, "DETAILS", 120, 40);
    gscreen_close(&g);

    ASSERT(gscreen_open(&g, 120, 40));
    view_details(&g.s, &b, h);
    screen_rows(&g.s, tb, sizeof tb);
    guard_or_fail(&g, "DETAILS", 120, 40);
    gscreen_close(&g);

    /* The three markers exist nowhere else in the program. */
    ASSERT(strstr(ta, COLO_A) != NULL);
    ASSERT(strstr(ta, ORG_A)  != NULL);
    ASSERT(strstr(ta, IP_A)   != NULL);

    /* Negative control. Without this the three assertions above could be
     * satisfied by three hardcoded literals that never change. */
    ASSERT(strstr(tb, COLO_B) != NULL);
    ASSERT(strstr(tb, ORG_B)  != NULL);
    ASSERT(strstr(tb, IP_B)   != NULL);
    ASSERT(strstr(tb, COLO_A) == NULL);
    ASSERT(strstr(tb, ORG_A)  == NULL);
    ASSERT(strstr(tb, IP_A)   == NULL);

    /* Also the measurement side, from the same struct. */
    ASSERT(strstr(ta, "487") != NULL);   /* down.avg  487.25 */
    ASSERT(strstr(ta, "92.5") != NULL);  /* up.avg    92.50 */
    ASSERT(strstr(ta, "9.25ms") != NULL);/* idle.med   9.25 */
    ASSERT(strstr(ta, "28ms") != NULL);  /* bufferbloat 28.0 */
    free(h);
}

/* ======================================================= 4-6. HISTORY == */

/* Column geometry copied from views.c. Duplicated rather than included so that
 * a change to the layout shows up here as a failure instead of silently
 * moving the goalposts. */
enum {
    H_TIME_X = 0,  H_TIME_W = 8,
    H_DOWN_X = 8,  H_DOWN_W = 8,
    H_LOSS_X = 40, H_LOSS_W = 6,
    H_BF_X = 46,   H_BF_W = 6
};

/* `count` columns of `line`, starting at column `x`, trimmed. */
static void slice(const char *line, int x, int count, char *out, size_t cap)
{
    size_t n = 0;
    size_t len = strlen(line);
    for (int i = 0; i < count; i++) {
        size_t idx = (size_t)x + (size_t)i;
        char c = idx < len ? line[idx] : ' ';
        if (c != ' ' && n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n] = '\0';
}

TEST(history_shows_every_stored_run_and_a_real_loss_figure)
{
    Engine e;
    History *h = history_alloc();
    GScreen g;
    char rows[TEXT_CAP], line[TEXT_CAP], col[TEXT_CAP], want[32];

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    ASSERT(gscreen_open(&g, 120, 40));
    view_history(&g.s, &e, h, 0);
    screen_rows(&g.s, rows, sizeof rows);
    guard_or_fail(&g, "HISTORY", 120, 40);
    gscreen_close(&g);

    ASSERT(strstr(rows, "111M") != NULL);
    ASSERT(strstr(rows, "222M") != NULL);
    ASSERT(strstr(rows, "333M") != NULL);

    /* Runs[3] sits past count and holds 999.0 Mbps. If it ever reaches the
     * screen, the view is indexing past the end of the log. */
    ASSERT(strstr(rows, "999M") == NULL);

    /* Run i is drawn at VIEWS_BODY_TOP + 1 + i. */
    for (int i = 0; i < 3; i++) {
        static const double loss[3] = { 0.50, 1.25, 2.75 };
        static const char  *down[3] = { "111M", "222M", "333M" };
        static const uint64_t stamp[3] = { T_RUN0, T_RUN1, T_RUN2 };

        ASSERT(row_text(rows, 3 + i, line, sizeof line));

        if (strstr(line, down[i]) == NULL)
            FAIL_MSG("run %d: down.avg marker \"%s\" missing from row %d",
                     i, down[i], 3 + i);

        /* THE PINNED BUG. The LOSS column printed "-" for every row once,
         * because it was keyed off idle.loss > 0 rather than off
         * idle.samples, and fmt_pct(0.0) was never reached. A run that
         * probed 10 times and lost half a percent has to say 0.50%. */
        snprintf(want, sizeof want, "%.2f%%", loss[i]);
        slice(line, H_LOSS_X, H_LOSS_W, col, sizeof col);
        ASSERT_STR(col, want);
        if (strcmp(col, "-") == 0)
            FAIL_MSG("run %d: LOSS column shows \"-\" despite idle.samples=%d "
                     "and idle.loss=%.2f", i, (i + 1) * 10, loss[i]);

        stamp_of(stamp[i], want, sizeof want);
        slice(line, H_TIME_X, H_TIME_W, col, sizeof col);
        ASSERT_STR(col, want);

        slice(line, H_DOWN_X, H_DOWN_W, col, sizeof col);
        ASSERT_STR(col, down[i]);
    }

    /* The header row is still the header row, not a data row. */
    ASSERT(row_text(rows, 2, line, sizeof line));
    ASSERT(strstr(line, "TIME") != NULL);
    ASSERT(strstr(line, "LOSS") != NULL);
    free(h);
}

TEST(history_loss_column_falls_back_to_dash_only_with_no_probes)
{
    Engine e;
    History *h = history_alloc();
    GScreen g;
    char rows[TEXT_CAP], line[TEXT_CAP], col[TEXT_CAP];

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    /* samples = 0, loss = 0.0: never probed, so "-" is the honest answer and
     * "0.00%" would be a measurement that was never taken. This is the other
     * half of the pin above. */
    h->runs[0].idle.samples = 0;
    h->runs[0].idle.loss = 0.0;
    h->runs[0].idle.med = 0.0;
    h->runs[1].idle.samples = 1;
    h->runs[1].idle.loss = 4.0;
    /* THE case that separates "no probes" from "no loss". Probed 40 times and
     * lost nothing, which is a clean link and not a test that never ran. This
     * is the row the shipped bug got wrong: keying the column off
     * `idle.loss > 0` prints "-" here, which reads as "never probed" and is
     * indistinguishable from the run above it. Keying it off `idle.samples`
     * prints "0.00%", which is the measured truth. */
    h->runs[2].idle.samples = 40;
    h->runs[2].idle.loss = 0.0;
    h->runs[2].idle.med = 0.0;

    ASSERT(gscreen_open(&g, 120, 40));
    view_history(&g.s, &e, h, 0);
    screen_rows(&g.s, rows, sizeof rows);
    guard_or_fail(&g, "HISTORY", 120, 40);
    gscreen_close(&g);

    ASSERT(row_text(rows, 3, line, sizeof line));
    slice(line, H_LOSS_X, H_LOSS_W, col, sizeof col);
    ASSERT_STR(col, "-");

    ASSERT(row_text(rows, 4, line, sizeof line));
    slice(line, H_LOSS_X, H_LOSS_W, col, sizeof col);
    ASSERT_STR(col, "4.00%");

    ASSERT(row_text(rows, 5, line, sizeof line));
    slice(line, H_LOSS_X, H_LOSS_W, col, sizeof col);
    if (strcmp(col, "-") == 0)
        FAIL_MSG("a run with idle.samples=40 and idle.loss=0.0 shows \"-\" in "
                 "the LOSS column; that reads as \"never probed\". Expected "
                 "0.00%%. Row reads \"%s\"", line);
    ASSERT_STR(col, "0.00%");
    free(h);
}

TEST(history_selection_moves_and_clamps)
{
    Engine e;
    History *h = history_alloc();
    static const int SELS[] = { 0, 1, 2, 9999, -1, -9999 };
    static const int WANT[] = { 0, 1, 2,  2,   0,  0   };

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    for (unsigned k = 0; k < sizeof SELS / sizeof SELS[0]; k++) {
        GScreen g;
        char rows[TEXT_CAP], line[TEXT_CAP];
        int row = -1, want_row;

        ASSERT(gscreen_open(&g, 120, 40));
        view_history(&g.s, &e, h, SELS[k]);

        /* The selection is drawn as an inverse band, T_SURFACE background
         * across the whole row. That is the only way a caller can tell which
         * row is selected without reaching into the view's internals. */
        for (int y = 0; y < g.s.h && row < 0; y++) {
            int painted = 0;
            for (int x = 0; x < g.s.w; x++)
                if (g.s.cells[(size_t)y * g.s.w + x].bg == T_SURFACE) painted++;
            if (painted > 0) row = y;
        }

        screen_rows(&g.s, rows, sizeof rows);
        want_row = 3 + WANT[k];

        printf("    sel=%-6d -> row %d (want %d)\n", SELS[k], row, want_row);

        ASSERT(row == want_row);
        if (row >= 0) {
            ASSERT(row_text(rows, row, line, sizeof line));
            /* And the row under the highlight really is the run that was asked
             * for - a highlight on the right row with the wrong data under it
             * is still a wrong screen. */
            switch (WANT[k]) {
                case 0: ASSERT(strstr(line, "111M") != NULL); break;
                case 1: ASSERT(strstr(line, "222M") != NULL); break;
                default: ASSERT(strstr(line, "333M") != NULL); break;
            }
        }
        ASSERT(strstr(rows, "999M") == NULL);
        guard_or_fail(&g, "HISTORY", 120, 40);
        gscreen_close(&g);
    }
    free(h);
}

TEST(history_scrolling_keeps_the_selection_on_screen)
{
    Engine e;
    History *h = history_alloc();
    static const int SELS[] = { 0, 7, 8, 40, 199, 5000 };

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);
    h->count = MAX_RUNS;
    for (int i = 0; i < MAX_RUNS; i++) {
        h->runs[i].timestamp_ms = T_RUN0 + (uint64_t)i * 60000ull;
        h->runs[i].down.avg = 100.0 + (double)i;
        h->runs[i].idle.samples = 5;
        h->runs[i].idle.loss = 0.0;
        h->runs[i].stab_grade[0] = '\0';
        h->runs[i].bloat_grade[0] = '\0';
    }

    /* 120x40 leaves 36 body rows for data. */
    for (unsigned k = 0; k < sizeof SELS / sizeof SELS[0]; k++) {
        GScreen g;
        char rows[TEXT_CAP], line[TEXT_CAP];
        int row = -1, want = SELS[k] >= MAX_RUNS ? MAX_RUNS - 1 : SELS[k];

        ASSERT(gscreen_open(&g, 120, 40));
        view_history(&g.s, &e, h, SELS[k]);
        for (int y = 0; y < g.s.h && row < 0; y++) {
            int painted = 0;
            for (int x = 0; x < g.s.w; x++)
                if (g.s.cells[(size_t)y * g.s.w + x].bg == T_SURFACE) painted++;
            if (painted) row = y;
        }
        screen_rows(&g.s, rows, sizeof rows);

        /* Rows run from y=3 to y=3+36-1 = 38, and the last screen row belongs
         * to the tab bar. Anything outside that band means the scroll clamp
         * let the selection walk off the table. */
        ASSERT(row >= 3 && row <= 38);
        printf("    200-run log, sel=%-5d -> highlight row %d\n", SELS[k], row);

        ASSERT(row_text(rows, row, line, sizeof line));
        {
            char want_rate[32];
            snprintf(want_rate, sizeof want_rate, "%.0fM", 100.0 + (double)want);
            if (strstr(line, want_rate) == NULL)
                FAIL_MSG("sel=%d: highlight is on row %d but that row does not "
                         "show run %d's rate \"%s\"; row reads \"%s\"",
                         SELS[k], row, want, want_rate, line);
        }
        guard_or_fail(&g, "HISTORY(200)", 120, 40);
        gscreen_close(&g);
    }
    free(h);
}

TEST(empty_history_renders_an_empty_state)
{
    Engine e;
    History *h;
    GScreen g;
    char rows[TEXT_CAP], line[TEXT_CAP];
    int ink;

    engine_init(&e, IP_A, COLO_A, ORG_A);
    h = (History *)calloc(1, sizeof *h);
    ASSERT(h != NULL);
    if (!h) return;

    ASSERT(gscreen_open(&g, 120, 40));
    view_history(&g.s, &e, h, 0);
    ink = screen_ink(&g.s);
    screen_rows(&g.s, rows, sizeof rows);
    guard_or_fail(&g, "HISTORY(empty)", 120, 40);
    gscreen_close(&g);

    if (ink == 0)
        FAIL_MSG("an empty History rendered a blank screen instead of its "
                 "empty state");
    ASSERT(ink > 0);
    ASSERT(strstr(rows, "HISTORY") != NULL);
    ASSERT(strstr(rows, "nothing recorded yet") != NULL);
    /* Count 0 is reported as "0 runs" or "no runs yet"; either is fine, but a
     * screen claiming "3 runs" over an empty log is not. */
    ASSERT(strstr(rows, "no runs yet") != NULL);
    /* Nothing that could only have come from uninitialised memory. */
    ASSERT(strstr(rows, "111M") == NULL);
    ASSERT(strstr(rows, "999M") == NULL);

    /* And the row that holds the message is inside the screen. */
    ASSERT(row_text(rows, 4, line, sizeof line));
    ASSERT(strstr(line, "nothing recorded") != NULL);

    /* A NULL History is the other empty case the view claims to handle. */
    ASSERT(gscreen_open(&g, 120, 40));
    view_history(&g.s, &e, NULL, 0);
    screen_rows(&g.s, rows, sizeof rows);
    ASSERT(strstr(rows, "no runs yet") != NULL);
    guard_or_fail(&g, "HISTORY(NULL)", 120, 40);
    gscreen_close(&g);
    free(h);
}

TEST(history_count_outside_the_array_is_clamped_not_trusted)
{
    Engine e;
    History *h = history_alloc();
    GScreen g;
    char rows[TEXT_CAP];

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    /* count comes off disk. A hand-edited log with a count past the end of
     * the array must not be walked past the end. */
    h->count = MAX_RUNS + 50;
    ASSERT(gscreen_open(&g, 120, 40));
    view_history(&g.s, &e, h, 3);
    screen_rows(&g.s, rows, sizeof rows);
    guard_or_fail(&g, "HISTORY(count>MAX_RUNS)", 120, 40);
    gscreen_close(&g);
    ASSERT(strstr(rows, "HISTORY") != NULL);

    /* Negative count, which is what a truncated write can leave behind. */
    h->count = -5;
    ASSERT(gscreen_open(&g, 120, 40));
    view_history(&g.s, &e, h, 0);
    screen_rows(&g.s, rows, sizeof rows);
    ASSERT(strstr(rows, "no runs yet") != NULL);
    guard_or_fail(&g, "HISTORY(count<0)", 120, 40);
    gscreen_close(&g);
    free(h);
}

/* ======================================================= 7. screen sizes = */

static const int SIZES[][2] = {
    { 200, 60 }, { 120, 40 }, { 100, 30 }, { 60, 20 },
    {  40, 10 }, {  20,  6 }, {   4,  3 }, {   1,  1 }
};
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

/* The string views.c draws when it refuses to draw anything else. */
static const char TOO_SMALL[] = "terminal too small - need at least 60x20";

/* Exactly what row 0 must contain at a given width, clipped by cell_text's
 * max (w - 2, floored at 1) and then by cell_put's right edge. */
static void expected_small_row0(int w, char *out, size_t cap)
{
    int max = w - 2;
    int n;
    size_t len;

    if (max < 1) max = 1;
    n = max;
    if (n > w - 1) n = w - 1;
    if (n < 0) n = 0;
    if ((size_t)n + 2 >= cap) n = (int)cap - 2;
    memcpy(out, " ", (size_t)n + 1);
    memcpy(out + 1, TOO_SMALL, (size_t)n);
    out[n + 1] = '\0';                 /* the memcpy above only cleared to n */
    len = strlen(out);
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
}

TEST(every_view_survives_every_screen_size)
{
    Engine e;
    History *h = history_alloc();

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    for (int si = 0; si < NSIZES; si++) {
        int w = SIZES[si][0], hh = SIZES[si][1];
        int big = (w >= VIEWS_MIN_W && hh >= VIEWS_MIN_H);

        for (int i = 0; i < 4; i++) {
            GScreen g;
            char rows[TEXT_CAP], line[TEXT_CAP];
            char want[64];

            ASSERT(gscreen_open(&g, w, hh));
            VIEWS[i].fn(&g.s, &e, h);
            if (big) assert_not_blank(&g.s, VIEWS[i].name);
            screen_rows(&g.s, rows, sizeof rows);

            printf("    %-8s %3dx%-3d ink=%-5d row0=\"%.60s\"\n",
                   VIEWS[i].name, w, hh, screen_ink(&g.s), rows);

            if (big) {
                /* Full render: the title strip must be there, clipped but
                 * present, at every size the view claims to support. */
                ASSERT(row_text(rows, 0, line, sizeof line));
                ASSERT(strstr(line, VIEWS[i].title) != NULL);
            } else {
                /* Small-screen path: row 0 carries exactly the clipped
                 * "too small" notice, and nothing else on the screen is
                 * touched. A view that draws real content here is writing
                 * into a layout it has already decided does not fit. */
                expected_small_row0(w, want, sizeof want);
                ASSERT(row_text(rows, 0, line, sizeof line));
                ASSERT_STR(line, want);
                for (int y = 1; y < hh; y++) {
                    ASSERT(row_text(rows, y, line, sizeof line));
                    ASSERT_STR(line, "");
                }
            }

            guard_or_fail(&g, VIEWS[i].name, w, hh);
            gscreen_close(&g);
        }
    }
    free(h);
}

/* ========================================================= 8. canary ==== */

TEST(cell_buffer_guard_region_is_never_touched)
{
    Engine e;
    History *h = history_alloc();
    int renders = 0, dirty = 0;

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    for (int si = 0; si < NSIZES; si++) {
        int w = SIZES[si][0], hh = SIZES[si][1];

        for (int i = 0; i < 4; i++) {
            GScreen g;
            ASSERT(gscreen_open(&g, w, hh));
            VIEWS[i].fn(&g.s, &e, h);
            renders++;
            if (gscreen_guard_damage(&g) != -1) {
                dirty++;
                guard_or_fail(&g, VIEWS[i].name, w, hh);
            }
            gscreen_close(&g);

            /* Same again through the sel-taking entry point, including the
             * out-of-range selections, because the clamp is the code that
             * indexes the array. */
            ASSERT(gscreen_open(&g, w, hh));
            view_history(&g.s, &e, h, 9999);
            renders++;
            if (gscreen_guard_damage(&g) != -1) {
                dirty++;
                guard_or_fail(&g, "HISTORY(sel=9999)", w, hh);
            }
            gscreen_close(&g);

            /* And with a history whose count points past the array. */
            ASSERT(gscreen_open(&g, w, hh));
            h->count = MAX_RUNS + 17;
            view_history(&g.s, &e, h, 2);
            h->count = 3;
            renders++;
            if (gscreen_guard_damage(&g) != -1) {
                dirty++;
                guard_or_fail(&g, "HISTORY(count>MAX_RUNS)", w, hh);
            }
            gscreen_close(&g);
        }
    }

    printf("    guard: %d renders across %d sizes, %d guard regions clobbered "
           "(%d cells each side, byte 0x%02X)\n",
           renders, NSIZES, dirty, GUARD_CELLS, GUARD_BYTE);
    ASSERT(dirty == 0);
    free(h);
}

TEST(guard_region_check_can_actually_fail)
{
    /* A canary that has never been seen to trip is indistinguishable from a
     * canary that is not wired up. Write one cell past the end of a real
     * buffer and confirm the check reports it - then write one cell before the
     * start, because a view indexing with a negative row lands there. */
    GScreen g;
    long dmg;

    ASSERT(gscreen_open(&g, 40, 10));
    ASSERT(gscreen_guard_damage(&g) == -1);

    g.base[g.lead + g.ncells].utf8[0] = 'X';
    dmg = gscreen_guard_damage(&g);
    ASSERT(dmg == 1);
    g.base[g.lead + g.ncells].utf8[0] = (char)GUARD_BYTE;

    ASSERT(gscreen_guard_damage(&g) == -1);

    g.base[g.lead - 1].utf8[0] = 'X';
    dmg = gscreen_guard_damage(&g);
    ASSERT(dmg == -1);
    g.base[g.lead - 1].utf8[0] = (char)GUARD_BYTE;

    /* And one that changes only the fg of a guard cell, which the utf8 byte
     * probe alone would miss. */
    g.base[g.lead + g.ncells + 2].fg = 0x12345678u;
    dmg = gscreen_guard_damage(&g);
    ASSERT(dmg == 3);
    g.base[g.lead + g.ncells + 2].fg = (uint32_t)(0xA5A5A5A5u);

    ASSERT(gscreen_guard_damage(&g) == -1);
    gscreen_close(&g);
}

/* ============================================================ dump ====== */

TEST(dump_rendered_text_of_each_view)
{
    Engine e;
    History *h = history_alloc();

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    for (int i = 0; i < 4; i++) {
        GScreen g;
        char rows[TEXT_CAP];
        const char *p;

        ASSERT(gscreen_open(&g, 120, 40));
        VIEWS[i].fn(&g.s, &e, h);
        screen_rows(&g.s, rows, sizeof rows);
        printf("---- %s 120x40 ----------------------------------------\n",
               VIEWS[i].name);
        for (p = rows; *p; ) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            printf("|%.*s|\n", (int)len, p);
            if (!nl) break;
            p = nl + 1;
        }
        printf("---- end %s (%d non-blank cells) ------------------------\n",
               VIEWS[i].name, screen_ink(&g.s));
        guard_or_fail(&g, VIEWS[i].name, 120, 40);
        gscreen_close(&g);
    }
    free(h);
}

/* ====================================================== dispatcher ====== */

#ifdef CFS_WITH_UI

/* The test the whole file exists for. ui_draw() routes tab -> view_*; the
 * bug it had in life took a tab argument and ignored it, so all five tabs drew
 * LIVE. Nothing about the navigation was visibly broken: the tabs were drawn,
 * hotkeyed and clickable. It showed up only in the pixels.
 *
 * This needs ui.c, which is fine to link because ui.c also reaches the engine
 * only through the stubs above. Build with -DCFS_WITH_UI and src/ui.c.
 */
TEST(dispatcher_routes_each_tab_to_a_distinct_view)
{
    Engine e;
    History *h = history_alloc();
    static const char *TAB_TITLE[5] = {
        "LIVE", "CHARTS", "DETAILS", "HISTORY", "HELP"
    };
    char *text[5];
    static const int W = 120, H = 40;

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);
    ui_set_history_sel(0);

    for (int tab = 0; tab < 5; tab++) {
        GScreen g;
        char rows[TEXT_CAP];

        ASSERT(gscreen_open(&g, W, H));
        ui_draw(&g.s, &e, h, tab, 1);
        screen_rows(&g.s, rows, sizeof rows);

        text[tab] = (char *)malloc(TEXT_CAP);
        if (text[tab]) memcpy(text[tab], rows, TEXT_CAP);

        printf("    tab %d %-8s ink=%-5d row0=\"%.70s\"\n",
               tab, TAB_TITLE[tab], screen_ink(&g.s), rows);

        /* Not blank, and named after itself in the tab bar ui.c draws. */
        ASSERT(screen_ink(&g.s) > 0);
        ASSERT(strstr(rows, TAB_TITLE[tab]) != NULL);
        guard_or_fail(&g, TAB_TITLE[tab], W, H);
        gscreen_close(&g);
    }

    /* All ten pairs. One collision is the bug; ten passing pairs is the proof
     * it is gone. */
    for (int a = 0; a < 5; a++) {
        for (int b = a + 1; b < 5; b++) {
            char label[32];
            if (!text[a] || !text[b]) continue;
            if (strcmp(text[a], text[b]) != 0) continue;
            snprintf(label, sizeof label, "ui_draw %dx%d", W, H);
            report_collision(label, TAB_TITLE[a], text[a],
                             TAB_TITLE[b], text[b]);
        }
    }

    printf("    pairwise: ");
    for (int a = 0; a < 5; a++)
        for (int b = a + 1; b < 5; b++)
            printf("%d/%d=%s ", a, b,
                   (text[a] && text[b] && strcmp(text[a], text[b])) ? "DIFF" : "SAME");
    printf("\n");

    for (int i = 0; i < 5; i++) free(text[i]);
    free(h);
}

TEST(dispatcher_passes_the_history_selection_through)
{
    Engine e;
    History *h = history_alloc();
    GScreen g;

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    ui_set_history_sel(1);
    ASSERT(ui_history_sel() == 1);
    ASSERT(gscreen_open(&g, 120, 40));
    ui_draw(&g.s, &e, h, 3, 1);
    {
        int row = -1;
        for (int y = 0; y < g.s.h && row < 0; y++)
            for (int x = 0; x < g.s.w; x++)
                if (g.s.cells[(size_t)y * g.s.w + x].bg == T_SURFACE) { row = y; break; }
        /* sel=1 with 3 runs from first=0 -> row 3 + 1 = 4. */
        ASSERT(row == 4);
    }
    guard_or_fail(&g, "ui_draw(tab=3)", 120, 40);
    gscreen_close(&g);

    /* A negative selection is clamped by ui.c before the view ever sees it. */
    ui_set_history_sel(-9);
    ASSERT(ui_history_sel() == 0);
    free(h);
}

TEST(dispatcher_refuses_to_draw_below_its_own_minimum)
{
    Engine e;
    History *h = history_alloc();
    GScreen g;
    char rows[TEXT_CAP];

    ASSERT(h != NULL);
    if (!h) return;
    engine_init(&e, IP_A, COLO_A, ORG_A);

    /* ui.c has its own floor (60x14), tighter than the views' 60x20. Below
     * it nothing is routed anywhere at all, and the refusal is drawn at the
     * full width rather than clipped to w-2. */
    ASSERT(gscreen_open(&g, 40, 10));
    ui_draw(&g.s, &e, h, 0, 1);
    screen_rows(&g.s, rows, sizeof rows);
    printf("    ui_draw 40x10: \"%s\"\n", rows);
    ASSERT(strstr(rows, "too small") != NULL);
    guard_or_fail(&g, "ui_draw(40x10)", 40, 10);
    gscreen_close(&g);

    /* At 60x14 ui.c lets the frame through and each view applies its own
     * 60x20 rule, so the message is the views' and the tab bar is not drawn. */
    ASSERT(gscreen_open(&g, 60, 14));
    ui_draw(&g.s, &e, h, 4, 1);
    screen_rows(&g.s, rows, sizeof rows);
    printf("    ui_draw 60x14 tab=4: \"%s\"\n", rows);
    ASSERT(strstr(rows, "terminal too small") != NULL);
    guard_or_fail(&g, "ui_draw(60x14)", 60, 14);
    gscreen_close(&g);
    free(h);
}

#endif /* CFS_WITH_UI */

/* ============================================================== main ==== */

int main(void)
{
    /* Pin the timezone so the dumps below read the same on every machine.
     * The tests themselves do not depend on it: expected stamps are derived
     * with the same localtime() call views.c uses. */
#if defined(_WIN32)
    _putenv_s("TZ", "UTC0");
    _tzset();
#else
    setenv("TZ", "UTC0", 1);
    tzset();
#endif
    return cfs_test_run_all("views-render");
}
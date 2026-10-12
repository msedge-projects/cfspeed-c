/* zz_integrity.c - VERIFICATION SCAFFOLD. Not part of the product.
 *
 * A single translation unit that includes EVERY header in src/ at once, then
 * names one real symbol from every module so that no declaration can be
 * optimised away and no header can hide behind "never referenced".
 *
 * The point: each module passed `-fsyntax-only` on its own .c file, which
 * only ever proves the module is self-consistent. It cannot see a macro that
 * a sibling header redefines, a typedef that a sibling header redeclares with
 * a different shape, or an enum constant that two modules both claim. This
 * file is where those show up.
 *
 * One function, `zz_integrity_probe`, accumulates every module's entry point
 * into one volatile sink so all references survive -O2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

/* Every header in src/, in dependency-agnostic (alphabetical) order so that
 * nothing here accidentally relies on one header arriving before another. */
#include "actions.h"
#include "chart.h"
#include "cli.h"
#include "engine.h"
#include "env_size.h"
#include "fmt.h"
#include "history.h"
#include "keymap.h"
#include "net.h"
#include "stats.h"
#include "term.h"
#include "test.h"
#include "theme.h"
#include "ui.h"
#include "views.h"

/* Accumulated so nothing below is optimised away. Never read by product code. */
static volatile unsigned long long zz_sink = 0;

static void zz_take(unsigned long long v) { zz_sink += v; }

/* --- one reference per module ---------------------------------------------- */

/* term.c */
static void zz_touch_term(void) {
    int w = 0, h = 0;
    zz_take((unsigned long long)term_size(&w, &h));
    zz_take((unsigned long long)(size_t)w + (unsigned long long)(size_t)h);
    Screen *s = screen_create(8, 4);
    screen_clear(s);
    cell_put(s, 0, 0, "x", T_TEXT, BG_NONE, 0, 0);
    cell_fill(s, 0, 0, 1, 1, T_SURFACE);
    zz_take((unsigned long long)cell_text(s, 0, 0, 8, "x", T_TEXT, BG_NONE, 0));
    frame_invalidate();
    frame_present(s);
    screen_free(s);
    zz_take((unsigned long long)now_ms());
    zz_take((unsigned long long)(uint32_t)utf8_encode(0x41, (char *)&w));
    Event ev = term_poll();
    zz_take((unsigned long long)ev.type);
}

/* net.c */
static void zz_touch_net(void) {
    zz_take((unsigned long long)(long long)net_init());
    Meta m;
    memset(&m, 0, sizeof m);
    net_fetch_meta("https://example.invalid", &m);
    zz_take((unsigned long long)(size_t)m.edge_ip_count);
    double ms = 0.0;
    zz_take((unsigned long long)(long long)net_probe("https://example.invalid", &ms));
    net_shutdown();
    LatStats ls;
    memset(&ls, 0, sizeof ls);
    zz_take((unsigned long long)(size_t)ls.samples);
    zz_take((unsigned long long)(size_t)(NET_OK + NET_ERR + NET_REFUSED));
}

/* engine.c */
static void zz_touch_engine(void) {
    Engine *e = engine_new("https://example.invalid", MODE_FULL);
    zz_take((unsigned long long)engine_poll(e));
    zz_take((unsigned long long)engine_finished(e));
    zz_take((unsigned long long)(size_t)engine_phase(e));
    zz_take((unsigned long long)(size_t)(engine_mode(e)));
    zz_take((unsigned long long)engine_elapsed_ms(e));
    zz_take((unsigned long long)(long long)(engine_instant(e) * 1000.0));
    zz_take((unsigned long long)engine_error(e) ? 1u : 0u);
    zz_take((unsigned long long)phase_label(PH_IDLE) ? 1u : 0u);
    const RunResult *rr = engine_result(e);
    const Meta *mm = engine_meta(e);
    if (rr) zz_take((unsigned long long)rr->valid +
                    (unsigned long long)rr->down.nsamples);
    if (mm) zz_take((unsigned long long)(size_t)mm->edge_ip_count);
    int cnt = 0;
    zz_take((unsigned long long)(uintptr_t)engine_down_samples(e, &cnt));
    zz_take((unsigned long long)(uintptr_t)engine_up_samples(e, &cnt));
    zz_take((unsigned long long)(uintptr_t)engine_probe_samples(e, &cnt));
    engine_restart(e, MODE_LATENCY);
    engine_cancel(e);
    engine_free(e);

    double samp[MAX_SAMPLES];
    Throughput tp;
    memset(samp, 0, sizeof samp);
    memset(&tp, 0, sizeof tp);
    stats_finalise(samp, MAX_SAMPLES, 0, 1, &tp);
    zz_take((unsigned long long)tp.nsamples);
    zz_take((unsigned long long)(size_t)MAX_PROBES);
}

/* stats.c */
static void zz_touch_stats(void) {
    static const double v[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const size_t n = 8;
    zz_take((unsigned long long)(stats_mean(v, n) * 100.0));
    zz_take((unsigned long long)(stats_percentile(v, n, 0.95) * 100.0));
    zz_take((unsigned long long)(stats_median(v, n) * 100.0));
    zz_take((unsigned long long)(stats_min(v, n) * 100.0));
    zz_take((unsigned long long)(stats_max(v, n) * 100.0));
    zz_take((unsigned long long)(stats_stddev(v, n) * 100.0));
    zz_take((unsigned long long)(stats_jitter(v, n) * 100.0));
    zz_take((unsigned long long)(stats_cv_percent(v, n) * 100.0));
    zz_take((unsigned long long)(stats_loss_percent(1, 2) * 100.0));
    zz_take((unsigned long long)(stats_bufferbloat(v, n, 2.0) * 100.0));
    zz_take((unsigned long long)stats_grade_bufferbloat(1.0));
    zz_take((unsigned long long)stats_grade_stability(1.0));
    zz_take((unsigned long long)stats_grade_loss(1.0));
    zz_take((unsigned long long)(stats_trimmed_mean(v, n) * 100.0));
}

/* fmt.c */
static void zz_touch_fmt(void) {
    char buf[64];
    fmt_rate(buf, sizeof buf, 110.5);
    zz_take((unsigned long long)buf[0]);
    fmt_ms(buf, sizeof buf, 9.4);
    zz_take((unsigned long long)buf[0]);
    fmt_bytes(buf, sizeof buf, 5200000u);
    zz_take((unsigned long long)buf[0]);
    fmt_pct(buf, sizeof buf, 1.25);
    zz_take((unsigned long long)buf[0]);
    fmt_stamp(buf, sizeof buf, (int64_t)1760000000000LL);
    zz_take((unsigned long long)buf[0]);
    fmt_duration(buf, sizeof buf, 65.0);
    zz_take((unsigned long long)buf[0]);
    fmt_truncate(buf, sizeof buf, "truncate me", 4);
    zz_take((unsigned long long)buf[0]);
}

/* chart.c */
static void zz_touch_chart(void) {
    static const double samples[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                        9, 10, 11, 12, 13, 14, 15, 16 };
    ChartScale cs;
    memset(&cs, 0, sizeof cs);
    chart_scale(samples, 16, 16, &cs);
    zz_take((unsigned long long)cs.bars);
    zz_take((unsigned long long)(cs.peak + cs.mean + cs.p95));
    zz_take((unsigned long long)cs.zeroed);
    zz_take((unsigned long long)(cs.h[0] * 100.0f));
    zz_take((unsigned long long)(size_t)CHART_MAX_BARS);
    zz_take((unsigned long long)(chart_nice_ceiling(87.3) * 100.0));
    char ticks[64];
    zz_take((unsigned long long)chart_ticks(100.0, ticks, sizeof ticks));
    /* chart.h feature-detects stats.h; prove which branch this TU took. */
#if defined(CFS_CHART_HAVE_STATS_H) && CFS_CHART_HAVE_STATS_H
    zz_take(1u);
#else
    zz_take(0u);
#endif
}

/* actions.c */
static void zz_touch_actions(void) {
    ActionMenu m;
    actions_init(&m);
    zz_take((unsigned long long)m.count);
    zz_take((unsigned long long)m.sel);
    zz_take((unsigned long long)m.open);
    zz_take((unsigned long long)m.items[0]);
    zz_take((unsigned long long)actions_apply(&m, ACTMENU_NAVIGATE, 1, 0));
    zz_take((unsigned long long)actions_apply(&m, ACTMENU_TOGGLE, 1, 0));
    zz_take((unsigned long long)actions_apply(&m, ACTMENU_CANCEL, 1, 0));
    zz_take((unsigned long long)actions_label(ITEM_RERUN) ? 1u : 0u);
    zz_take((unsigned long long)(unsigned)actions_enabled(ITEM_RESUME, 1, 0));
    zz_take((unsigned long long)(size_t)ACTIONS_MAX_ITEMS);
    zz_take((unsigned long long)(size_t)ITEM_COUNT);
    zz_take((unsigned long long)(size_t)ACTMENU_NONE);
}

/* keymap.c - note this header pulls in term.h. */
static void zz_touch_keymap(void) {
    TermKey k;
    memset(&k, 0, sizeof k);
    k.type = EV_KEY;
    k.key = KEY_CHAR;
    k.ch = 'q';
    k.mods = KM_MOD_CTRL;
    zz_take((unsigned long long)keymap_action(k, 0, 0));
    zz_take((unsigned long long)(size_t)ACT_NONE);
    zz_take((unsigned long long)(size_t)ACT_QUIT);
    zz_take((unsigned long long)(size_t)ACT_CLICK);
    int col = -1, row = -1;
    TermKey d = keymap_decode_at("\t", &col, &row);
    zz_take((unsigned long long)(size_t)d.key);
    zz_take((unsigned long long)(size_t)col + (unsigned long long)(size_t)row);
    Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_MOUSE;
    ev.mbtn = 0;
    zz_take((unsigned long long)(size_t)keymap_from_event(ev).type);
    char name[32];
    keymap_keyname(k, name, sizeof name);
    zz_take((unsigned long long)name[0]);
    char help[256];
    zz_take((unsigned long long)keymap_help(help, sizeof help));
    int c2 = 0, r2 = 0;
    zz_take((unsigned long long)keymap_wants_point(ACT_CLICK, &c2, &r2));
    zz_take((unsigned long long)(size_t)KM_TAB_ROW);
    zz_take((unsigned long long)(KM_MOD_SHIFT + KM_MOD_CTRL + KM_MOD_ALT));
}

/* ui.c */
static void zz_touch_ui(Screen *s, const Engine *e, const History *h) {
    ui_draw(s, e, h, 0, 1);
    ui_set_history_sel(3);
    zz_take((unsigned long long)ui_history_sel());
}

/* views.c - History comes from views.h, which may or may not be history.h's. */
static void zz_touch_views(Screen *s, const Engine *e) {
    History h;
    memset(&h, 0, sizeof h);
    view_charts(s, e, &h);
    view_details(s, e, &h);
    view_history(s, e, &h, 0);
    view_help(s, e, &h);
    zz_take((unsigned long long)(size_t)VIEWS_MIN_W);
    zz_take((unsigned long long)(size_t)VIEWS_MIN_H);
    zz_take((unsigned long long)(size_t)(VIEWS_BODY_TOP + VIEWS_BODY_BOT));
#if defined(CFS_VIEWS_HAVE_HISTORY_H) && CFS_VIEWS_HAVE_HISTORY_H
    zz_take((unsigned long long)h.count);
    zz_take((unsigned long long)(uintptr_t)h.runs);
    zz_take((unsigned long long)(size_t)MAX_RUNS);
#else
    zz_take((unsigned long long)h.count);
    zz_take((unsigned long long)(uintptr_t)h.runs);
#endif
}

/* history.c */
static void zz_touch_history(void) {
    History h;
    memset(&h, 0, sizeof h);
    zz_take((unsigned long long)history_save(&h));
    history_load(&h);
    zz_take((unsigned long long)h.count);
    zz_take((unsigned long long)history_clear(&h));
    zz_take((unsigned long long)(history_path() ? 1u : 0u));
    zz_take((unsigned long long)(size_t)MAX_RUNS);
}

/* cli.c */
static void zz_touch_cli(int argc, char **argv) {
    Options o;
    memset(&o, 0, sizeof o);
    cli_parse(argc, argv, &o);
    zz_take((unsigned long long)cli_validate(&o));
    zz_take((unsigned long long)(size_t)o.mode);
    zz_take((unsigned long long)o.out[0]);
}

/* test.c */
static void zz_touch_test(void) {
    zz_take((unsigned long long)(long long)cfs_test_run_all("integrity"));
}

/* theme.h - static inline at file scope, included from many TUs. */
static void zz_touch_theme(void) {
    zz_take((unsigned long long)theme_shade(T_DOWN, 0.5));
    zz_take((unsigned long long)theme_grade("A+"));
    zz_take((unsigned long long)RGB(1, 2, 3));
    zz_take((unsigned long long)T_TEXT_HI);
    zz_take((unsigned long long)T_GRADE_A + T_GRADE_B + T_GRADE_C +
            T_GRADE_D + T_GRADE_F + T_GRADE_AP);
    zz_take((unsigned long long)T_LAT + T_UP + T_BORDER + T_TEXT_DIM);
}

/* env_size.h - calls getenv/atoi. */
static void zz_touch_env_size(void) {
    int w = 0, h = 0;
    env_size(&w, &h);
    zz_take((unsigned long long)(size_t)w + (unsigned long long)(size_t)h);
    zz_take((unsigned long long)(size_t)(MIN_SCREEN_W + MIN_SCREEN_H));
}

/* main.c - referenced without defining a second entry point. */
int main(int argc, char **argv); /* declared by main.c; referenced, not defined */

static void zz_touch_main(void) {
    int (*pmain)(int, char **) = &main;
    zz_take((unsigned long long)(uintptr_t)pmain);
}

void zz_integrity_probe(int argc, char **argv);

void zz_integrity_probe(int argc, char **argv) {
    zz_touch_term();
    zz_touch_net();
    zz_touch_engine();
    zz_touch_stats();
    zz_touch_fmt();
    zz_touch_chart();
    zz_touch_actions();
    zz_touch_keymap();
    zz_touch_history();
    zz_touch_cli(argc, argv);
    zz_touch_test();
    zz_touch_theme();
    zz_touch_env_size();
    zz_touch_main();

    Engine *e = engine_new("https://example.invalid", MODE_FULL);
    Screen *s = screen_create(80, 24);
    History h;
    memset(&h, 0, sizeof h);
    zz_touch_ui(s, e, &h);
    zz_touch_views(s, e);
    screen_free(s);
    engine_free(e);
}

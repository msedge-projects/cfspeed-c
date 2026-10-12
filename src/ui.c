/* Views. Port of the Rust layout: full-bleed, no boxes, structure carried by
 * whitespace, thin rules and colour weight. */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "views.h"

#define MIN_W 60
#define MIN_H 14

/* ------------------------------------------------------------- utilities */

static void fmt_rate(double mbps, char *out, size_t cap) {
    if (mbps >= 1000.0)      snprintf(out, cap, "%.2f Gbps", mbps / 1000.0);
    else if (mbps >= 100.0)  snprintf(out, cap, "%.0f Mbps", mbps);
    else if (mbps >= 10.0)   snprintf(out, cap, "%.1f Mbps", mbps);
    else                     snprintf(out, cap, "%.2f Mbps", mbps);
}

static void fmt_ms(double ms, char *out, size_t cap) {
    if (ms >= 100.0)     snprintf(out, cap, "%.0fms", ms);
    else if (ms >= 10.0) snprintf(out, cap, "%.1fms", ms);
    else                 snprintf(out, cap, "%.2fms", ms);
}

static void fmt_clock(uint64_t ms, char *out, size_t cap) {
    uint64_t total = ms / 1000;
    snprintf(out, cap, "%d:%02d", (int)(total / 60), (int)(total % 60));
}

/* Strip a trailing :443 so the route line stays readable. */
static void strip_port(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if (n >= 4 && strcmp(in + n - 4, ":443") == 0) n -= 4;
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

static int is_v6(const char *ip) { return strchr(ip, ':') != NULL; }

/* ---------------------------------------------------------------- charts */

#define BLOCK "\xe2\x96\x88" /* U+2588 FULL BLOCK */

/* A bar graph, one column per sample, with the leading edge of each bar
 * brightened so the current value is findable without reading the axis.
 * Bucketing takes the peak of the bucket, never the mean: averaging spikes
 * away is how a speed trace loses the thing it exists to show. */
static void draw_chart(Screen *s, int x, int y, int w, int h, const double *data,
                       int n, double max, uint32_t accent) {
    if (w <= 0 || h <= 0) return;
    char label[32];

    /* Grid rule at half height, behind the bars. */
    for (int i = 0; i < w; i++)
        cell_put(s, x + i, y + h / 2, "\xc2\xb7", T_BORDER, BG_NONE, 0, 0);

    if (max <= 0.0) max = 1.0;

    for (int i = 0; i < w; i++) {
        double v;
        if (n >= w && w > 0) {
            int lo = (int)((long)i * n / w);
            int hi = (int)((long)(i + 1) * n / w);
            if (hi <= lo) hi = lo + 1;
            if (hi > n) hi = n;
            v = 0.0;
            for (int k = lo; k < hi; k++)
                if (data[k] > v) v = data[k];
        } else {
            if (i >= n) break;
            v = data[i];
        }
        int filled = (int)((v / max) * (double)h + 0.5);
        if (filled > h) filled = h;
        int top = h - filled;
        for (int cy = top; cy < h; cy++) {
            uint32_t col = (cy == top) ? accent : theme_shade(accent, 0.72);
            cell_put(s, x + i, y + cy, BLOCK, col, BG_NONE, 0, 0);
        }
    }

    /* Scale labels: top, middle, baseline. */
    snprintf(label, sizeof label, "%d", (int)(max + 0.5));
    cell_text(s, x, y, 8, label, T_TEXT_DIM, BG_NONE, 0);
    snprintf(label, sizeof label, "%d", (int)(max / 2.0 + 0.5));
    cell_text(s, x, y + h / 2, 8, label, T_TEXT_DIM, BG_NONE, 0);
    cell_text(s, x, y + h - 1, 8, "0", T_TEXT_DIM, BG_NONE, 0);
}

/* ---------------------------------------------------------------- header */

static void draw_header(Screen *s, int y, int w, const Engine *e) {
    char buf[160];
    const Meta *m = engine_meta(e);
    snprintf(buf, sizeof buf, " cfspeed \xc2\xb7 %s \xc2\xb7 %s", m->colo[0] ? m->colo : "\xe2\x80\xa6",
             m->loc[0] ? m->loc : "");
    cell_text(s, 0, y, w / 2, buf, T_TEXT_DIM, BG_NONE, 0);

    char clock[16];
    fmt_clock(engine_elapsed_ms(e), clock, sizeof clock);
    Phase ph = engine_phase(e);
    int right = snprintf(buf, sizeof buf, "%s  %s", clock, phase_label(ph));
    cell_text(s, w - right, y, w, buf, T_TEXT_DIM, BG_NONE, 0);

    /* Phase badge, the one deliberate background in the header. */
    const char *badge = (ph == PH_DONE) ? "COMPLETE"
                     : (ph == PH_ERROR) ? "FAILED"
                                        : phase_label(ph);
    int bw = (int)strlen(badge) + 2;
    uint32_t bc = (ph == PH_ERROR) ? T_GRADE_F
               : (ph == PH_DONE)  ? T_GRADE_A
                                  : T_UP;
    cell_fill(s, w - bw - 14, y, bw, 1, bc);
    cell_text(s, w - bw - 13, y, bw, badge, T_SURFACE, bc, 1);
}

static void rule(Screen *s, int y, int w) {
    for (int i = 0; i < w; i++) cell_put(s, i, y, "\xe2\x94\x80", T_BORDER, BG_NONE, 0, 0);
}

/* ------------------------------------------------------------ route strip */

#define YOU_COLS 52

static void draw_route(Screen *s, int y0, int w, const Engine *e) {
    const Meta *m = engine_meta(e);
    char buf[512];

    /* --- you, rule, edge --- */
    int col = cell_text(s, 0, y0, w, "  YOU   ", T_DOWN, BG_NONE, 1);
    col += cell_text(s, col, y0, w - col, m->ip[0] ? m->ip : "\xe2\x80\xa6",
                     T_TEXT_HI, BG_NONE, 0);

    char who[160] = "";
    if (m->asn_org[0]) {
        snprintf(who, sizeof who, "%s", m->asn_org);
        if (m->asn[0] || m->loc[0]) {
            size_t l = strlen(who);
            snprintf(who + l, sizeof who - l, " \xc2\xb7 ");
            l = strlen(who);
            if (m->asn[0]) snprintf(who + l, sizeof who - l, "AS%s", m->asn);
            l = strlen(who);
            if (m->loc[0]) snprintf(who + l, sizeof who - l, " \xc2\xb7 %s", m->loc);
        }
    } else if (m->loc[0]) {
        snprintf(who, sizeof who, "%s", m->loc);
    }
    int used = col - 8;
    if (used < YOU_COLS - 28) used = YOU_COLS - 28;
    col += cell_text(s, col, y0, w - col, who, T_TEXT, BG_NONE, 0);
    for (int i = col; i < YOU_COLS && i < w; i++)
        cell_put(s, i, y0, " ", FG_NONE, BG_NONE, 0, 0);

    cell_put(s, YOU_COLS, y0, "\xe2\x94\x82", T_BORDER, BG_NONE, 0, 0);
    col = YOU_COLS + 1;
    col += cell_text(s, col, y0, w - col, " EDGE  ", T_UP, BG_NONE, 1);
    col += cell_text(s, col, y0, w - col, m->colo[0] ? m->colo : "\xe2\x80\xa6",
                     T_TEXT_HI, BG_NONE, 0);

    /* Location and addresses only: the port, TLS version, cipher, HTTP version
     * and WARP state are properties of the connection, not the route. */
    int n4 = 0;
    for (int i = 0; i < m->edge_ip_count; i++) {
        char ip[80];
        strip_port(m->edge_ips[i], ip, sizeof ip);
        if (is_v6(ip)) continue;
        col += cell_text(s, col, y0, w - col, "  ", FG_NONE, BG_NONE, 0);
        col += cell_text(s, col, y0, w - col, ip, T_TEXT, BG_NONE, 0);
        n4++;
    }

    /* --- v6 addresses, under the v4 ones --- */
    cell_put(s, YOU_COLS, y0 + 1, "\xe2\x94\x82", T_BORDER, BG_NONE, 0, 0);
    /* Re-walk to the same column the v4 list started at. */
    col = YOU_COLS + 1;
    col += cell_text(s, col, y0 + 1, w - col, " EDGE  ", T_UP, BG_NONE, 0);
    col += cell_text(s, col, y0 + 1, w - col,
                     m->colo[0] ? m->colo : "\xe2\x80\xa6", T_TEXT, BG_NONE, 0);
    for (int i = 0; i < m->edge_ip_count; i++) {
        char ip[80];
        strip_port(m->edge_ips[i], ip, sizeof ip);
        if (!is_v6(ip)) continue;
        col += cell_text(s, col, y0 + 1, w - col, "  ", FG_NONE, BG_NONE, 0);
        col += cell_text(s, col, y0 + 1, w - col, ip, T_TEXT, BG_NONE, 0);
    }
    (void)n4;
    (void)buf;
}

/* -------------------------------------------------------------- tiles */

static void draw_tiles(Screen *s, int y, int w, const Engine *e) {
    const RunResult *r = engine_result(e);
    const Meta *m = engine_meta(e);
    char lbl[32], val[64];

    int col = 0;
    int measuring = engine_phase(e) != PH_DONE && engine_phase(e) != PH_ERROR;
    int wrote = 0;

#define TILE(label_str, value_str, colour)                                  \
    do {                                                                    \
        int lw = (int)strlen(label_str) + 1;                                \
        int vw = (int)strlen(value_str);                                    \
        if (col + lw + vw + 3 > w) { col = 0;                               \
            if (wrote) y++; }                                               \
        cell_text(s, col, y, w - col, " " label_str " ", T_TEXT_DIM, BG_NONE, 0); \
        cell_text(s, col + lw, y, w - col - lw, value_str, (colour), BG_NONE, 0); \
        col += lw + vw + 3;                                                 \
        wrote = 1;                                                          \
    } while (0)

    if (r->idle.med > 0) { fmt_ms(r->idle.med, val, sizeof val); TILE("IDLE", val, T_LAT); }
    if (r->loaded_down.med > 0) { fmt_ms(r->loaded_down.med, val, sizeof val); TILE("LOADED", val, T_LAT); }
    if (r->idle.jitter > 0) { fmt_ms(r->idle.jitter, val, sizeof val); TILE("JITTER", val, T_LAT); }
    if (r->valid || measuring) {
        snprintf(val, sizeof val, "%.1f%%", r->idle.loss);
        /* Shown even at zero: hiding it when there is none makes a clean link
         * indistinguishable from a test that never probed anything. */
        const char *g = r->idle.loss <= 0.0 ? "A+"
                       : r->idle.loss <= 0.5 ? "A"
                       : r->idle.loss <= 1.0 ? "B"
                       : r->idle.loss <= 2.0 ? "C"
                       : r->idle.loss <= 5.0 ? "D" : "F";
        TILE("LOSS", val, theme_grade(g));
    }
    if (r->idle.min > 0) { fmt_ms(r->idle.min, val, sizeof val); TILE("MIN", val, T_LAT); }
    if (r->valid) {
        snprintf(lbl, sizeof lbl, "%s", r->bloat_grade);
        snprintf(val, sizeof val, "%s +%.0fms", r->bloat_grade, r->bufferbloat_ms);
        TILE("BLOAT", val, theme_grade(r->bloat_grade));
        snprintf(val, sizeof val, "%s %.0f%%", r->stab_grade, r->stability_cv);
        TILE("STAB", val, theme_grade(r->stab_grade));
    }
    if (m->dns_ms > 0) { fmt_ms(m->dns_ms, val, sizeof val); TILE("DNS", val, T_TEXT); }
    if (m->tls_ms > 0) { fmt_ms(m->tls_ms, val, sizeof val); TILE("TLS", val, T_TEXT); }
#undef TILE
}

/* ------------------------------------------------------------------- nav */

/* Selected row of the HISTORY tab; see ui_set_history_sel. */
static int g_hist_sel = 0;

void ui_set_history_sel(int row) { g_hist_sel = row < 0 ? 0 : row; }
int  ui_history_sel(void) { return g_hist_sel; }

static const char *TABS[] = {"LIVE", "CHARTS", "DETAILS", "HISTORY", "HELP"};
#define TAB_N ((int)(sizeof(TABS) / sizeof(TABS[0])))

static void draw_nav(Screen *s, int y, int w, int sel) {
    int x = 0;
    for (int i = 0; i < TAB_N; i++) {
        int tw = (int)strlen(TABS[i]) + 2;
        if (x + tw > w) break;
        if (i == sel) {
            cell_fill(s, x, y, tw, 1, T_LAT);
            cell_text(s, x + 1, y, tw, TABS[i], T_SURFACE, T_LAT, 1);
        } else {
            cell_text(s, x + 1, y, tw, TABS[i], T_TEXT, BG_NONE, 0);
        }
        x += tw + 1;
    }

    const char *keys = "space pause  +/- zoom  m actions  f test  p ping  d down  u up  r rerun  q quit";
    int kw = (int)strlen(keys);
    if (x + 4 + kw < w) cell_text(s, w - kw, y, kw, keys, T_TEXT_DIM, BG_NONE, 0);
}

void draw_tab_count(Screen *s, int y, int w, int sel) { draw_nav(s, y, w, sel); }

/* -------------------------------------------------------------- top draw */

void ui_draw(Screen *s, const Engine *e, const History *hist, int tab, int zoom) {
    screen_clear(s);
    int w = s->w, h = s->h;

    if (w < MIN_W || h < MIN_H) {
        char msg[64];
        snprintf(msg, sizeof msg, "Terminal too small\nneed at least %dx%d", MIN_W, MIN_H);
        cell_text(s, 0, 0, w, msg, T_TEXT_DIM, BG_NONE, 0);
        return;
    }

    /* Tabs 1-4 own the whole body. Dispatching before the error banner keeps
     * DETAILS and HISTORY reachable after a failed run - losing the result is
     * exactly when the user most wants to read why it failed. */
    switch (tab) {
        case 1: view_charts(s, e, hist);  draw_nav(s, h - 1, w, tab); return;
        case 2: view_details(s, e, hist); draw_nav(s, h - 1, w, tab); return;
        case 3: view_history(s, e, hist, g_hist_sel); draw_nav(s, h - 1, w, tab); return;
        case 4: view_help(s, e, hist);    draw_nav(s, h - 1, w, tab); return;
        default: break;
    }

    const char *err = engine_error(e);
    if (err[0]) {
        cell_text(s, 1, 0, w - 2, err, T_GRADE_D, BG_NONE, 0);
        return;
    }

    draw_header(s, 0, w, e);
    rule(s, 1, w);
    draw_route(s, 2, w, e);
    rule(s, 4, w);

    int y = 5;
    int chart_h = h - 9;
    if (chart_h < 4) chart_h = 4;

    int half = (w - 4) / 2;
    int nd = 0, nu = 0;
    const double *ds = engine_down_samples(e, &nd);
    const double *us = engine_up_samples(e, &nu);
    const RunResult *r = engine_result(e);

    /* Zoom keeps the tail of each series, so a higher zoom spends the same
     * chart width on a shorter window instead of rescaling the y axis. */
    if (zoom > 1) {
        int dn = nd / zoom, un = nu / zoom;
        if (dn > 0) { ds += nd - dn; nd = dn; }
        if (un > 0) { us += nu - un; nu = un; }
    }

    double dmax = r->down.max > 0 ? r->down.max : 1.0;
    double umax = r->up.max > 0 ? r->up.max : 1.0;
    /* Headroom so the peak never pegs to the top edge. */
    dmax *= 1.12;
    umax *= 1.12;

    cell_text(s, 1, y, half, " \xe2\x86\x93 DOWNLOAD", T_DOWN, BG_NONE, 0);
    cell_text(s, half + 4, y, half, " \xe2\x86\x91 UPLOAD", T_UP, BG_NONE, 0);
    y++;

    char val[32], summary[128];
    fmt_rate(engine_instant(e), val, sizeof val);
    cell_text(s, 1, y, half, val, T_TEXT_HI, BG_NONE, 0);
    y++;

    draw_chart(s, 0, y, half, chart_h, ds, nd, dmax, T_DOWN);
    draw_chart(s, half + 4, y, half, chart_h, us, nu, umax, T_UP);
    y += chart_h;

    if (nd > 0) {
        fmt_rate(r->down.avg, val, sizeof val);
        char p[32], mx[32];
        fmt_rate(r->down.p95, p, sizeof p);
        fmt_rate(r->down.max, mx, sizeof mx);
        snprintf(summary, sizeof summary, "avg %s  p95 %s  max %s", val, p, mx);
        cell_text(s, 1, y, half, summary, T_TEXT, BG_NONE, 0);
    }
    if (nu > 0) {
        fmt_rate(r->up.avg, val, sizeof val);
        char p[32], mx[32];
        fmt_rate(r->up.p95, p, sizeof p);
        fmt_rate(r->up.max, mx, sizeof mx);
        snprintf(summary, sizeof summary, "avg %s  p95 %s  max %s", val, p, mx);
        cell_text(s, half + 5, y, half, summary, T_TEXT, BG_NONE, 0);
    }
    y++;

    rule(s, y, w);
    y++;
    draw_tiles(s, y, w, e);
    rule(s, h - 1, w);
    draw_nav(s, h - 1, w, tab);
}
/* Non-live tabs: charts, details, history and help.
 *
 * The live tab in ui.c sets the house style - no boxes, structure carried by
 * whitespace and thin rules, values in one of three accents, rates and
 * milliseconds formatted the same way everywhere. Everything here follows it.
 */
#include "views.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "theme.h"

/* ------------------------------------------------------------- formatting */

/* Identical to ui.c on purpose: a number that means one thing on the live tab
 * must not mean another on the charts tab. */
static void fmt_rate(double mbps, char *out, size_t cap) {
    if (mbps >= 1000.0)      snprintf(out, cap, "%.2f Gbps", mbps / 1000.0);
    else if (mbps >= 100.0)  snprintf(out, cap, "%.0f Mbps", mbps);
    else if (mbps >= 10.0)   snprintf(out, cap, "%.1f Mbps", mbps);
    else                     snprintf(out, cap, "%.2f Mbps", mbps);
}

/* The run table has to survive a 60-column terminal, so there the unit word is
 * dropped and the decimal kept, which is what makes "912M" and "1.20G"
 * comparable at a glance. */
static void fmt_rate_tight(double mbps, char *out, size_t cap) {
    if (mbps >= 1000.0)      snprintf(out, cap, "%.2fG", mbps / 1000.0);
    else if (mbps >= 100.0)  snprintf(out, cap, "%.0fM", mbps);
    else if (mbps >= 10.0)   snprintf(out, cap, "%.1fM", mbps);
    else                     snprintf(out, cap, "%.2fM", mbps);
}

static void fmt_ms(double ms, char *out, size_t cap) {
    if (ms >= 100.0)     snprintf(out, cap, "%.0fms", ms);
    else if (ms >= 10.0) snprintf(out, cap, "%.1fms", ms);
    else                 snprintf(out, cap, "%.2fms", ms);
}

static void fmt_pct(double pct, char *out, size_t cap) {
    snprintf(out, cap, "%.2f%%", pct);
}

static void fmt_bytes(uint64_t b, char *out, size_t cap) {
    if (b >= (uint64_t)1 << 30)      snprintf(out, cap, "%.2f GiB", (double)b / 1073741824.0);
    else if (b >= (uint64_t)1 << 20) snprintf(out, cap, "%.1f MiB", (double)b / 1048576.0);
    else if (b >= (uint64_t)1 << 10) snprintf(out, cap, "%.0f KiB", (double)b / 1024.0);
    else                             snprintf(out, cap, "%llu B", (unsigned long long)b);
}

static void fmt_clock(uint64_t ms, char *out, size_t cap) {
    uint64_t total = ms / 1000;
    snprintf(out, cap, "%d:%02d", (int)(total / 60), (int)(total % 60));
}

/* RunResult.timestamp_ms is Unix epoch milliseconds, so the table shows the
 * wall clock of the run rather than an elapsed offset that would be
 * meaningless a day later. */
static void fmt_stamp(uint64_t epoch_ms, char *out, size_t cap) {
    time_t secs = (time_t)(epoch_ms / 1000u);
    struct tm tmv;
#if defined(_WIN32)
    if (localtime_s(&tmv, &secs) != 0) { snprintf(out, cap, "--:--:--"); return; }
#else
    if (localtime_r(&secs, &tmv) == NULL) { snprintf(out, cap, "--:--:--"); return; }
#endif
    snprintf(out, cap, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* A trailing :443 makes the route line unreadable and adds nothing here. */
static void strip_port(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if (n >= 4 && strcmp(in + n - 4, ":443") == 0) n -= 4;
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

static int is_v6(const char *ip) { return strchr(ip, ':') != NULL; }

/* Addresses joined into one cell. Cutting the list off mid-address would print
 * a wrong address, so once the budget is gone the remainder collapses to a
 * count instead. */
static void join_list(char *out, size_t cap, const char *const *items, int n, int budget) {
    int shown = 0;
    if (budget < 12) budget = 12;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t l = strlen(items[i]);
        size_t used = strlen(out);
        int sep = shown ? 2 : 0;
        /* Room for the trailing "+N" is reserved up front, otherwise the
         * count itself gets truncated off the right edge. */
        if (shown > 0 && (int)(used + (size_t)sep + l + 8u) > budget) break;
        if (used + (size_t)sep + l + 1u > cap) break;
        if (sep) { memcpy(out + used, ", ", 2); used += 2; }
        memcpy(out + used, items[i], l);
        used += l;
        out[used] = '\0';
        shown++;
    }
    if (shown < n) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s+%d", shown ? ", " : "", n - shown);
    }
}

/* ------------------------------------------------------------- history in */

/* The whole of the run log's surface, in two places. History.runs is a
 * MAX_RUNS array by value, several megabytes in all, so only ever a pointer
 * to one crosses this boundary. The count is clamped to the array: it comes
 * off disk, and a hand-edited or truncated log must not index past the end. */
static int hist_count(const History *h) {
    if (!h) return 0;
    if (h->count < 0) return 0;
    return h->count > MAX_RUNS ? MAX_RUNS : h->count;
}

static const RunResult *hist_run(const History *h, int i) {
    if (!h || i < 0 || i >= hist_count(h)) return NULL;
    return &h->runs[i];
}

/* ------------------------------------------------------------- page frame */

/* Body rows between the title strip and the tab bar. */
static int body_rows(const Screen *s) {
    int n = s->h - VIEWS_BODY_TOP - VIEWS_BODY_BOT;
    return n > 0 ? n : 0;
}

/* One line and nothing else. Anything more would be drawn into cells that do
 * not exist, or would claim space the tab has not got. */
static int too_small(Screen *s) {
    if (s->w >= VIEWS_MIN_W && s->h >= VIEWS_MIN_H) return 0;
    char msg[96];
    snprintf(msg, sizeof msg, "terminal too small - need at least %dx%d",
             VIEWS_MIN_W, VIEWS_MIN_H);
    int max = s->w - 2;
    if (max < 1) max = 1;
    cell_text(s, 1, 0, max, msg, T_TEXT_DIM, BG_NONE, 0);
    return 1;
}

static void rule(Screen *s, int y) {
    for (int i = 0; i < s->w; i++) cell_put(s, i, y, "\xe2\x94\x80", T_BORDER, BG_NONE, 0, 0);
}

/* Right-hand status on the title strip, e.g. the phase and the clock. */
static void page_head(Screen *s, const char *title, const char *status) {
    int max = s->w - 2;
    if (max < 1) return;
    cell_text(s, 1, 0, max, title, T_TEXT_HI, BG_NONE, 1);
    if (status && status[0]) {
        int n = (int)strlen(status);
        if (n < max) cell_text(s, s->w - n - 1, 0, n, status, T_TEXT_DIM, BG_NONE, 0);
    }
    rule(s, 1);
}

/* ---------------------------------------------------------------- charts */

#define BLOCK "\xe2\x96\x88" /* U+2588 FULL BLOCK */

/* ui.c's bar graph, unchanged in behaviour: one column per sample, the
 * leading edge of each bar brightened so the current value is findable
 * without reading the axis, and buckets taking the peak rather than the mean
 * because averaging spikes away is how a speed trace loses the thing it
 * exists to show. */
static void draw_chart(Screen *s, int x, int y, int w, int h, const double *data,
                       int n, double max, uint32_t accent, const char *unit) {
    if (w <= 0 || h <= 0) return;
    char label[32];

    for (int i = 0; i < w; i++)
        cell_put(s, x + i, y + h / 2, "\xc2\xb7", T_BORDER, BG_NONE, 0, 0);

    if (max <= 0.0) max = 1.0;

    for (int i = 0; i < w; i++) {
        double v;
        if (n >= w) {
            int lo = (int)((long)i * n / w);
            int hi = (int)((long)(i + 1) * n / w);
            if (hi <= lo) hi = lo + 1;
            if (hi > n) hi = n;
            if (lo >= n) break;
            v = 0.0;
            for (int k = lo; k < hi; k++)
                if (data[k] > v) v = data[k];
        } else {
            if (i >= n) break;
            v = data[i];
        }
        int filled = (int)((v / max) * (double)h + 0.5);
        if (filled > h) filled = h;
        if (filled < 0) filled = 0;
        int top = h - filled;
        for (int cy = top; cy < h; cy++) {
            uint32_t col = (cy == top) ? accent : theme_shade(accent, 0.72);
            cell_put(s, x + i, y + cy, BLOCK, col, BG_NONE, 0, 0);
        }
    }

    snprintf(label, sizeof label, "%d%s", (int)(max + 0.5), unit);
    cell_text(s, x, y, 8, label, T_TEXT_DIM, BG_NONE, 0);
    snprintf(label, sizeof label, "%d", (int)(max / 2.0 + 0.5));
    cell_text(s, x, y + h / 2, 8, label, T_TEXT_DIM, BG_NONE, 0);
    cell_text(s, x, y + h - 1, 8, "0", T_TEXT_DIM, BG_NONE, 0);
}

static double peak_of(const double *d, int n) {
    double m = 0.0;
    for (int i = 0; i < n; i++)
        if (d[i] > m) m = d[i];
    return m;
}

void view_charts(Screen *s, const Engine *e, const History *h) {
    (void)h;
    if (!s || !e) return;
    if (too_small(s)) return;

    const int w = s->w;
    const int rows = body_rows(s);
    const RunResult *r = engine_result(e);

    char status[64];
    fmt_clock(engine_elapsed_ms(e), status, sizeof status);
    {
        size_t l = strlen(status);
        snprintf(status + l, sizeof status - l, " \xc2\xb7 %s", phase_label(engine_phase(e)));
    }
    page_head(s, " CHARTS", status);

    int nd = 0, nu = 0, np = 0;
    const double *ds = engine_down_samples(e, &nd);
    const double *us = engine_up_samples(e, &nu);
    const double *ps = engine_probe_samples(e, &np);
    if (!ds) nd = 0;
    if (!us) nu = 0;
    if (!ps) np = 0;

    /* Labels, values, summaries, the spacer and the latency caption are the
     * fixed cost; everything left over goes to the bars, two thirds of it to
     * the pair above and the rest to the latency trace below. */
    const int chrome = 6;
    int bars = rows - chrome;
    if (bars < 8) bars = 8;
    int h1 = bars * 2 / 3;
    int h2 = bars - h1;
    if (h2 < 4) { h2 = 4; if (h1 > bars - h2) h1 = bars - h2; }
    if (h1 < 4) h1 = 4;

    const int half = (w - 4) / 2;
    int y = VIEWS_BODY_TOP;

    /* Headroom so the peak never pegs to the top edge. Each axis is scaled to
     * its own series: download, upload and latency share no unit. */
    double dmax = r->down.max > 0.0 ? r->down.max : 1.0;
    double umax = r->up.max > 0.0 ? r->up.max : 1.0;
    dmax *= 1.12;
    umax *= 1.12;
    double pmax = peak_of(ps, np);
    if (pmax <= 0.0) pmax = 10.0;   /* an axis with no data still needs a scale */
    pmax *= 1.25;

    char val[32], p95[32], mx[32], sum[160];

    cell_text(s, 1, y, half, " \xe2\x86\x93 DOWNLOAD", T_DOWN, BG_NONE, 0);
    cell_text(s, half + 4, y, half, " \xe2\x86\x91 UPLOAD", T_UP, BG_NONE, 0);
    y++;

    /* engine_instant tracks the download side only, so the upload column is
     * headed by its own most recent reading rather than by a second number
     * that would quietly mean the same thing twice. */
    fmt_rate(engine_instant(e), val, sizeof val);
    cell_text(s, 1, y, half, val, T_TEXT_HI, BG_NONE, 0);
    if (nu > 0) fmt_rate(us[nu - 1], val, sizeof val);
    else        snprintf(val, sizeof val, "-");
    cell_text(s, half + 5, y, half, val, T_TEXT_HI, BG_NONE, 0);
    y++;

    draw_chart(s, 0, y, half, h1, ds, nd, dmax, T_DOWN, "");
    draw_chart(s, half + 4, y, half, h1, us, nu, umax, T_UP, "");
    y += h1;

    if (nd > 0) {
        fmt_rate(r->down.avg, val, sizeof val);
        fmt_rate(r->down.p95, p95, sizeof p95);
        fmt_rate(r->down.max, mx, sizeof mx);
        snprintf(sum, sizeof sum, "avg %s  p95 %s  max %s", val, p95, mx);
        cell_text(s, 1, y, half, sum, T_TEXT, BG_NONE, 0);
    } else {
        cell_text(s, 1, y, half, "no samples yet", T_TEXT_DIM, BG_NONE, 0);
    }
    if (nu > 0) {
        fmt_rate(r->up.avg, val, sizeof val);
        fmt_rate(r->up.p95, p95, sizeof p95);
        fmt_rate(r->up.max, mx, sizeof mx);
        snprintf(sum, sizeof sum, "avg %s  p95 %s  max %s", val, p95, mx);
        cell_text(s, half + 5, y, half, sum, T_TEXT, BG_NONE, 0);
    } else {
        cell_text(s, half + 5, y, half, "no samples yet", T_TEXT_DIM, BG_NONE, 0);
    }
    y++;

    y++; /* spacer: the latency trace is a different quantity and reads better
          * separated from the throughput pair than stacked against it */

    cell_text(s, 1, y, w - 2, " \xe2\x86\x93 LATENCY", T_LAT, BG_NONE, 0);
    fmt_ms(engine_probe_instant(e), val, sizeof val);
    cell_text(s, half + 5, y, half, val, T_TEXT_HI, BG_NONE, 0);
    y++;

    draw_chart(s, 0, y, w, h2, ps, np, pmax, T_LAT, "ms");
    y += h2;

    if (np > 0) {
        fmt_ms(r->idle.min, val, sizeof val);
        fmt_ms(r->idle.med, p95, sizeof p95);
        fmt_ms(r->idle.max, mx, sizeof mx);
        snprintf(sum, sizeof sum, "min %s  med %s  max %s  \xc2\xb7  %d probes",
                 val, p95, mx, np);
        cell_text(s, 1, y, w - 2, sum, T_TEXT, BG_NONE, 0);
    } else {
        cell_text(s, 1, y, w - 2, "no probes yet", T_TEXT_DIM, BG_NONE, 0);
    }
}

/* ---------------------------------------------------------------- details */

/* Label field is seven columns, one space either side, so a seven-character
 * label still leaves a gap before its value and the grid holds together. */
#define LBL_W 9
#define DROWS 40

typedef struct {
    int         section; /* 1 = a heading, 0 = a label/value pair */
    const char *label;
    char        title[24];
    /* Sized to hold the longest source it is ever given, so the copy below is
     * provably a copy and not a truncation. */
    char        value[160];
    uint32_t    colour;
} DRow;

static void dsection(DRow *rows, int *n, const char *title) {
    if (*n >= DROWS) return;
    rows[*n].section = 1;
    rows[*n].label = NULL;
    snprintf(rows[*n].title, sizeof rows[*n].title, "%s", title);
    rows[*n].value[0] = '\0';
    rows[*n].colour = T_TEXT_HI;
    (*n)++;
}

/* An unknown value is dropped rather than printed as a zero: a row reading
 * "DNS 0.00ms" on a run where the lookup never happened is worse than no row.
 *
 * `len` is the size of the buffer `text` points into, passed as sizeof at the
 * call site. Handing a bounded copy to a %s conversion loses the bound as soon
 * as the source is a plain pointer, which is every Meta field; a memchr bound
 * keeps it, and keeps the read inside the field even if the field is full.
 */
static void drow(DRow *rows, int *n, const char *label, const char *text,
                 size_t len, uint32_t colour) {
    if (*n >= DROWS) return;
    size_t cap = sizeof rows[*n].value - 1;
    const char *z;
    if (!text || len == 0) return;
    if (len > cap) len = cap;
    z = memchr(text, '\0', len);
    if (z) len = (size_t)(z - text);
    if (len == 0) return;
    rows[*n].section = 0;
    rows[*n].label = label;
    memcpy(rows[*n].value, text, len);
    rows[*n].value[len] = '\0';
    rows[*n].colour = colour;
    (*n)++;
}

static void dms(DRow *rows, int *n, const char *label, double ms) {
    char v[32];
    fmt_ms(ms, v, sizeof v);
    drow(rows, n, label, v, sizeof v, T_LAT);
}

/* Cut to the rows that fit, then drop a heading that lost its members: a
 * section title with nothing under it reads as a rendering fault rather than
 * as a measurement that was not taken. The count that falls off the end is
 * reported on the title strip rather than silently swallowed. */
static int dtrim(DRow *rows, int n, int avail) {
    if (n > avail) n = avail;
    while (n > 0 && rows[n - 1].section) n--;
    return n;
}

static void ddraw(Screen *s, int x, int vw, int y0, DRow *rows, int n) {
    char lbl[16];
    for (int i = 0; i < n; i++) {
        int y = y0 + i;
        if (rows[i].section) {
            cell_text(s, x + 1, y, vw - 1, rows[i].title, T_TEXT_HI, BG_NONE, 1);
        } else {
            /* Same shape as ui.c's tiles: a padded label, then the value on the
             * shared column. Every label is at most seven characters. */
            snprintf(lbl, sizeof lbl, " %-7s ", rows[i].label ? rows[i].label : "");
            cell_text(s, x, y, LBL_W, lbl, T_TEXT_DIM, BG_NONE, 0);
            cell_text(s, x + LBL_W, y, vw, rows[i].value, rows[i].colour, BG_NONE, 0);
        }
    }
}

/* Fills `out` with the stripped addresses of one family and returns how many
 * it found. The count arrives over the wire and the array is eight deep, so
 * both ends are clamped before anything is read. */
static int build_edges(const Meta *m, int v6, char out[][80]) {
    int n = 0;
    int total = m->edge_ip_count;
    if (total > 8) total = 8;
    if (total < 0) total = 0;
    for (int i = 0; i < total && n < 8; i++) {
        strip_port(m->edge_ips[i], out[n], 80);
        if (!out[n][0]) continue;
        if (is_v6(out[n]) != v6) continue;
        n++;
    }
    return n;
}

void view_details(Screen *s, const Engine *e, const History *h) {
    (void)h;
    if (!s || !e) return;
    if (too_small(s)) return;

    const int w = s->w;
    const int avail = body_rows(s);
    const RunResult *r = engine_result(e);
    const Meta *m = engine_meta(e);

    DRow left[DROWS], right[DROWS];
    int nl = 0, nr = 0;
    char v[128];

    /* --- left: the measurements themselves --- */
    if (r->idle.samples > 0 || r->idle.med > 0.0) {
        dsection(left, &nl, "IDLE LATENCY");
        dms(left, &nl, "AVG", r->idle.avg);
        dms(left, &nl, "MIN", r->idle.min);
        dms(left, &nl, "MED", r->idle.med);
        dms(left, &nl, "P95", r->idle.p95);
        dms(left, &nl, "P99", r->idle.p99);
        dms(left, &nl, "JITTER", r->idle.jitter);
        fmt_pct(r->idle.loss, v, sizeof v);
        /* Loss is the one figure shown at zero: hiding it when there is none
         * makes a clean link indistinguishable from a test that never probed. */
        drow(left, &nl, "LOSS", v, sizeof v, theme_grade(r->idle.loss <= 0.0 ? "A+" :
                                              r->idle.loss <= 0.5 ? "A" :
                                              r->idle.loss <= 1.0 ? "B" :
                                              r->idle.loss <= 2.0 ? "C" :
                                              r->idle.loss <= 5.0 ? "D" : "F"));
        snprintf(v, sizeof v, "%d", r->idle.samples);
        drow(left, &nl, "PROBES", v, sizeof v, T_TEXT);
    }

    if (r->down.nsamples > 0 || r->down.avg > 0.0) {
        dsection(left, &nl, "DOWNLOAD");
        fmt_rate(r->down.avg, v, sizeof v);
        drow(left, &nl, "AVG", v, sizeof v, T_DOWN);
        fmt_rate(r->down.p95, v, sizeof v);
        drow(left, &nl, "P95", v, sizeof v, T_TEXT);
        fmt_rate(r->down.max, v, sizeof v);
        drow(left, &nl, "MAX", v, sizeof v, T_TEXT_HI);
        fmt_bytes(r->down.bytes, v, sizeof v);
        drow(left, &nl, "BYTES", v, sizeof v, T_TEXT);
        snprintf(v, sizeof v, "%d", r->down.nsamples);
        drow(left, &nl, "SAMPLES", v, sizeof v, T_TEXT_DIM);
    }

    if (r->up.nsamples > 0 || r->up.avg > 0.0) {
        dsection(left, &nl, "UPLOAD");
        fmt_rate(r->up.avg, v, sizeof v);
        drow(left, &nl, "AVG", v, sizeof v, T_UP);
        fmt_rate(r->up.p95, v, sizeof v);
        drow(left, &nl, "P95", v, sizeof v, T_TEXT);
        fmt_rate(r->up.max, v, sizeof v);
        drow(left, &nl, "MAX", v, sizeof v, T_TEXT_HI);
        fmt_bytes(r->up.bytes, v, sizeof v);
        drow(left, &nl, "BYTES", v, sizeof v, T_TEXT);
        snprintf(v, sizeof v, "%d", r->up.nsamples);
        drow(left, &nl, "SAMPLES", v, sizeof v, T_TEXT_DIM);
    }

    /* --- right: the conditions those numbers were measured under --- */
    if (r->loaded_down.samples > 0 || r->loaded_down.med > 0.0) {
        dsection(right, &nr, "LOADED LATENCY");
        dms(right, &nr, "AVG", r->loaded_down.avg);
        dms(right, &nr, "MIN", r->loaded_down.min);
        dms(right, &nr, "MED", r->loaded_down.med);
        dms(right, &nr, "P99", r->loaded_down.p99);
        /* The rise is only meaningful against a baseline that was actually
         * taken: a download-only run has no idle window to subtract. */
        if (r->idle.med > 0.0) {
            snprintf(v, sizeof v, "%.0fms", r->loaded_down.avg - r->idle.med);
            drow(right, &nr, "RISE", v, sizeof v, T_TEXT_DIM);
        }
    }

    if (r->valid) {
        dsection(right, &nr, "QUALITY");
        snprintf(v, sizeof v, "%.3s +%.0fms", r->bloat_grade, r->bufferbloat_ms);
        drow(right, &nr, "BLOAT", v, sizeof v, theme_grade(r->bloat_grade));
        snprintf(v, sizeof v, "%.3s %.2f%%", r->stab_grade, r->stability_cv);
        drow(right, &nr, "STAB CV", v, sizeof v, theme_grade(r->stab_grade));
    }

    dsection(right, &nr, "CONNECTION");
    drow(right, &nr, "COLO", m->colo, sizeof m->colo, T_UP);
    drow(right, &nr, "CLIENT", m->ip, sizeof m->ip, T_TEXT_HI);
    drow(right, &nr, "ASN ORG", m->asn_org, sizeof m->asn_org, T_TEXT);
    if (m->asn[0]) {
        snprintf(v, sizeof v, "AS%.15s", m->asn);
        drow(right, &nr, "ASN", v, sizeof v, T_TEXT);
    }
    drow(right, &nr, "COUNTRY", m->loc, sizeof m->loc, T_TEXT);
    if (m->dns_ms > 0.0) dms(right, &nr, "DNS", m->dns_ms);
    if (m->tls_ms > 0.0) dms(right, &nr, "TLS", m->tls_ms);
    drow(right, &nr, "TLS VER", m->tls, sizeof m->tls, T_TEXT);
    drow(right, &nr, "CIPHER", m->tls_cipher, sizeof m->tls_cipher, T_TEXT);
    drow(right, &nr, "HTTP", m->http, sizeof m->http, T_TEXT);
    drow(right, &nr, "WARP", m->warp, sizeof m->warp, T_TEXT);

    /* Two columns share the width, so each side gets whatever the split
     * leaves; the address list is fitted to that, not to a guessed constant. */
    const int mid = w / 2;
    const int right_x = mid + 2;
    const int left_vw = mid - 1 - LBL_W;
    const int right_vw = w - (right_x + LBL_W);

    char store4[8][80], store6[8][80];
    const char *list4[8], *list6[8];
    int n4 = build_edges(m, 0, store4);
    int n6 = build_edges(m, 1, store6);
    for (int i = 0; i < n4; i++) list4[i] = store4[i];
    for (int i = 0; i < n6; i++) list6[i] = store6[i];

    if (n4 > 0 || n6 > 0) {
        dsection(right, &nr, "EDGE");
        if (n4 > 0) {
            join_list(v, sizeof v, list4, n4, right_vw);
            drow(right, &nr, "EDGE V4", v, sizeof v, T_TEXT);
        }
        if (n6 > 0) {
            join_list(v, sizeof v, list6, n6, right_vw);
            drow(right, &nr, "EDGE V6", v, sizeof v, T_TEXT);
        }
    }

    int lrows = dtrim(left, nl, avail);
    int rrows = dtrim(right, nr, avail);
    int hidden = (nl - lrows) + (nr - rrows);

    char status[96];
    if (r->valid) {
        /* Precision limits, not decoration: these come off the wire and the
         * status strip has a fixed width to live in. */
        snprintf(status, sizeof status, "%.15s \xc2\xb7 %.3s \xc2\xb7 %.3s",
                 m->colo[0] ? m->colo : "\xe2\x80\xa6",
                 r->bloat_grade[0] ? r->bloat_grade : "-",
                 r->stab_grade[0] ? r->stab_grade : "-");
    } else {
        snprintf(status, sizeof status, "%.15s \xc2\xb7 running",
                 m->colo[0] ? m->colo : "\xe2\x80\xa6");
    }
    if (hidden > 0) {
        size_t l = strlen(status);
        snprintf(status + l, sizeof status - l, " \xc2\xb7 +%d more", hidden);
    }
    page_head(s, " DETAILS", status);

    ddraw(s, 0, left_vw, VIEWS_BODY_TOP, left, lrows);
    ddraw(s, right_x, right_vw, VIEWS_BODY_TOP, right, rrows);

    /* The same thin rule the live tab uses to separate the route strip. */
    for (int y = VIEWS_BODY_TOP; y < VIEWS_BODY_TOP + avail && y < s->h - VIEWS_BODY_BOT; y++)
        cell_put(s, mid, y, "\xe2\x94\x82", T_BORDER, BG_NONE, 0, 0);
}

/* ---------------------------------------------------------------- history */

/* Column geometry, tuned to fit VIEWS_MIN_W exactly with a small right
 * margin. Values are right-aligned so the digits form a column of their own. */
enum {
    H_TIME_X = 0,  H_TIME_W = 8,
    H_DOWN_X = 8,  H_DOWN_W = 8,
    H_UP_X = 16,   H_UP_W = 8,
    H_IDLE_X = 24, H_IDLE_W = 8,
    H_JIT_X = 32,  H_JIT_W = 8,
    H_LOSS_X = 40, H_LOSS_W = 6,
    H_BF_X = 46,   H_BF_W = 6,
    H_ST_X = 52,   H_ST_W = 5
};

static void hcol(Screen *s, int y, int x, int width, const char *val,
                 uint32_t fg, uint32_t bg, int right_align) {
    int n = (int)strlen(val);
    int sx = right_align ? x + width - n : x;
    if (sx < x) sx = x;
    int room = x + width - sx;
    if (room < 0) room = 0;
    if (room < n) n = room;
    cell_text(s, sx, y, n, val, fg, bg, bg != BG_NONE);
}

void view_history(Screen *s, const Engine *e, const History *h, int sel) {
    (void)e;
    if (!s) return;
    if (too_small(s)) return;

    const int w = s->w;
    const int y0 = VIEWS_BODY_TOP;
    const int count = hist_count(h);
    const int rows = body_rows(s) - 1;

    int sel_c = sel;
    if (sel_c < 0) sel_c = 0;
    if (sel_c >= count) sel_c = count > 0 ? count - 1 : 0;

    /* Scroll just far enough to keep the selection on screen, then clamp so
     * the table never sits half empty at the end of the log. */
    int first = 0;
    if (count > rows) {
        first = sel_c - rows + 1;
        if (first < 0) first = 0;
        if (first > count - rows) first = count - rows;
    }

    /* Both facts about the log live on the title strip. The table header row
     * is dense out to the last column and has no room to give. */
    char status[48];
    if (count > rows && count > 0)
        snprintf(status, sizeof status, "%d of %d shown", rows, count);
    else if (count > 0)
        snprintf(status, sizeof status, "%d run%s", count, count == 1 ? "" : "s");
    else
        snprintf(status, sizeof status, "no runs yet");
    page_head(s, " HISTORY", status);

    static const struct { int x, w; const char *label; } HEAD[] = {
        { H_TIME_X, H_TIME_W, "TIME"    },
        { H_DOWN_X, H_DOWN_W, "DOWN"    },
        { H_UP_X,   H_UP_W,   "UP"      },
        { H_IDLE_X, H_IDLE_W, "IDLE"    },
        { H_JIT_X,  H_JIT_W,  "JITTER"  },
        { H_LOSS_X, H_LOSS_W, "LOSS"    },
        { H_BF_X,   H_BF_W,   "BLOAT"   },
        { H_ST_X,   H_ST_W,   "STAB"    }
    };
    for (unsigned i = 0; i < sizeof HEAD / sizeof HEAD[0]; i++)
        cell_text(s, HEAD[i].x, y0, HEAD[i].w, HEAD[i].label, T_TEXT_DIM, BG_NONE, 0);

    if (rows < 1) return;

    if (count == 0) {
        cell_text(s, 1, y0 + 2, w - 2, "nothing recorded yet - press r to run a test",
                  T_TEXT_DIM, BG_NONE, 0);
        return;
    }

    for (int i = 0; i < rows; i++) {
        int idx = first + i;
        if (idx >= count) break;
        const RunResult *r = hist_run(h, idx);
        if (!r) continue;
        int y = y0 + 1 + i;
        /* The inverse highlight: one of the few backgrounds in the app that
         * are not chrome, because the selection has to read as a selection. */
        uint32_t bg = (idx == sel_c) ? T_SURFACE : BG_NONE;
        uint32_t fg = (idx == sel_c) ? T_TEXT_HI  : T_TEXT;

        if (bg != BG_NONE) cell_fill(s, 0, y, w, 1, bg);

        char v[32];
        fmt_stamp(r->timestamp_ms, v, sizeof v);
        hcol(s, y, H_TIME_X, H_TIME_W, v, T_TEXT_DIM, bg, 0);

        if (r->down.avg > 0.0) fmt_rate_tight(r->down.avg, v, sizeof v);
        else                   snprintf(v, sizeof v, "-");
        hcol(s, y, H_DOWN_X, H_DOWN_W, v, fg, bg, 1);

        if (r->up.avg > 0.0) fmt_rate_tight(r->up.avg, v, sizeof v);
        else                  snprintf(v, sizeof v, "-");
        hcol(s, y, H_UP_X, H_UP_W, v, fg, bg, 1);

        if (r->idle.med > 0.0) fmt_ms(r->idle.med, v, sizeof v);
        else                   snprintf(v, sizeof v, "-");
        hcol(s, y, H_IDLE_X, H_IDLE_W, v, T_LAT, bg, 1);

        if (r->idle.jitter > 0.0) fmt_ms(r->idle.jitter, v, sizeof v);
        else                      snprintf(v, sizeof v, "-");
        hcol(s, y, H_JIT_X, H_JIT_W, v, T_TEXT, bg, 1);

        /* A run that never probed anything reports 0.00% loss, which is not
         * the same as having measured zero loss. */
        if (r->idle.samples > 0) fmt_pct(r->idle.loss, v, sizeof v);
        else                     snprintf(v, sizeof v, "-");
        hcol(s, y, H_LOSS_X, H_LOSS_W, v, T_TEXT, bg, 1);

        hcol(s, y, H_BF_X, H_BF_W, r->bloat_grade[0] ? r->bloat_grade : "-",
             r->bloat_grade[0] ? theme_grade(r->bloat_grade) : T_TEXT_DIM, bg, 1);
        hcol(s, y, H_ST_X, H_ST_W, r->stab_grade[0] ? r->stab_grade : "-",
             r->stab_grade[0] ? theme_grade(r->stab_grade) : T_TEXT_DIM, bg, 1);
    }
}

/* ------------------------------------------------------------------- help */

#define KEY_W 12

typedef struct {
    const char *key;
    const char *desc;
} KeyRow;

static const KeyRow KEYS_A[] = {
    { "1",         "live view" },
    { "2",         "charts" },
    { "3",         "details" },
    { "4",         "run history" },
    { "5",         "this help" },
    { "?",         "this help" },
    { "tab",       "next view" },
    { "shift+tab", "previous view" },
    { "space",     "pause charts" },
    { "+",         "zoom in" },
    { "-",         "zoom out" }
};

static const KeyRow KEYS_B[] = {
    { "f", "full test" },
    { "p", "ping only" },
    { "d", "download only" },
    { "u", "upload only" },
    { "r", "restart test" },
    { "m", "actions menu" },
    { "shift+H", "reveal terminal" },
    { "q", "quit" },
    { "esc", "quit" }
};

static void keytable(Screen *s, int x, int dw, int y0, int avail,
                     const KeyRow *keys, int n) {
    int drawn = 0;
    for (int i = 0; i < n && drawn < avail; i++, drawn++) {
        cell_text(s, x + 1, y0 + i, KEY_W - 1, keys[i].key, T_LAT, BG_NONE, 1);
        cell_text(s, x + KEY_W, y0 + i, dw, keys[i].desc, T_TEXT, BG_NONE, 0);
    }
}

void view_help(Screen *s, const Engine *e, const History *h) {
    (void)e;
    (void)h;
    if (!s) return;
    if (too_small(s)) return;

    const int w = s->w;
    const int rows = body_rows(s);
    page_head(s, " HELP", NULL);

    /* The note is the point of the tab as much as the key list is, so the
     * rows are reserved from the bottom up rather than appended if space
     * happens to be left. */
    const int note_h = 3;
    int table_rows = rows - note_h;
    if (table_rows < 1) table_rows = 1;

    const int mid = w / 2;
    const int right_x = mid + 2;
    const int left_dw = mid - 1 - KEY_W;
    const int right_dw = w - (right_x + KEY_W);

    keytable(s, 0, left_dw, VIEWS_BODY_TOP, table_rows,
             KEYS_A, (int)(sizeof KEYS_A / sizeof KEYS_A[0]));
    keytable(s, right_x, right_dw, VIEWS_BODY_TOP, table_rows,
             KEYS_B, (int)(sizeof KEYS_B / sizeof KEYS_B[0]));

    for (int y = VIEWS_BODY_TOP; y < VIEWS_BODY_TOP + table_rows && y < s->h; y++)
        cell_put(s, mid, y, "\xe2\x94\x82", T_BORDER, BG_NONE, 0, 0);

    int y = s->h - VIEWS_BODY_BOT - note_h;
    if (y > VIEWS_BODY_TOP + table_rows) y = VIEWS_BODY_TOP + table_rows;
    if (y < 0) return;

    rule(s, y);
    static const char *NOTE[] = {
        "No background is ever painted - every cell keeps the",
        "terminal's own background, so a theme shows through."
    };
    for (int i = 0; i < 2 && y + 1 + i < s->h - VIEWS_BODY_BOT; i++)
        cell_text(s, 1, y + 1 + i, w - 2, NOTE[i], T_TEXT_DIM, BG_NONE, 0);
}
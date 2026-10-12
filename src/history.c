#include "history.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define CFS_SEP      '\\'
#  define CFS_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define CFS_SEP      '/'
#  define CFS_MKDIR(p) mkdir((p), 0777)
#endif

#define PATH_CAP 512
#define LINE_CAP 16384
#define NUM_CAP  64

/* A record carries the context strings and the full address list, so its worst
 * case is every field escaped at six bytes per character: roughly 7 KiB. The
 * line buffer has to cover that or a record with an unprintable byte in an org
 * name would be dropped, and a dropped record is silent data loss. */
#define FILE_CAP (4u * 1024u * 1024u)

/* Worst case for one field is \u00xx per byte. Every buffer is sized from the
 * destination it feeds, so a truncated escape can never run past it. */
#define ESC_CAP(n) ((n) * 6 + 1)

/* Meta.edge_ips, named once so the writer's scratch, the writer's clamp and
 * the parser's clamp cannot disagree. sizeof never evaluates its operand, so
 * this stays a constant expression despite reaching through a null pointer. */
#define EDGE_SLOTS ((int)(sizeof ((Meta *)0)->edge_ips / sizeof ((Meta *)0)->edge_ips[0]))
#define EDGE_WIDTH  (sizeof ((Meta *)0)->edge_ips[0])

/* Enough for one escaped latency window: ten fixed numbers, each at %.4f of a
 * worst-case double, plus the keys. */
#define LAT_CAP 512

/* ------------------------------------------------------------------- path */

static char g_path[PATH_CAP];
static int  g_path_ready;

static const char *base_dir(void) {
    const char *p;
#if defined(_WIN32)
    p = getenv("LOCALAPPDATA");
    if (p && *p) return p;
    p = getenv("USERPROFILE");
    if (p && *p) return p;
#else
    p = getenv("HOME");
    if (p && *p) return p;
    p = getenv("XDG_DATA_HOME");
    if (p && *p) return p;
#endif
    /* Sandboxes and service accounts export neither; a relative path keeps
     * history working beside the binary instead of failing every save. */
    return ".";
}

const char *history_path(void) {
    if (g_path_ready) return g_path;
    g_path_ready = 1; /* the environment cannot change under a running app */
    int n = snprintf(g_path, sizeof g_path, "%s%ccfspeed%chistory.json",
                     base_dir(), CFS_SEP, CFS_SEP);
    if (n < 0 || (size_t)n >= sizeof g_path) g_path[0] = '\0';
    return g_path;
}

static int mkdir_one(const char *p) {
    if (CFS_MKDIR(p) == 0) return 0;
    /* Every step of the chain is created, so an existing one counts as done. */
    return errno == EEXIST ? 0 : -1;
}

static int mkdirs(const char *dir) {
    char buf[PATH_CAP];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, dir, n + 1);

    size_t i = 1; /* never split the leading separator off a path */
#if defined(_WIN32)
    if (n >= 3 && buf[1] == ':') i = 3; /* "C:\" has to stay intact */
#endif
    for (; i < n; i++) {
        if (buf[i] != CFS_SEP || buf[i - 1] == CFS_SEP) continue;
        buf[i] = '\0';
        int rc = mkdir_one(buf);
        buf[i] = CFS_SEP;
        if (rc != 0) return -1;
    }
    return mkdir_one(buf);
}

/* ----------------------------------------------------------------- writer */

/* JSON has no spelling for NaN or infinity, and a very short measurement
 * divides by zero often enough to produce both. */
static double finite_or_zero(double v) {
    return isfinite(v) ? v : 0.0;
}

static void esc(const char *s, char *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)s;
    size_t o = 0;
    for (; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            default: break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (o + l + 1 > cap) break;
            memcpy(out + o, rep, l);
            o += l;
            continue;
        }
        if (*p < 0x20) {
            /* Unreachable from our own fields, but a control byte smuggled in
             * through a response header must not corrupt the line. */
            char u[8];
            int l = snprintf(u, sizeof u, "\\u%04x", (unsigned)*p);
            if (l > 0 && o + (size_t)l + 1 <= cap) {
                memcpy(out + o, u, (size_t)l);
                o += (size_t)l;
            }
            continue;
        }
        /* Bytes above 0x7F are already valid UTF-8 and pass through. */
        if (o + 2 > cap) break;
        out[o++] = (char)*p;
    }
    out[o] = '\0';
}

/* One latency window, spelled once for both windows so a consumer can compare
 * them field by field. Same keys, same order and same precision as cli.c's
 * emit_lat, which is what makes a record portable between the two files. */
static int write_lat(char *dst, size_t cap, const LatStats *s) {
    int n = snprintf(dst, cap,
        "{\"avg\":%.4f,\"min\":%.4f,\"med\":%.4f,\"p25\":%.4f,\"p75\":%.4f,"
        "\"p95\":%.4f,\"p99\":%.4f,\"max\":%.4f,\"jitter\":%.4f,"
        "\"loss\":%.4f,\"samples\":%d}",
        finite_or_zero(s->avg), finite_or_zero(s->min), finite_or_zero(s->med),
        finite_or_zero(s->p25), finite_or_zero(s->p75), finite_or_zero(s->p95),
        finite_or_zero(s->p99), finite_or_zero(s->max),
        finite_or_zero(s->jitter), finite_or_zero(s->loss), s->samples);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int write_run(FILE *f, const RunResult *r) {
    char colo[ESC_CAP(sizeof r->meta.colo)];
    char ip[ESC_CAP(sizeof r->meta.ip)];
    char asn[ESC_CAP(sizeof r->meta.asn)];
    char org[ESC_CAP(sizeof r->meta.asn_org)];
    char loc[ESC_CAP(sizeof r->meta.loc)];
    char tls[ESC_CAP(sizeof r->meta.tls)];
    char cipher[ESC_CAP(sizeof r->meta.tls_cipher)];
    char http[ESC_CAP(sizeof r->meta.http)];
    char warp[ESC_CAP(sizeof r->meta.warp)];
    char edges[EDGE_SLOTS * (ESC_CAP(EDGE_WIDTH) + 2)];
    char stab[ESC_CAP(sizeof r->stab_grade)];
    char bloat[ESC_CAP(sizeof r->bloat_grade)];
    char idle[LAT_CAP], loaded[LAT_CAP];
    char line[LINE_CAP];

    esc(r->meta.colo, colo, sizeof colo);
    esc(r->meta.ip, ip, sizeof ip);
    esc(r->meta.asn, asn, sizeof asn);
    esc(r->meta.asn_org, org, sizeof org);
    esc(r->meta.loc, loc, sizeof loc);
    esc(r->meta.tls, tls, sizeof tls);
    esc(r->meta.tls_cipher, cipher, sizeof cipher);
    esc(r->meta.http, http, sizeof http);
    esc(r->meta.warp, warp, sizeof warp);
    esc(r->stab_grade, stab, sizeof stab);
    esc(r->bloat_grade, bloat, sizeof bloat);

    /* The address list is the only part of the record whose length is not
     * fixed, so it is flattened into one escaped fragment and interpolated
     * like every other string. The count is clamped with the loop, so the
     * field and the array can never disagree about how many there are. */
    int nedge = r->meta.edge_ip_count;
    if (nedge < 0) nedge = 0;
    if (nedge > EDGE_SLOTS) nedge = EDGE_SLOTS;

    size_t el = 0;
    edges[0] = '\0';
    for (int i = 0; i < nedge; i++) {
        char one[ESC_CAP(EDGE_WIDTH)];
        esc(r->meta.edge_ips[i], one, sizeof one);
        int w = snprintf(edges + el, sizeof edges - el, "%s\"%s\"",
                         (i > 0) ? "," : "", one);
        /* snprintf reports the length it wanted, so the cursor is clamped
         * before the next write rather than after it. */
        if (w < 0 || (size_t)w >= sizeof edges - el) { el = sizeof edges - 1; break; }
        el += (size_t)w;
    }

    /* The sample arrays are deliberately absent: history keeps the summary
     * numbers, and carrying them would make the file unreadable. The counts
     * do survive, because views gates whole sections on them. */
    if (write_lat(idle, sizeof idle, &r->idle) != 0) return -1;
    if (write_lat(loaded, sizeof loaded, &r->loaded_down) != 0) return -1;

    /* One vocabulary with cli.c, so a line here and a line from --json are the
     * same document and a record copied between them needs no translation. */
    int n = snprintf(line, sizeof line,
        "{\"timestamp_ms\":%llu,\"valid\":%s,"
        "\"meta\":{\"colo\":\"%s\",\"ip\":\"%s\",\"asn\":\"%s\","
        "\"asn_org\":\"%s\",\"loc\":\"%s\",\"tls\":\"%s\","
        "\"tls_cipher\":\"%s\",\"http\":\"%s\",\"warp\":\"%s\","
        "\"dns_ms\":%.4f,\"tls_ms\":%.4f,"
        "\"edge_ip_count\":%d,\"edge_ips\":[%s]},"
        "\"download\":{\"bytes\":%llu,\"nsamples\":%d,\"avg\":%.4f,"
        "\"p95\":%.4f,\"max\":%.4f},"
        "\"upload\":{\"bytes\":%llu,\"nsamples\":%d,\"avg\":%.4f,"
        "\"p95\":%.4f,\"max\":%.4f},"
        "\"latency\":{\"idle\":%s,\"loaded\":%s},"
        "\"bufferbloat_ms\":%.4f,\"stability_cv\":%.4f,"
        "\"grades\":{\"stability\":\"%s\",\"bufferbloat\":\"%s\"}}",
        (unsigned long long)r->timestamp_ms, r->valid ? "true" : "false",
        colo, ip, asn, org, loc, tls, cipher, http, warp,
        finite_or_zero(r->meta.dns_ms), finite_or_zero(r->meta.tls_ms),
        nedge, edges,
        (unsigned long long)r->down.bytes, r->down.nsamples,
        finite_or_zero(r->down.avg), finite_or_zero(r->down.p95),
        finite_or_zero(r->down.max),
        (unsigned long long)r->up.bytes, r->up.nsamples,
        finite_or_zero(r->up.avg), finite_or_zero(r->up.p95),
        finite_or_zero(r->up.max),
        idle, loaded,
        finite_or_zero(r->bufferbloat_ms), finite_or_zero(r->stability_cv),
        stab, bloat);

    /* Unreachable with the field sizes above; dropping one run beats writing a
     * truncated line, which would poison every record after it. */
    if (n < 0 || (size_t)n >= sizeof line) return -1;
    if (fputs(line, f) == EOF) return -1;
    return fputc('\n', f) == EOF ? -1 : 0;
}

int history_save(const History *h) {
    if (!h || h->count < 0) return -1;
    const char *path = history_path();
    if (!path || !*path) return -1;

    char dir[PATH_CAP];
    size_t n = strlen(path);
    if (n >= sizeof dir) return -1;
    memcpy(dir, path, n + 1);
    char *cut = strrchr(dir, CFS_SEP);
    if (!cut) return -1;
    *cut = '\0';
    if (mkdirs(dir) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    fputs("[\n", f);
    /* Oldest first, so dropping the head keeps the newest MAX_RUNS. The
     * caller's array is const and is left alone. */
    int start = (h->count > MAX_RUNS) ? h->count - MAX_RUNS : 0;
    for (int i = start; i < h->count; i++) {
        if (write_run(f, &h->runs[i]) != 0) {
            fclose(f);
            return -1;
        }
    }
    fputs("]\n", f);

    int rc = ferror(f) ? -1 : 0;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

int history_clear(History *h) {
    if (!h) return -1;
    const char *path = history_path();
    int rc = 0;
    if (path && *path && remove(path) != 0 && errno != ENOENT) rc = -1;
    memset(h, 0, sizeof *h);
    return rc;
}

/* --------------------------------------------------------------- scanner */

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_delim(char c) {
    return c == ',' || c == '}' || c == ']';
}

/* Just past the closing quote of the string at p. */
static const char *str_end(const char *p, const char *end) {
    for (p++; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return end;
}

/* Just past the object or array that starts at p. */
static const char *skip_container(const char *p, const char *end) {
    char open = *p, close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (p < end) {
        if (*p == '"') { p = str_end(p, end); continue; }
        if (*p == open) depth++;
        else if (*p == close && --depth == 0) return p + 1;
        p++;
    }
    return end;
}

static const char *skip_value(const char *p, const char *end) {
    if (p >= end) return end;
    if (*p == '"') return str_end(p, end);
    if (*p == '{' || *p == '[') return skip_container(p, end);
    while (p < end && !is_delim(*p)) p++;
    return p;
}

/* The value of a member belonging directly to the object at obj, or NULL.
 * Nested containers are stepped over: an "avg" inside "down" is not the "avg"
 * the caller asked for, and the scan must not wander into it. */
static const char *member(const char *obj, const char *end, const char *key) {
    size_t klen = strlen(key);
    const char *p = obj;
    if (p >= end || *p != '{') return NULL;

    for (p++; p < end;) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;

        if (*p != '"') {
            const char *nx = skip_value(p, end);
            p = nx > p ? nx : p + 1; /* malformed input must not spin here */
            continue;
        }

        const char *after = str_end(p, end);
        const char *q = after;
        while (q < end && is_ws(*q)) q++;

        if ((size_t)(after - p) - 2 == klen && memcmp(p + 1, key, klen) == 0 &&
            q < end && *q == ':') {
            q++;
            while (q < end && is_ws(*q)) q++;
            return (q < end && !is_delim(*q)) ? q : NULL;
        }

        const char *nx = (q < end && *q == ':') ? skip_value(q + 1, end) : after;
        p = nx > p ? nx : p + 1;
    }
    return NULL;
}

/* Bounds of a member whose value is an object. */
static int member_span(const char *obj, const char *end, const char *key,
                       const char **s, const char **e) {
    const char *v = member(obj, end, key);
    if (!v || *v != '{') return 0;
    *s = v;
    *e = skip_container(v, end);
    return *e > *s;
}

/* Decodes the escape at v (pointing at the backslash) into *out and advances v
 * past the last byte consumed. Returns 0 when the escape is malformed. */
static int unescape(const char **v, const char *end, char *out) {
    const char *p = *v + 1;
    if (p >= end) return 0;
    switch (*p) {
        case 'n': *out = '\n'; break;
        case 'r': *out = '\r'; break;
        case 't': *out = '\t'; break;
        case 'b': *out = '\b'; break;
        case 'f': *out = '\f'; break;
        case 'u': {
            if (end - p < 5) return 0;
            unsigned cp = 0;
            for (int k = 1; k <= 4; k++) {
                char h = p[k];
                int d = (h >= '0' && h <= '9')   ? h - '0'
                      : (h >= 'a' && h <= 'f')   ? h - 'a' + 10
                      : (h >= 'A' && h <= 'F')   ? h - 'A' + 10
                                                 : -1;
                if (d < 0) return 0;
                cp = cp * 16u + (unsigned)d;
            }
            /* Only Latin-1 is decoded. Every field we store is an ASCII label,
             * and a surrogate pair would need a re-encoder we do not carry. */
            *out = (cp < 0x80) ? (char)cp : '?';
            p += 4;
            break;
        }
        default: *out = *p; break;
    }
    *v = p;
    return 1;
}

static int read_num(const char *obj, const char *end, const char *key, double *out) {
    const char *v = member(obj, end, key);
    if (!v) return 0;
    /* Copied out rather than handed to strtod directly: the buffer is bounded
     * by end, and the NUL guarantees the parser cannot read past the file. */
    char tmp[NUM_CAP];
    size_t i = 0;
    while (v < end && i + 1 < sizeof tmp &&
           (*v == '-' || *v == '+' || *v == '.' || *v == 'e' || *v == 'E' ||
            (*v >= '0' && *v <= '9')))
        tmp[i++] = *v++;
    tmp[i] = '\0';
    if (i == 0) return 0;

    char *tail = NULL;
    double d = strtod(tmp, &tail);
    if (tail == tmp) return 0;
    *out = d;
    return 1;
}

static int read_str(const char *obj, const char *end, const char *key,
                    char *dst, size_t cap) {
    const char *v = member(obj, end, key);
    if (!v || cap == 0) return 0;
    dst[0] = '\0';
    if (v >= end || *v != '"') return 0;

    size_t o = 0;
    for (v++; v < end && *v != '"'; v++) {
        char c = *v;
        if (c == '\\' && !unescape(&v, end, &c)) break;
        if (o + 1 < cap) dst[o++] = c;
    }
    dst[o] = '\0'; /* a field longer than its destination is cut, not wrapped */
    return 1;
}

static void read_tput(const char *obj, const char *end, Throughput *t) {
    double d;
    if (read_num(obj, end, "bytes", &d)) t->bytes = (uint64_t)(d > 0.0 ? d : 0.0);
    if (read_num(obj, end, "avg",  &d)) t->avg = d;
    if (read_num(obj, end, "p95",  &d)) t->p95 = d;
    if (read_num(obj, end, "max",  &d)) t->max = d;
}

static void read_lat(const char *obj, const char *end, LatStats *l) {
    double d;
    if (read_num(obj, end, "med",    &d)) l->med = d;
    if (read_num(obj, end, "min",    &d)) l->min = d;
    if (read_num(obj, end, "jitter", &d)) l->jitter = d;
    if (read_num(obj, end, "loss",   &d)) l->loss = d;
}

/* Returns 0 for an object holding none of our keys, which is how a version
 * header or a future record type is stepped over instead of aborting the load. */
static int parse_run(const char *obj, const char *end, RunResult *r) {
    double d;
    int found = 0;
    memset(r, 0, sizeof *r);

    const char *s, *e;

    if (read_num(obj, end, "timestamp_ms", &d)) {
        r->timestamp_ms = (uint64_t)(d > 0.0 ? d : 0.0);
        found++;
    }
    if (read_num(obj, end, "dns_ms", &d)) { r->meta.dns_ms = d; found++; }
    if (read_num(obj, end, "tls_ms", &d)) { r->meta.tls_ms = d; found++; }
    found += read_str(obj, end, "colo", r->meta.colo, sizeof r->meta.colo);
    found += read_str(obj, end, "ip", r->meta.ip, sizeof r->meta.ip);
    found += read_str(obj, end, "asn", r->meta.asn, sizeof r->meta.asn);
    found += read_str(obj, end, "asn_org", r->meta.asn_org, sizeof r->meta.asn_org);
    found += read_str(obj, end, "loc", r->meta.loc, sizeof r->meta.loc);
    found += read_str(obj, end, "tls", r->meta.tls, sizeof r->meta.tls);
    found += read_str(obj, end, "warp", r->meta.warp, sizeof r->meta.warp);
    found += read_str(obj, end, "http", r->meta.http, sizeof r->meta.http);
    found += read_str(obj, end, "stab_grade", r->stab_grade, sizeof r->stab_grade);
    found += read_str(obj, end, "bloat_grade", r->bloat_grade, sizeof r->bloat_grade);
    if (read_num(obj, end, "bufferbloat_ms", &d)) { r->bufferbloat_ms = d; found++; }
    if (read_num(obj, end, "stability_cv", &d))   { r->stability_cv = d; found++; }

    if (member_span(obj, end, "down", &s, &e)) { found++; read_tput(s, e, &r->down); }
    if (member_span(obj, end, "up", &s, &e))   { found++; read_tput(s, e, &r->up); }
    if (member_span(obj, end, "idle", &s, &e)) { found++; read_lat(s, e, &r->idle); }
    if (member_span(obj, end, "loaded_down", &s, &e)) {
        found++;
        read_lat(s, e, &r->loaded_down);
    }

    if (found == 0) return 0;
    r->valid = 1; /* anything stored here is a run that reached the end */
    return 1;
}

/* ------------------------------------------------------------------ load */

void history_load(History *h) {
    if (!h) return;
    memset(h, 0, sizeof *h);

    const char *path = history_path();
    if (!path || !*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return; /* nothing recorded yet is the normal first-run case */

    size_t cap = (size_t)FILE_CAP + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) { fclose(f); return; }

    size_t len = 0;
    while (len + 1 < cap) {
        size_t got = fread(buf + len, 1, cap - 1 - len, f);
        if (got == 0) break;
        len += got;
    }
    fclose(f);
    buf[len] = '\0'; /* a short read still leaves a parseable prefix */

    const char *end = buf + len;

    /* Two passes: count the records, then parse only the newest MAX_RUNS.
     * Shifting the array on overflow instead would memcpy megabytes per extra
     * run, and a hand-edited file could carry thousands of them. */
    int total = 0;
    const char *keep = NULL;
    for (const char *q = buf; q < end;) {
        if (*q != '{') { q++; continue; }
        const char *oe = skip_container(q, end);
        if (oe <= q) { q++; continue; }
        if (total >= MAX_RUNS && !keep) keep = q;
        total++;
        q = oe;
    }

    for (const char *p = keep ? keep : buf; p < end && h->count < MAX_RUNS;) {
        if (*p != '{') { p++; continue; }
        const char *oe = skip_container(p, end);
        if (oe <= p) { p++; continue; }
        RunResult r;
        if (parse_run(p, oe, &r)) h->runs[h->count++] = r;
        p = oe;
    }

    free(buf);
}
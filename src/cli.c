/* Argument parsing and the two headless outputs.
 *
 * Everything here is deliberately dependency-free: no JSON library, no getopt.
 * The document is assembled by hand because it is a fixed set of fields from
 * one struct, and the alternative - a vendored serialiser - would be more code
 * than the serialiser it replaces. The guarantee that matters is that the
 * output is always parseable, so truncation has to be impossible rather than
 * merely unlikely: a half-written object is worse than no object, and a
 * consumer reading one field out of a broken document has no way to tell.
 *
 * The JSON is a faithful serialisation of RunResult. Field names match the
 * engine's own so there is one vocabulary to learn, which means units are not
 * carried in the names: throughput is Mbps, latency and bufferbloat are ms,
 * loss and stability_cv are percent, bytes is bytes.
 */
#include "cli.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CFS_VERSION "0.1.0"

#define EXIT_USAGE 2

#define MAX_ITERATIONS 1000
#define MAX_DURATION_S 3600

/* Worst case for the document: every string field escaped at six bytes per
 * character, every number at full precision. Past this the buffer is treated
 * as failed and a minimal valid object is emitted instead. */
#define JSON_CAP 16384

/* Longest string we serialise is the engine's 256-byte error message; a
 * \uXXXX escape is the worst a byte can become, so six per character plus the
 * terminator covers it with room to spare. */
#define SCRATCH_CAP 1600

/* ------------------------------------------------------------- usage text */

static void cli_usage(void) {
    fputs("cfspeed - a speed test against Cloudflare's public endpoints\n"
          "\n"
          "usage: cfspeed [options]\n"
          "\n"
          "  -p, --ping          latency only; skips the transfer phases\n"
          "  -d, --download      download only\n"
          "  -u, --upload        upload only\n"
          "      --json          print one JSON object on stdout and exit\n"
          "  -q, --quiet         suppress the human-readable summary on stderr\n"
          "      --iterations N  run the test N times (default 1)\n"
          "      --duration N    override each transfer phase, in seconds\n"
          "      --out PATH      also write the JSON summary to PATH\n"
          "      --no-history    do not persist this run\n"
          "      --version       print the version and exit\n"
          "  -h, --help          print this text and exit\n"
          "\n"
          "With no options cfspeed runs the full test in the TUI.\n"
          "--json runs headless, so its output stays usable in a pipeline.\n",
          stderr);
}

/* ------------------------------------------------------------ arg parsing */

/* Matches `name` exactly, or `name=value` with the value handed back through
 * *val. Long options only - every short form in this program stands alone. */
static int is_opt(const char *arg, const char *name, const char **val) {
    size_t n = strlen(name);
    if (strncmp(arg, name, n) != 0) return 0;
    if (arg[n] == '\0') {
        if (val) *val = NULL;
        return 1;
    }
    if (arg[n] == '=') {
        if (val) *val = arg + n + 1;
        return 1;
    }
    return 0;
}

/* The value of an option written "--flag value", consuming it. A missing
 * value is a typo, not a request for the built-in default, so it ends the run
 * instead of silently running with something the user did not ask for. */
static const char *next_arg(int argc, char **argv, int *i) {
    if (*i + 1 < argc) {
        (*i)++;
        return argv[*i];
    }
    fprintf(stderr, "cfspeed: %s needs a value\n", argv[*i]);
    exit(EXIT_USAGE);
}

/* Whole-number parse with the trailing-garbage check the standard library does
 * not do, so "--iterations 3x" is an error rather than a quiet 3. */
static long parse_int(const char *s, const char *opt, long lo, long hi) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "cfspeed: %s expects a whole number from %ld to %ld, got '%s'\n",
                opt, lo, hi, s);
        exit(EXIT_USAGE);
    }
    return v;
}

void cli_parse(int argc, char **argv, Options *o) {
    if (!o) return;

    memset(o, 0, sizeof *o);
    o->mode = MODE_FULL;
    o->iterations = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = NULL;

        /* The usage text is printed here rather than returned, because there
         * is no way for the caller to learn a help request was made through
         * this interface. Exiting is what makes `--help` exit. */
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            cli_usage();
            exit(EXIT_SUCCESS);
        }
        if (strcmp(a, "--version") == 0) {
            printf("cfspeed %s\n", CFS_VERSION);
            exit(EXIT_SUCCESS);
        }

        if (strcmp(a, "-p") == 0 || strcmp(a, "--ping") == 0) {
            o->mode = MODE_LATENCY;
            continue;
        }
        if (strcmp(a, "-d") == 0 || strcmp(a, "--download") == 0) {
            o->mode = MODE_DOWNLOAD;
            continue;
        }
        if (strcmp(a, "-u") == 0 || strcmp(a, "--upload") == 0) {
            o->mode = MODE_UPLOAD;
            continue;
        }
        if (strcmp(a, "--json") == 0) {
            o->json = 1;
            continue;
        }
        if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            o->quiet = 1;
            continue;
        }
        if (strcmp(a, "--no-history") == 0) {
            o->no_history = 1;
            continue;
        }
        if (is_opt(a, "--iterations", &val)) {
            if (!val) val = next_arg(argc, argv, &i);
            o->iterations = (int)parse_int(val, "--iterations", 1, MAX_ITERATIONS);
            continue;
        }
        if (is_opt(a, "--duration", &val)) {
            if (!val) val = next_arg(argc, argv, &i);
            o->duration_s = (int)parse_int(val, "--duration", 0, MAX_DURATION_S);
            continue;
        }
        if (is_opt(a, "--out", &val)) {
            if (!val) val = next_arg(argc, argv, &i);
            /* A path cut short would write the result somewhere the user never
             * named, which is worse than refusing to write at all. */
            if (!val[0]) {
                fprintf(stderr, "cfspeed: --out needs a file name\n");
                exit(EXIT_USAGE);
            }
            if (strlen(val) >= sizeof o->out) {
                fprintf(stderr, "cfspeed: output path is longer than %d characters\n",
                        (int)sizeof o->out - 1);
                exit(EXIT_USAGE);
            }
            snprintf(o->out, sizeof o->out, "%s", val);
            continue;
        }

        /* Silently ignoring a misspelled flag is how a user ends up believing
         * they capped the duration when they did not. */
        fprintf(stderr, "cfspeed: unknown option '%s'\n", a);
        fprintf(stderr, "try 'cfspeed --help' for the list of options\n");
        exit(EXIT_USAGE);
    }

    /* One authoritative place for the rule, so --json behaves the same however
     * it was reached: a JSON object cannot come out of a full-screen TUI. */
    if (o->json) o->headless = 1;
}

int cli_validate(const Options *o) {
    if (!o) return EXIT_USAGE;

    if (o->mode < MODE_FULL || o->mode > MODE_UPLOAD) {
        fprintf(stderr, "cfspeed: invalid test mode\n");
        return EXIT_USAGE;
    }
    if (o->iterations < 1) {
        fprintf(stderr, "cfspeed: --iterations must be at least 1\n");
        return EXIT_USAGE;
    }
    if (o->duration_s < 0) {
        fprintf(stderr, "cfspeed: --duration cannot be negative\n");
        return EXIT_USAGE;
    }
    /* Accepted and ignored is worse than rejected: a ping run has no transfer
     * phase, so there is nothing for the number to change. */
    if (o->duration_s > 0 && o->mode == MODE_LATENCY) {
        fprintf(stderr, "cfspeed: --duration has no transfer phase to change in ping mode\n");
        return EXIT_USAGE;
    }
    return 0;
}

/* ------------------------------------------------------------------- json */

typedef struct {
    char  *p;      /* document being built */
    size_t cap;
    size_t len;
    int    cut;    /* set once the document can no longer be completed */
    char   num[48];
    char   str[SCRATCH_CAP];
} Buf;

static void bput(Buf *b, const char *fmt, ...) {
    if (b->cut) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        /* Leave the fragment in place but stop: the caller discards anything
         * flagged this way, so a partial document is never printed. */
        b->cut = 1;
        return;
    }
    b->len += (size_t)n;
}

/* A JSON number, written as a bare token. Non-finite values have no JSON
 * spelling and a degenerate window can produce them, so they become null and
 * the rest of the document stays readable. Zero is emitted bare and never as
 * "-0": a signed zero is a difference that landed exactly on nothing, and it
 * reads like a defect in the measurement. */
static void jnum(Buf *b, double v) {
    if (!isfinite(v)) {
        bput(b, "null");
        return;
    }
    /* Sized first, so a value that cannot be written as a whole number
     * degrades to null rather than to a truncated fragment like "1.234e+2". */
    char probe[512];
    int need = snprintf(probe, sizeof probe, "%.4f", v);
    if (need < 0 || (size_t)need >= sizeof probe || (size_t)need >= sizeof b->num) {
        bput(b, "null");
        return;
    }
    snprintf(b->num, sizeof b->num, "%.4f", v);
    size_t n = strlen(b->num);
    /* %.4f always writes a '.', so trimming cannot eat the integer part. */
    while (n > 0 && b->num[n - 1] == '0') n--;
    if (n > 0 && b->num[n - 1] == '.') n--;
    if (n == 2 && b->num[0] == '-' && b->num[1] == '0') {
        bput(b, "0");
        return;
    }
    b->num[n] = '\0';
    bput(b, "%s", b->num);
}

/* A JSON string, quoted. Escapes the two characters that would end the token
 * and every control byte, since raw ones are illegal inside a JSON string.
 * Bytes above 0x7f pass through: the document is UTF-8 and that is legal.
 * Truncation happens only between whole characters, so a short string is still
 * a valid one. */
static void jstr(Buf *b, const char *s) {
    static const char hex[] = "0123456789abcdef";
    if (!s) s = "";
    bput(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (b->len + 3 > b->cap) break;
            b->p[b->len++] = '\\';
            b->p[b->len++] = (char)c;
        } else if (c < 0x20) {
            const char *esc = (c == '\n') ? "\\n" : (c == '\r') ? "\\r"
                           : (c == '\t') ? "\\t" : (c == '\b') ? "\\b"
                           : (c == '\f') ? "\\f" : NULL;
            if (esc) {
                if (b->len + 3 > b->cap) break;
                b->p[b->len++] = esc[0];
                b->p[b->len++] = esc[1];
            } else {
                if (b->len + 7 > b->cap) break;
                b->p[b->len++] = '\\';
                b->p[b->len++] = 'u';
                b->p[b->len++] = '0';
                b->p[b->len++] = '0';
                b->p[b->len++] = hex[(c >> 4) & 0x0F];
                b->p[b->len++] = hex[c & 0x0F];
            }
        } else {
            if (b->len + 2 > b->cap) break;
            b->p[b->len++] = (char)c;
        }
    }
    bput(b, "\"");
}

static const char *mode_name(Mode m) {
    switch (m) {
        case MODE_FULL:     return "full";
        case MODE_LATENCY:  return "latency";
        case MODE_DOWNLOAD: return "download";
        case MODE_UPLOAD:   return "upload";
        default:            return "unknown";
    }
}

/* UTC, so a report does not change shape with the reader's timezone. */
static void iso8601(uint64_t ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000u);
    struct tm tmv;
    int ok;
#if defined(_WIN32)
    ok = (gmtime_s(&tmv, &t) == 0);
#else
    ok = (gmtime_r(&t, &tmv) != NULL);
#endif
    if (!ok || strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0)
        snprintf(out, cap, "1970-01-01T00:00:00Z");
}

/* The per-phase sample arrays are not serialised. They exist to draw charts,
 * and a thousand doubles a phase would make the report mostly data the caller
 * cannot act on. The count survives in nsamples. */
static void emit_throughput(Buf *b, const char *name, const Throughput *t) {
    bput(b, "\"%s\":{\"avg\":", name);
    jnum(b, t->avg);
    bput(b, ",\"p95\":");
    jnum(b, t->p95);
    bput(b, ",\"max\":");
    jnum(b, t->max);
    bput(b, ",\"bytes\":%llu,\"nsamples\":%d}",
         (unsigned long long)t->bytes, t->nsamples);
}

/* Both windows share one writer so the idle and loaded objects cannot drift
 * apart - a consumer should be able to compare them field by field. */
static void emit_lat(Buf *b, const char *name, const LatStats *s) {
    bput(b, "\"%s\":{\"avg\":", name);
    jnum(b, s->avg);
    bput(b, ",\"min\":");
    jnum(b, s->min);
    bput(b, ",\"med\":");
    jnum(b, s->med);
    bput(b, ",\"p25\":");
    jnum(b, s->p25);
    bput(b, ",\"p75\":");
    jnum(b, s->p75);
    bput(b, ",\"p95\":");
    jnum(b, s->p95);
    bput(b, ",\"p99\":");
    jnum(b, s->p99);
    bput(b, ",\"max\":");
    jnum(b, s->max);
    bput(b, ",\"jitter\":");
    jnum(b, s->jitter);
    bput(b, ",\"loss\":");
    jnum(b, s->loss);
    bput(b, ",\"samples\":%d}", s->samples);
}

static void emit_meta(Buf *b, const Meta *m) {
    bput(b, "\"meta\":{\"colo\":");
    jstr(b, m->colo);
    bput(b, ",\"ip\":");
    jstr(b, m->ip);
    bput(b, ",\"asn\":");
    jstr(b, m->asn);
    bput(b, ",\"asn_org\":");
    jstr(b, m->asn_org);
    bput(b, ",\"loc\":");
    jstr(b, m->loc);
    bput(b, ",\"tls\":");
    jstr(b, m->tls);
    bput(b, ",\"tls_cipher\":");
    jstr(b, m->tls_cipher);
    bput(b, ",\"http\":");
    jstr(b, m->http);
    bput(b, ",\"warp\":");
    jstr(b, m->warp);
    bput(b, ",\"dns_ms\":");
    jnum(b, m->dns_ms);
    bput(b, ",\"tls_ms\":");
    jnum(b, m->tls_ms);

    /* The count is clamped with the loop so the field and the array can never
     * disagree about how many addresses there are. */
    int n = m->edge_ip_count;
    if (n < 0) n = 0;
    if (n > (int)(sizeof m->edge_ips / sizeof m->edge_ips[0])) n = (int)(sizeof m->edge_ips / sizeof m->edge_ips[0]);
    bput(b, ",\"edge_ip_count\":%d,\"edge_ips\":[", n);
    for (int i = 0; i < n; i++) {
        if (i > 0) bput(b, ",");
        jstr(b, m->edge_ips[i]);
    }
    bput(b, "]}");
}

void cli_json(const Engine *e) {
    if (!e) {
        fputs("{\"error\":\"no engine\"}", stdout);
        return;
    }
    const RunResult *r = engine_result(e);
    const Meta *m = engine_meta(e);
    if (!r || !m) {
        fputs("{\"error\":\"no result\"}", stdout);
        return;
    }

    char *doc = (char *)malloc(JSON_CAP);
    if (!doc) {
        fputs("{\"error\":\"out of memory\"}", stdout);
        return;
    }

    Buf b;
    memset(&b, 0, sizeof b);
    b.p = doc;
    b.cap = JSON_CAP;

    char iso[32];
    iso8601(r->timestamp_ms, iso, sizeof iso);

    bput(&b, "{\"timestamp\":\"%s\",\"timestamp_ms\":%llu", iso,
         (unsigned long long)r->timestamp_ms);
    bput(&b, ",\"mode\":\"%s\",\"phase\":\"%s\",\"valid\":%s",
         mode_name(engine_mode(e)), phase_label(engine_phase(e)),
         r->valid ? "true" : "false");
    bput(&b, ",\"error\":");
    jstr(&b, engine_error(e));
    bput(&b, ",");
    emit_meta(&b, &r->meta);
    bput(&b, ",");
    emit_throughput(&b, "download", &r->down);
    bput(&b, ",");
    emit_throughput(&b, "upload", &r->up);
    bput(&b, ",\"latency\":{");
    emit_lat(&b, "idle", &r->idle);
    bput(&b, ",");
    emit_lat(&b, "loaded", &r->loaded_down);
    bput(&b, "},\"bufferbloat_ms\":");
    jnum(&b, r->bufferbloat_ms);
    bput(&b, ",\"stability_cv\":");
    jnum(&b, r->stability_cv);
    bput(&b, ",\"grades\":{\"stability\":");
    jstr(&b, r->stab_grade);
    bput(&b, ",\"bufferbloat\":");
    jstr(&b, r->bloat_grade);
    bput(&b, "}}");

    /* One write, so a consumer reading a pipe sees the object whole. */
    if (b.cut)
        fputs("{\"error\":\"document exceeded the output buffer\"}", stdout);
    else
        fwrite(doc, 1, b.len, stdout);
    free(doc);
}

/* ---------------------------------------------------------------- summary */

static void fmt_rate(double mbps, char *out, size_t cap) {
    if (mbps >= 1000.0)     snprintf(out, cap, "%.2f Gbps", mbps / 1000.0);
    else if (mbps >= 100.0) snprintf(out, cap, "%.0f Mbps", mbps);
    else if (mbps >= 10.0)  snprintf(out, cap, "%.1f Mbps", mbps);
    else                    snprintf(out, cap, "%.2f Mbps", mbps);
}

static void fmt_ms(double ms, char *out, size_t cap) {
    if (ms >= 100.0) snprintf(out, cap, "%.0fms", ms);
    else if (ms >= 10.0) snprintf(out, cap, "%.1fms", ms);
    else snprintf(out, cap, "%.2fms", ms);
}

static void fmt_bytes(uint64_t b, char *out, size_t cap) {
    if (b >= 1073741824ull)
        snprintf(out, cap, "%.2f GiB", (double)b / 1073741824.0);
    else if (b >= 1048576ull)
        snprintf(out, cap, "%.2f MiB", (double)b / 1048576.0);
    else if (b >= 1024ull)
        snprintf(out, cap, "%.2f KiB", (double)b / 1024.0);
    else
        snprintf(out, cap, "%llu B", (unsigned long long)b);
}

/* One transfer line. Both phases share the layout so their columns line up and
 * a reader comparing them does not have to re-find anything. */
static void line_rate(const char *label, const Throughput *t, char *line, size_t cap) {
    char avg[32], p95[32], mx[32], bytes[32];
    fmt_rate(t->avg, avg, sizeof avg);
    fmt_rate(t->p95, p95, sizeof p95);
    fmt_rate(t->max, mx, sizeof mx);
    fmt_bytes(t->bytes, bytes, sizeof bytes);
    snprintf(line, cap, "  %-6s %12s  p95 %10s  max %10s  %s\n",
             label, avg, p95, mx, bytes);
}

/* The median is the headline figure because the mean of latency samples is
 * dragged around by the occasional stalled probe. */
static void line_lat(const char *label, const LatStats *s, char *line, size_t cap) {
    char med[32], jit[32], loss[32];
    fmt_ms(s->med, med, sizeof med);
    fmt_ms(s->jitter, jit, sizeof jit);
    snprintf(loss, sizeof loss, "%.1f%%", s->loss);
    snprintf(line, cap, "  %-6s  med %10s  jitter %10s  loss %7s  n=%d\n",
             label, med, jit, loss, s->samples);
}

/* Stderr, not stdout: the summary is a human aside, and --json consumers pipe
 * stdout somewhere that must not carry prose. */
void cli_summary(const Engine *e) {
    if (!e) {
        fputs("cfspeed: nothing to report\n", stderr);
        return;
    }
    const RunResult *r = engine_result(e);
    const Meta *m = engine_meta(e);
    const char *err = engine_error(e);
    char line[768];

    /* A refused run has numbers that are all zero. Printing them as a result
     * would be a lie about a link that never answered. */
    if (!r || !m || !r->valid) {
        snprintf(line, sizeof line, "cfspeed: %s: %s\n", phase_label(engine_phase(e)),
                 (err && err[0]) ? err : "the run produced no result");
        fputs(line, stderr);
        return;
    }

    snprintf(line, sizeof line, "cfspeed  %s \xc2\xb7 %s\n",
             m->colo[0] ? m->colo : "?", m->loc[0] ? m->loc : "?");
    fputs(line, stderr);

    snprintf(line, sizeof line, "  you    %s \xc2\xb7 %s%s%s\n", m->ip[0] ? m->ip : "?",
             m->asn_org[0] ? m->asn_org : "?",
             m->asn[0] ? " (AS" : "", m->asn[0] ? m->asn : "");
    fputs(line, stderr);

    /* Every address the edge answered with, in the order the server gave them,
     * so a log line and a screenshot describe the same route. */
    char edges[640];
    edges[0] = '\0';
    size_t el = 0;
    for (int i = 0; i < m->edge_ip_count &&
                     i < (int)(sizeof m->edge_ips / sizeof m->edge_ips[0]); i++) {
        int n = snprintf(edges + el, sizeof edges - el, "%s%s", (i > 0) ? " " : "",
                         m->edge_ips[i][0] ? m->edge_ips[i] : "?");
        if (n < 0) break;
        /* snprintf reports the length it wanted, so the cursor has to be
         * clamped before the next write or the buffer end is passed. */
        if ((size_t)n >= sizeof edges - el) { el = sizeof edges - 1; break; }
        el += (size_t)n;
    }
    if (edges[0]) {
        snprintf(line, sizeof line, "  edge   %s\n", edges);
        fputs(line, stderr);
    }

    line_rate("down", &r->down, line, sizeof line);
    fputs(line, stderr);
    line_rate("up", &r->up, line, sizeof line);
    fputs(line, stderr);
    line_lat("idle", &r->idle, line, sizeof line);
    fputs(line, stderr);
    line_lat("loaded", &r->loaded_down, line, sizeof line);
    fputs(line, stderr);

    char cv[32], bloat[32];
    snprintf(cv, sizeof cv, "%.1f%%", r->stability_cv);
    fmt_ms(r->bufferbloat_ms, bloat, sizeof bloat);
    snprintf(line, sizeof line, "  grades  bloat %s (%s)   stability %s (CV %s)\n",
             r->bloat_grade[0] ? r->bloat_grade : "-", bloat,
             r->stab_grade[0] ? r->stab_grade : "-", cv);
    fputs(line, stderr);

    char dns[32], tls[32];
    fmt_ms(m->dns_ms, dns, sizeof dns);
    fmt_ms(m->tls_ms, tls, sizeof tls);
    snprintf(line, sizeof line, "  conn    %s \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 warp %s"
                               " \xc2\xb7 dns %s \xc2\xb7 tls %s\n",
             m->tls[0] ? m->tls : "-",
             m->tls_cipher[0] ? m->tls_cipher : "-",
             m->http[0] ? m->http : "-",
             m->warp[0] ? m->warp : "-", dns, tls);
    fputs(line, stderr);
    fflush(stderr);
}
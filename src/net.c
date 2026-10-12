#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
#else
#  include <pthread.h>
#  include <time.h>
#  include <sys/time.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------ atomic accessors */

/* The byte counter is a plain `uint64_t` owned by engine.c and shared by up to
 * six worker threads (write) and the UI thread (read). It was being updated
 * with a bare `+=`, so concurrent workers lost updates and every reported
 * figure - Mbps, avg, p95, max, total bytes - was silently undercounted by a
 * different amount on every run. That is exactly the case where a number
 * disagrees with an external speed test.
 *
 * These use the GCC/clang __atomic builtins rather than <stdatomic.h> _Atomic
 * objects or the Win32 Interlocked* intrinsics, for two reasons:
 *
 *   - The builtins give atomicity on a pointer to an ordinary naturally
 *     aligned uint64_t. That means engine.h does NOT have to redeclare
 *     Engine.counter as _Atomic: the type stays `uint64_t` and this file stays
 *     independent of engine.h's layout, which was the whole point of the
 *     constraint. _Atomic objects would in fact still work through a
 *     uint64_t*, but they would force the declaration change.
 *   - InterlockedExchangeAdd64 needs windows.h included with a sufficiently
 *     new _WIN32_WINNT, and the Makefile's Windows build defines neither, so
 *     the intrinsics would need a header-order workaround. The builtins need
 *     nothing beyond -std=gnu11.
 *
 * Relaxed ordering is sufficient and is the cheapest correct choice here: the
 * counter is a self-contained running total. No other datum is published
 * through it, so there is nothing for these accesses to be ordered against.
 * A mutex would also make the counter correct, but this runs in curl's write
 * callback - tens of thousands of times a second across six threads - so
 * serialising there would cost more than the measurement itself.
 *
 * Alignment: the target is x86-64, where a uint64_t object is always
 * 8-byte aligned, which is what these builtins require. */
static inline uint64_t atomic_load_u64(const uint64_t *p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void atomic_add_u64(uint64_t *p, uint64_t v) {
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static inline int atomic_load_i(const int *p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void atomic_store_i(int *p, int v) {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

/* --------------------------------------------------------------- helpers */

static uint64_t mono_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Set CFS_DEBUG=1 to trace requests to stderr. Off by default: this is a TUI
 * and a wall of curl messages would wreck it. */
static int dbg(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("CFS_DEBUG");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} Sink;

static size_t sink_cb(char *p, size_t sz, size_t n, void *ud) {
    Sink *s = (Sink *)ud;
    size_t total = sz * n;
    if (s->len + total + 1 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        while (cap < s->len + total + 1) cap *= 2;
        char *nb = (char *)realloc(s->buf, cap);
        if (!nb) return 0;
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, p, total);
    s->len += total;
    s->buf[s->len] = '\0';
    return total;
}

/* Copies only, never counts. Download is counted by the progress callback so
 * that in-flight bytes are attributed to the right moment in time. */
static size_t sink_discard(char *p, size_t sz, size_t n, void *ud) {
    Sink *s = (Sink *)ud;
    size_t total = sz * n;
    if (s->len + total + 1 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 65536;
        while (cap < s->len + total + 1) cap *= 2;
        char *nb = (char *)realloc(s->buf, cap);
        if (!nb) return 0;
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, p, total);
    s->len += total;
    s->buf[s->len] = '\0';
    return total;
}

static int file_exists(const char *path) {
#if defined(_WIN32)
    return _access(path, 0) == 0;
#else
    return access(path, R_OK) == 0;
#endif
}

/* Locate a CA bundle.
 *
 * Linux and macOS find the system store on their own. A Windows curl built
 * against GnuTLS or OpenSSL has none, and without one every request fails with
 * CURLE_PEER_FAILED_VERIFICATION. CURLSSLOPT_NATIVE_CA does not help there: it
 * is honoured only by schannel builds. So a bundle is shipped beside the binary
 * and pointed at explicitly.
 *
 * Disabling verification instead would be a security regression on a tool whose
 * entire job is trusting a remote endpoint. */
static void use_ca_bundle(CURL *c) {
#if defined(_WIN32)
    /* Declared inside the branch that uses it: the POSIX path below never
     * builds a path, so hoisting this out left an unused-variable warning
     * under -Wall on every Linux/macOS build. */
    char path[512];
    /* Next to the executable, which is where the release puts it. */
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (n > 0 && n < sizeof exe) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *slash = '\0';
            size_t l = strlen(exe);
            snprintf(path, sizeof path, "%s%scfspeed-ca.pem", exe,
                     l && exe[l - 1] == '\\' ? "" : "\\");
            if (file_exists(path)) {
                curl_easy_setopt(c, CURLOPT_CAINFO, path);
                if (dbg()) fprintf(stderr, "[ca] using %s\n", path);
                return;
            }
        }
    }
#else
    static const char *candidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (file_exists(candidates[i])) {
            curl_easy_setopt(c, CURLOPT_CAINFO, candidates[i]);
            if (dbg()) fprintf(stderr, "[ca] using %s\n", candidates[i]);
            return;
        }
    }
#endif
    if (dbg()) fprintf(stderr, "[ca] no bundle found, relying on the build default\n");
}

static void curl_base(CURL *c, const char *url) {
    use_ca_bundle(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "cfspeed/1.0");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
#if defined(_WIN32)
    /* CURLSSLOPT_NATIVE_CA is an enum member, not a macro, so it cannot be
     * tested with #if defined() - doing so silently compiles the branch out and
     * every request then fails with CURLE_PEER_FAILED_VERIFICATION, because
     * Windows curl has no CA bundle of its own. Linux and macOS locate the
     * system store without help.
     *
     * Turning verification off instead would be a security regression on a tool
     * whose entire job is trusting a remote endpoint. */
    {
        curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
        if (vi && vi->version_num >= 0x074700) /* 7.71.0 introduced it */
            curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    }
#endif
}

/* ------------------------------------------------------------- lifecycle */

int net_init(void) {
#if defined(_WIN32)
    /* Winsock needs explicit startup; the address lookups below depend on it. */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NET_ERR;
#endif
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? NET_OK : NET_ERR;
}

void net_shutdown(void) {
    curl_global_cleanup();
#if defined(_WIN32)
    WSACleanup();
#endif
}

/* ------------------------------------------------------------------ meta */

/* Minimal extractor for a flat JSON string field. Good enough for /meta and it
 * never allocates on attacker-controlled length. Returns 1 on hit. */
static int json_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

/* Pull the host out of a base URL so we can time its resolution. */
static void url_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && i + 1 < cap) out[i++] = *p++;
    out[i] = '\0';
}

/* Write `src` into a fixed-size Meta field, truncating at `cap - 1` bytes.
 *
 * Every Meta field below is a fixed C array fed by server-controlled JSON, so a
 * hostile or merely verbose response can hand us a string far longer than the
 * field. `snprintf(dst, sizeof dst, "%s", src)` does truncate, correctly, but
 * it says nothing to the compiler about how much it can write, so
 * -Wformat-truncation flagged all ten of these call sites and any build that
 * adds -Werror would fail.
 *
 * The bound is made explicit here instead, by writing the terminator at an
 * index the compiler can check against the buffer: `n` is clamped to cap - 1,
 * so the loop-free copy below touches at most indices 0..cap-2 and then
 * dst[cap-1]. Both are inside a cap-byte object, for any cap, with no
 * dependence on the optimisation level.
 *
 * A "%.*s" precision was the obvious alternative and passes at -O2, but it
 * only reads as bounded because GCC relates the precision argument to the
 * buffer size argument. Once this is inlined at -Os or -O0 that relationship is
 * lost and the same ten sites warn again - with GCC even pairing one call's
 * precision with another call's destination size. Making the bound structural
 * rather than format-driven is what makes it hold in every build.
 *
 * Truncation is preserved, not turned into rejection: an over-long value is
 * still stored, just clipped, which is what the UI wants to display. */
static void copy_field(char *dst, size_t cap, const char *src) {
    if (!dst || !src || cap == 0) return;
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* /cdn-cgi/trace answers a flat "key=value" listing, one pair per line.
 *
 * Matching is anchored to the start of a line. A plain strstr for "ip=" would
 * also match the tail of some other token, and a match that is not at a line
 * start is far more likely to be the wrong field than the right one. */
static int trace_field(const char *body, const char *key, char *out, size_t cap) {
    if (cap == 0) return 0;
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > klen && p[klen] == '=' && memcmp(p, key, klen) == 0) {
            size_t vlen = len - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

void net_fetch_meta(const char *base_url, Meta *m) {
    memset(m, 0, sizeof *m);

    CURL *c = curl_easy_init();
    if (!c) return;
    curl_base(c, base_url);
    if (dbg()) fprintf(stderr, "[meta] base=%s\n", base_url);

    /* Time DNS separately: resolution is a real part of connection setup and
     * conflating it with TLS hides which one is slow. */
    char host[256];
    url_host(base_url, host, sizeof host);
    {
        char *port = NULL;
        char hostport[300];
        snprintf(hostport, sizeof hostport, "%s", host);
        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        uint64_t t0 = mono_ms();
        struct addrinfo *res = NULL;
        int rc = getaddrinfo(hostport, NULL, &hints, &res);
        m->dns_ms = (double)(mono_ms() - t0);
        if (rc == 0 && res) {
            int n = 0;
            for (struct addrinfo *ai = res; ai && n < 8; ai = ai->ai_next) {
                char ip[INET6_ADDRSTRLEN + 1] = {0};
                if (ai->ai_family == AF_INET) {
                    struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
                    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
                    snprintf(m->edge_ips[n], sizeof m->edge_ips[0], "%s:443", ip);
                    n++;
                } else if (ai->ai_family == AF_INET6) {
                    struct sockaddr_in6 *sa = (struct sockaddr_in6 *)ai->ai_addr;
                    inet_ntop(AF_INET6, &sa->sin6_addr, ip, sizeof ip);
                    snprintf(m->edge_ips[n], sizeof m->edge_ips[0], "[%s]:443", ip);
                    n++;
                }
            }
            m->edge_ip_count = n;
            /* Release the resolved addresses, NOT Winsock. net_init() calls
             * WSAStartup() exactly once and net_shutdown() owns the single
             * matching WSACleanup(); calling WSACleanup() here dropped the
             * process-wide Winsock refcount to zero ~100 ms before the first
             * probe, so every later getaddrinfo() failed with WSANOTINITIALISED
             * (10093) and net_shutdown() then cleaned up a startup that no
             * longer existed. libcurl does not call WSAStartup() itself, so
             * nothing re-balanced it. freeaddrinfo() is declared and works on
             * mingw-w64 (ws2tcpip.h:233) - it is not platform-specific - and the
             * addrinfo list was additionally being leaked on Windows. */
            freeaddrinfo(res);
        }
        (void)port;
    }

    /* A small request whose timing splits connection setup from transfer. */
    {
        Sink sink = {0};
        char url[512];
        snprintf(url, sizeof url, "%s/__down?bytes=0", base_url);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_discard);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        uint64_t t0 = mono_ms();
        CURLcode rc = curl_easy_perform(c);
        double pre = 0.0;
        curl_easy_getinfo(c, CURLINFO_PRETRANSFER_TIME, &pre);
        if (rc == CURLE_OK) {
            m->tls_ms = pre * 1000.0;
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            if (code >= 200 && code < 300) snprintf(m->http, sizeof m->http, "HTTP/%ld.%ld", code / 100, code % 100);
        }
        (void)t0;
        free(sink.buf);
    }

    /* The colo, the client IP and the location come from /cdn-cgi/trace.
     * /meta answers an empty object {} for most clients now, so parsing it left
     * every one of these fields blank - the app ran and measured correctly but
     * could not say where it was or which edge it reached. */
    {
        Sink sink = {0};
        char url[512];
        snprintf(url, sizeof url, "%s/cdn-cgi/trace", base_url);
        curl_easy_reset(c);
        curl_base(c, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        if (curl_easy_perform(c) == CURLE_OK && sink.buf) {
            char v[256];
            if (trace_field(sink.buf, "ip", v, sizeof v))
                copy_field(m->ip, sizeof m->ip, v);
            if (trace_field(sink.buf, "colo", v, sizeof v))
                copy_field(m->colo, sizeof m->colo, v);
            if (trace_field(sink.buf, "loc", v, sizeof v))
                copy_field(m->loc, sizeof m->loc, v);
            if (trace_field(sink.buf, "warp", v, sizeof v))
                copy_field(m->warp, sizeof m->warp, v);
            if (trace_field(sink.buf, "tls", v, sizeof v))
                copy_field(m->tls, sizeof m->tls, v);
            if (trace_field(sink.buf, "http", v, sizeof v)) {
                /* trace reports "http/1.1"; the UI shows it as a protocol. */
                for (size_t i = 0; v[i] && i < 4; i++)
                    if (v[i] >= 'a' && v[i] <= 'z') v[i] = (char)(v[i] - 32);
                copy_field(m->http, sizeof m->http, v);
            }
        }
        free(sink.buf);
    }

    /* /meta is kept only for the ASN and cipher fields trace does not expose.
     * It answers {} for many clients, in which case these simply stay empty and
     * the views omit the rows rather than printing a zero. */
    {
        Sink sink = {0};
        char url[512];
        snprintf(url, sizeof url, "%s/meta", base_url);
        curl_easy_reset(c);
        curl_base(c, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        if (curl_easy_perform(c) == CURLE_OK && sink.buf) {
            char tmp[256];
            const char *p = strstr(sink.buf, "\"asnOrg\"");
            if (p) {
                const char *o = strchr(p, '{');
                if (o && json_str(o, "name", tmp, sizeof tmp))
                    copy_field(m->asn_org, sizeof m->asn_org, tmp);
            }
            const char *a = strstr(sink.buf, "\"asn\"");
            if (a && json_str(a, "asn", tmp, sizeof tmp))
                copy_field(m->asn, sizeof m->asn, tmp);
            const char *t = strstr(sink.buf, "\"tls\"");
            if (t) {
                const char *o = strchr(t, '{');
                if (o) {
                    if (json_str(o, "tlsVersion", tmp, sizeof tmp) && !m->tls[0])
                        copy_field(m->tls, sizeof m->tls, tmp);
                    if (json_str(o, "cipher", tmp, sizeof tmp))
                        copy_field(m->tls_cipher, sizeof m->tls_cipher, tmp);
                }
            }
        }
        free(sink.buf);
    }

    curl_easy_cleanup(c);
}

/* --------------------------------------------------------------- latency */

int net_probe(const char *base_url, double *ms) {
    CURL *c = curl_easy_init();
    if (!c) return NET_ERR;
    if (dbg()) fprintf(stderr, "[probe] start\n");
    Sink sink = {0};
    char url[512];
    snprintf(url, sizeof url, "%s/__down?bytes=0", base_url);
    curl_base(c, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_discard);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);

    uint64_t t0 = mono_ms();
    CURLcode rc = curl_easy_perform(c);
    double elapsed = (double)(mono_ms() - t0);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    free(sink.buf);
    curl_easy_cleanup(c);

    if (dbg()) fprintf(stderr, "[probe] rc=%d http=%ld ms=%.1f\n", (int)rc, code, elapsed);
    if (rc != CURLE_OK) return NET_ERR;
    if (code < 200 || code >= 300) return NET_REFUSED;
    *ms = elapsed;
    return NET_OK;
}

/* -------------------------------------------------------------- workers */

struct WorkerHandle {
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif
    /* Cross-thread visibility comes from the __atomic builtins on every access,
     * not from `volatile`. volatile is not a synchronisation primitive: it
     * constrains the compiler but orders nothing and is not atomic. That
     * matters for more than tidiness here - it is what lets net_worker_stop()'s
     * join observe the flag and return, which in turn is what guarantees the
     * context is dead before it is freed. */
    int stop;
};

/* A fixed 1 MiB upload body. Earlier this streamed an endless body and counted
 * as reqwest pulled chunks from it, which measures bytes handed to the kernel
 * rather than bytes delivered - it reported over 1 Gbps on a link that sustains
 * ~130 Mbps. Counting only on a 2xx response is ground truth.
 *
 * Per worker, not static. It used to be a function-local `static`, so all six
 * workers re-initialised the same 1 MiB buffer concurrently while other
 * workers were POSTing from it - a data race that corrupted the payload
 * mid-flight and left workers posting bytes belonging to a different request.
 * A mutex would only serialise the workers; since the body is filled once and
 * then posted many times, per-worker storage removes the shared state outright
 * instead of guarding it. */
#define NET_BODY_BYTES (1024u * 1024u)

typedef struct {
    struct WorkerHandle h;
    char base[256];
    uint64_t duration_ms;
    Worker cfg;
    unsigned char *body; /* this worker's private upload payload */
} WorkerCtx;

static size_t progress_cb(char *p, size_t sz, size_t n, void *ud) {
    (void)p;
    Worker *w = (Worker *)ud;
    /* Download progress is real: these bytes are off the socket and on the
     * wire, so counting here is legitimate and time-accurate. The add is
     * atomic because several workers share this one counter. */
    if (!w->upload && w->counter)
        atomic_add_u64(w->counter, (uint64_t)(sz * n));
    return sz * n;
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID arg);
#else
static void *worker_main(void *arg);
#endif

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID arg) {
#else
static void *worker_main(void *arg) {
#endif
    WorkerCtx *ctx = (WorkerCtx *)arg;
    Worker *w = &ctx->cfg;
    const char *base = ctx->base;
    uint64_t deadline = mono_ms() + ctx->duration_ms;

    /* The upload body is allocated and filled by net_worker_start() before this
     * thread exists, so it is private to this worker with nothing to race. */
    unsigned char *const body = ctx->body;

    CURL *c = curl_easy_init();
    if (!c) {
#if defined(_WIN32)
        return 0;
#else
        return NULL;
#endif
    }
    curl_base(c, base);

    int ok_streak = 0;

    while (mono_ms() < deadline && !atomic_load_i(&ctx->h.stop)) {
        char url[512];
        struct curl_slist *hdrs = NULL;
        int ok = 0;

        if (w->upload) {
            snprintf(url, sizeof url, "%s/__up", base);
            /* "Expect: 100-continue" would add a round trip per body; Cloudflare
             * does not need it and it would show up in the timing. */
            hdrs = curl_slist_append(hdrs, "Expect:");
            curl_easy_setopt(c, CURLOPT_URL, url);
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)NET_BODY_BYTES);
        } else {
            snprintf(url, sizeof url, "%s/__down?bytes=%lu", base, w->chunk);
            curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(c, CURLOPT_URL, url);
        }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, progress_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, w);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

        CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (hdrs) curl_slist_free_all(hdrs);
        if (dbg())
            fprintf(stderr, "[worker %s] url=%s rc=%d(%s) http=%ld count=%llu chunk=%lu\n",
                    w->upload ? "up" : "down", url, (int)rc,
                    rc == CURLE_OK ? "ok" : curl_easy_strerror(rc), code,
                    (unsigned long long)(w->counter ? atomic_load_u64(w->counter) : 0),
                    w->chunk);

        if (w->upload) {
            /* Only a 2xx means the body actually landed. The add is atomic
             * because the six workers share one counter. */
            if (rc == CURLE_OK && code >= 200 && code < 300) {
                if (w->counter) atomic_add_u64(w->counter, (uint64_t)NET_BODY_BYTES);
            } else {
                sleep_ms(rc == CURLE_OK ? 50 : 100);
            }
        } else {
            ok = (rc == CURLE_OK && code >= 200 && code < 300);
            /* Cloudflare rate-limits by payload size and answers 429. Shrink
             * fast when refused, grow back slowly when willing. */
            if (ok) {
                ok_streak++;
                if (ok_streak >= 8 && w->chunk < w->chunk_max) {
                    ok_streak = 0;
                    w->chunk += w->chunk / 2;
                    if (w->chunk > w->chunk_max) w->chunk = w->chunk_max;
                }
            } else {
                ok_streak = 0;
                if (w->chunk > w->chunk_min) {
                    w->chunk /= 2;
                    if (w->chunk < w->chunk_min) w->chunk = w->chunk_min;
                } else {
                    sleep_ms(250);
                }
            }
        }
    }

    curl_easy_cleanup(c);
#if defined(_WIN32)
    /* Both exits from a thread procedure must be explicit. This tail used to
     * return only under `#if !defined(_WIN32)`, so the Windows build fell off
     * the end of a non-void DWORD function: undefined behaviour, and a
     * -Wreturn-type warning on every Win32 compile. CreateThread ignores the
     * return value for a plain thread, so 0 here matches the early exit above
     * and changes nothing about how the thread ends. */
    return 0;
#else
    return NULL;
#endif
}

WorkerHandle *net_worker_start(const char *base_url, Worker *cfg) {
    if (!base_url || !cfg) return NULL;
    WorkerCtx *ctx = (WorkerCtx *)calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    snprintf(ctx->base, sizeof ctx->base, "%s", base_url);
    ctx->cfg = *cfg;
    ctx->duration_ms = cfg->duration_ms;

    /* Allocate and fill the body here, on the creating thread, rather than
     * inside the worker. Doing it before the thread exists is what makes it
     * private: the worker only ever reads it, so there is no window in which
     * two workers could be writing the same bytes. */
    ctx->body = (unsigned char *)malloc(NET_BODY_BYTES);
    if (!ctx->body) { free(ctx); return NULL; }
    for (size_t i = 0; i < NET_BODY_BYTES; i++)
        ctx->body[i] = (unsigned char)(i * 31u + 7u);

#if defined(_WIN32)
    ctx->h.thread = CreateThread(NULL, 0, worker_main, ctx, 0, NULL);
    if (!ctx->h.thread) { free(ctx->body); free(ctx); return NULL; }
#else
    if (pthread_create(&ctx->h.thread, NULL, worker_main, ctx) != 0) {
        free(ctx->body); free(ctx); return NULL;
    }
#endif
    return &ctx->h;
}

/* Upper bound on how long a worker can stay alive after stop is requested.
 * The worker leaves its loop the moment it sees the flag or passes its
 * deadline, so the only thing that can outlast the flag is a single in-flight
 * request, and curl_base() caps those at CURLOPT_TIMEOUT (30 s). 35 s leaves
 * headroom over that cap. */
#define NET_JOIN_MS 35000

void net_worker_stop(WorkerHandle *h) {
    if (!h) return;
    /* WorkerHandle is the first member of WorkerCtx, so the caller's handle
     * and the whole context share an address; recovering it this way is what
     * lets the body be released alongside the context below. */
    WorkerCtx *ctx = (WorkerCtx *)h;
    atomic_store_i(&ctx->h.stop, 1);
#if defined(_WIN32)
    /* This used to wait 3 s, ignore the result, and free the context
     * unconditionally. curl_base() sets CURLOPT_TIMEOUT to 30 s, so one stalled
     * request outlives that wait by an order of magnitude: the thread returned
     * into a freed ctx, re-read ctx->base and wrote through *w->counter - a
     * use-after-free on both a read and a write, and a corrupted byte total on
     * the way out.
     *
     * So the thread is waited for properly, and the context is released ONLY
     * once the wait confirms the thread really exited. On the timeout path the
     * context and its body are deliberately leaked: a rare leak at exit is
     * vastly preferable to freeing memory a live thread is still reading. The
     * invariant this guarantees is that no path frees a context a running
     * worker can still reach. */
    DWORD wr = WaitForSingleObject(ctx->h.thread, NET_JOIN_MS);
    /* Closing the handle does not kill or invalidate the thread - the kernel
     * keeps the thread object alive until it exits - so this is safe even on
     * the timeout path, and it reclaims the handle rather than leaking it. */
    CloseHandle(ctx->h.thread);
    if (wr != WAIT_OBJECT_0) {
        if (dbg())
            fprintf(stderr, "[worker] join timed out after %d ms, leaking context\n", NET_JOIN_MS);
        return; /* deliberate leak - see above */
    }
#else
    /* pthread_join has no timeout, so it already provides the same guarantee:
     * it returns only once the thread has terminated. */
    pthread_join(ctx->h.thread, NULL);
#endif
    free(ctx->body);
    free(ctx);
}
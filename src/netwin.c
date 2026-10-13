/* Windows HTTP transport, built on WinHTTP.
 *
 * This is the Windows counterpart to net.c and implements the same API from
 * net.h. It exists for one reason: size.
 *
 * The libcurl build needed libcurl plus its whole dependency tree - OpenSSL
 * alone was 6.3 MB - for a 155 KB program, so a Windows install was 19 files and
 * 14.4 MB. WinHTTP ships with Windows itself, so this build is one executable
 * with no dependencies at all.
 *
 * It also removes the CA bundle. net.c had to ship a certificate file because
 * OpenSSL and GnuTLS read a trust list from disk and Windows keeps its trust
 * store in a registry database neither of them can see. WinHTTP is the Windows
 * stack and uses that store natively.
 *
 * The honest cost: WinHTTP speaks HTTP/1.1 only, where libcurl may have
 * negotiated HTTP/2 or HTTP/3. Cloudflare's speed endpoints do not require the
 * newer versions, but it is a real difference between the two builds rather
 * than a cosmetic one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include "net.h"

/* ------------------------------------------------------------- plumbing */

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

static uint64_t mono_ms(void) {
    static LARGE_INTEGER f = {0};
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1000.0 / (double)f.QuadPart);
}

static void sleep_ms(int ms) { Sleep((DWORD)ms); }

static int dbg(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("CFS_DEBUG");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

/* Set on every request. A stalled transfer must not outlive the phase that
 * started it, so this is the same order of magnitude as the 30 s the libcurl
 * build used. */
#define NET_TIMEOUT_MS 30000

/* Upper bound on how long a worker can stay alive after stop is requested. The
 * worker leaves its loop the moment it sees the flag or passes its deadline, so
 * the only thing that can outlast the flag is one in-flight request, and
 * NET_TIMEOUT_MS caps that. */
#define NET_JOIN_MS 35000

/* ------------------------------------------------------------- URL split */

typedef struct {
    char host[256];
    char path[512];
    unsigned short port;
    int https;
} Url;

static int url_split(const char *url, Url *u) {
    memset(u, 0, sizeof *u);
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { u->https = 1; u->port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { u->https = 0; u->port = 80; p += 7; }
    else return -1;

    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && i + 1 < sizeof u->host) u->host[i++] = *p++;
    u->host[i] = '\0';
    if (*p == ':') {
        unsigned long v = 0;
        p++;
        while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned long)(*p++ - '0');
        u->port = (unsigned short)v;
    }
    if (!u->host[0]) return -1;

    snprintf(u->path, sizeof u->path, "%s", (*p == '/') ? p : "/");
    return 0;
}

static int url_host(const char *url, char *out, size_t cap) {
    Url u;
    if (url_split(url, &u) != 0) return -1;
    snprintf(out, cap, "%s", u.host);
    return 0;
}

/* ------------------------------------------------------------- session */

static HINTERNET g_session;

/* One connect per request is cheap on a loopback-free public internet path and
 * removes a lifetime question: WinHttpConnect handles must be closed in the
 * reverse order of opening, and tracking that across six worker threads buys
 * nothing when the alternative is a shared, synchronised handle. */
/* This mingw-w64 header set declares only the wide entry points, with no A/W
 * macro pair at all, so every string crossing the API boundary is widened here.
 * Everything the app passes is ASCII - hostnames, paths, verbs, header lines -
 * so a byte-wise widening is exact. */
static void to_wide(const char *src, WCHAR *dst, size_t dst_chars) {
    if (!dst_chars) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; src[i] && i + 1 < dst_chars; i++) dst[i] = (WCHAR)(unsigned char)src[i];
    dst[i] = 0;
}

static HINTERNET connect_for(const Url *u, DWORD *err) {
    WCHAR wh[300];
    to_wide(u->host, wh, 300);
    HINTERNET c = WinHttpConnect(g_session, wh, u->port, 0);
    if (!c) { *err = GetLastError(); return NULL; }
    return c;
}

static HINTERNET open_req(const Url *u, const char *verb, DWORD flags) {
    HINTERNET c = connect_for(u, &(DWORD){0});
    if (!c) return NULL;
    WCHAR wverb[16], wpath[600];
    to_wide(verb, wverb, 16);
    to_wide(u->path, wpath, 600);
    /* The accept-types argument is a pointer to a wide string, so NULL - "send
     * no Accept header" - is the correct value. This build sends none. */
    HINTERNET r = WinHttpOpenRequest(c, wverb, wpath, NULL, WINHTTP_NO_REFERER,
                                     NULL, flags);
    WinHttpCloseHandle(c);
    return r;
}

/* Set a millisecond timeout on a request handle. WinHTTP's defaults are long
 * enough that a dead connection can pin a worker past its phase deadline. */
static void set_timeouts(HINTERNET r) {
    DWORD t = NET_TIMEOUT_MS;
    WinHttpSetTimeouts(r, (int)t, (int)t, (int)t, (int)t);
}

/* Status code of a completed request, or 0. */
static DWORD status_of(HINTERNET req) {
    DWORD code = 0, len = sizeof code;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return code;
}

/* Perform a GET and return the body. Used only for the small metadata calls;
 * the throughput path streams and counts instead. */
static int http_get_text(const char *url, char **out, size_t *out_len, DWORD *code) {
    *out = NULL;
    *out_len = 0;
    *code = 0;

    Url u;
    if (url_split(url, &u) != 0) return NET_ERR;

    HINTERNET req = open_req(&u, "GET", 0);
    if (!req) return NET_ERR;
    set_timeouts(req);

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0)) {
        WinHttpCloseHandle(req);
        return NET_ERR;
    }
    if (!WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        return NET_ERR;
    }
    *code = status_of(req);

    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { WinHttpCloseHandle(req); return NET_ERR; }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (avail == 0) break;
        if (len + avail + 1 > cap) {
            size_t ncap = cap;
            while (ncap < len + avail + 1) ncap *= 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) { free(buf); WinHttpCloseHandle(req); return NET_ERR; }
            buf = nb;
            cap = ncap;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + len, avail, &got)) break;
        len += got;
    }
    WinHttpCloseHandle(req);

    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return NET_OK;
}

/* ------------------------------------------------------------- helpers */

static void copy_field(char *dst, size_t cap, const char *src) {
    if (!dst || !src || cap == 0) return;
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

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

/* /cdn-cgi/trace is a flat key=value listing, one pair per line. Matching is
 * anchored to the start of a line: a plain substring search for "ip=" would
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

/* ------------------------------------------------------------- lifecycle */

int net_init(void) {
    WSADATA wd;
    /* Exactly one WSAStartup for the process, matched by the one WSACleanup in
     * net_shutdown. Calling WSACleanup anywhere else drops the refcount to zero
     * and deinitialises Winsock process-wide, which breaks every later request. */
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return NET_ERR;

    g_session = WinHttpOpen(L"cfspeed/0.2",
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) {
        WSACleanup();
        return NET_ERR;
    }
    /* Match net.c's curl_base() agent string. The length is the header's own
     * length: WinHTTP treats the buffer as raw bytes here, not a C string. */
    WCHAR ua_hdr[64];
    to_wide("User-Agent: cfspeed", ua_hdr, 64);
    WinHttpSetOption(g_session, WINHTTP_OPTION_USER_AGENT, ua_hdr,
                     (DWORD)wcslen(ua_hdr));
    return NET_OK;
}

void net_shutdown(void) {
    if (g_session) {
        WinHttpCloseHandle(g_session);
        g_session = NULL;
    }
    WSACleanup();
}

/* ------------------------------------------------------------- metadata */

void net_fetch_meta(const char *base_url, Meta *m) {
    memset(m, 0, sizeof *m);

    /* Time DNS separately: resolution is a real part of connection setup and
     * conflating it with TLS hides which one is slow. */
    char host[256];
    if (url_host(base_url, host, sizeof host) == 0) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        uint64_t t0 = mono_ms();
        struct addrinfo *res = NULL;
        int rc = getaddrinfo(host, NULL, &hints, &res);
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
            /* Release the addresses. This must NOT be WSACleanup: that would
             * drop the process-wide Winsock refcount to zero. */
            freeaddrinfo(res);
        }
    }

    /* A small request whose timing splits connection setup from transfer. */
    {
        char url[512];
        snprintf(url, sizeof url, "%s/__down?bytes=0", base_url);
        DWORD code = 0;
        char *body = NULL;
        size_t blen = 0;
        uint64_t t0 = mono_ms();
        if (http_get_text(url, &body, &blen, &code) == NET_OK) {
            double pre = (double)(mono_ms() - t0);
            m->tls_ms = pre;
            if (code >= 200 && code < 300)
                snprintf(m->http, sizeof m->http, "HTTP/%lu.%lu",
                         (unsigned long)(code / 100), (unsigned long)(code % 100));
            free(body);
        }
    }

    /* The colo, the client IP and the location come from /cdn-cgi/trace.
     * /meta answers an empty object {} for most clients now, so parsing it left
     * every one of these fields blank - the app ran and measured correctly but
     * could not say where it was. */
    {
        char url[512];
        snprintf(url, sizeof url, "%s/cdn-cgi/trace", base_url);
        DWORD code = 0;
        char *body = NULL;
        size_t blen = 0;
        if (http_get_text(url, &body, &blen, &code) == NET_OK && body) {
            char v[256];
            if (trace_field(body, "ip", v, sizeof v)) copy_field(m->ip, sizeof m->ip, v);
            if (trace_field(body, "colo", v, sizeof v)) copy_field(m->colo, sizeof m->colo, v);
            if (trace_field(body, "loc", v, sizeof v)) copy_field(m->loc, sizeof m->loc, v);
            if (trace_field(body, "warp", v, sizeof v)) copy_field(m->warp, sizeof m->warp, v);
            if (trace_field(body, "tls", v, sizeof v)) copy_field(m->tls, sizeof m->tls, v);
            if (trace_field(body, "http", v, sizeof v)) {
                for (size_t i = 0; v[i] && i < 4; i++)
                    if (v[i] >= 'a' && v[i] <= 'z') v[i] = (char)(v[i] - 32);
                copy_field(m->http, sizeof m->http, v);
            }
            free(body);
        }
    }

    /* /meta is kept only for the ASN and cipher fields trace does not expose.
     * It answers {} for many clients, in which case these simply stay empty and
     * the views omit the rows rather than printing a zero. */
    {
        char url[512];
        snprintf(url, sizeof url, "%s/meta", base_url);
        DWORD code = 0;
        char *body = NULL;
        size_t blen = 0;
        if (http_get_text(url, &body, &blen, &code) == NET_OK && body) {
            char tmp[256];
            const char *p = strstr(body, "\"asnOrg\"");
            if (p) {
                const char *o = strchr(p, '{');
                if (o && json_str(o, "name", tmp, sizeof tmp))
                    copy_field(m->asn_org, sizeof m->asn_org, tmp);
            }
            const char *a = strstr(body, "\"asn\"");
            if (a && json_str(a, "asn", tmp, sizeof tmp))
                copy_field(m->asn, sizeof m->asn, tmp);
            const char *t = strstr(body, "\"tls\"");
            if (t) {
                const char *o = strchr(t, '{');
                if (o) {
                    if (json_str(o, "tlsVersion", tmp, sizeof tmp) && !m->tls[0])
                        copy_field(m->tls, sizeof m->tls, tmp);
                    if (json_str(o, "cipher", tmp, sizeof tmp))
                        copy_field(m->tls_cipher, sizeof m->tls_cipher, tmp);
                }
            }
            free(body);
        }
    }
}

/* ------------------------------------------------------------- latency */

int net_probe(const char *base_url, double *ms) {
    if (dbg()) fprintf(stderr, "[probe] start\n");
    char url[512];
    snprintf(url, sizeof url, "%s/__down?bytes=0", base_url);
    DWORD code = 0;
    char *body = NULL;
    size_t blen = 0;

    uint64_t t0 = mono_ms();
    int rc = http_get_text(url, &body, &blen, &code);
    double elapsed = (double)(mono_ms() - t0);
    free(body);

    if (dbg()) fprintf(stderr, "[probe] rc=%d http=%lu ms=%.1f\n", rc, code, elapsed);
    if (rc != NET_OK) return NET_ERR;
    if (code < 200 || code >= 300) return NET_REFUSED;
    *ms = elapsed;
    return NET_OK;
}

/* -------------------------------------------------------------- workers */

struct WorkerHandle {
    HANDLE thread;
    /* Cross-thread visibility comes from the __atomic builtins on every access,
     * not from `volatile`. volatile is not a synchronisation primitive: it
     * constrains the compiler but orders nothing and is not atomic. That matters
     * for more than tidiness here - it is what lets net_worker_stop()'s join
     * observe the flag and return, which in turn is what guarantees the context
     * is dead before it is freed. */
    int stop;
};

/* A fixed 1 MiB upload body. Earlier this streamed an endless body and counted
 * as reqwest pulled chunks from it, which measures bytes handed to the kernel
 * rather than bytes delivered - it reported over 1 Gbps on a link that sustains
 * ~130 Mbps. Counting only on a 2xx response is ground truth.
 *
 * Per worker, not static. It used to be a function-local `static`, so all six
 * workers re-initialised the same 1 MiB buffer concurrently while other workers
 * were POSTing from it. Since the body is filled once and then posted many
 * times, per-worker storage removes the shared state outright. */
#define NET_BODY_BYTES (1024u * 1024u)

typedef struct {
    struct WorkerHandle h;
    char base[256];
    uint64_t duration_ms;
    Worker cfg;
    unsigned char *body;
} WorkerCtx;

/* Stream a download, counting bytes off the socket. Returns 0 on success and
 * the HTTP status otherwise, with *count set to whatever actually arrived.
 *
 * Counting here rather than from a content-length header is deliberate: these
 * bytes are off the socket and on the wire, so the count is real and
 * time-accurate. */
static int do_download(const char *base, unsigned long chunk, Worker *w, uint64_t *count) {
    char url[512];
    snprintf(url, sizeof url, "%s/__down?bytes=%lu", base, chunk);
    Url u;
    if (url_split(url, &u) != 0) return 0;

    DWORD code = 0;
    HINTERNET req = open_req(&u, "GET", 0);
    if (!req) return 0;
    set_timeouts(req);

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        return 0;
    }
    code = status_of(req);

    char buf[65536];
    uint64_t local = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (avail == 0) break;
        DWORD want = avail;
        if (want > (DWORD)sizeof buf) want = (DWORD)sizeof buf;
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, want, &got)) break;
        if (got == 0) break;
        local += got;
        /* Added here rather than per-read so six workers contending on one
         * counter lose far fewer updates than a per-callback add would. */
        if (w->counter) atomic_add_u64(w->counter, (uint64_t)got);
    }
    WinHttpCloseHandle(req);
    if (count) *count = local;
    return (int)code;
}

/* POST the body. Returns the HTTP status, or 0 if the request never completed.
 * The body is NOT counted here: net.c adds it only on a 2xx, and that rule is
 * the whole reason the upload number is worth anything. */
static int do_upload(const char *base, const unsigned char *body, size_t body_len) {
    char url[512];
    snprintf(url, sizeof url, "%s/__up", base);
    Url u;
    if (url_split(url, &u) != 0) return 0;

    /* "Expect: 100-continue" would add a round trip per body; Cloudflare does
     * not need it and it would show up in the timing. */
    WCHAR hdrs[16];
    to_wide("Expect:\r\n", hdrs, 16);

    HINTERNET req = open_req(&u, "POST", 0);
    if (!req) return 0;
    set_timeouts(req);

    BOOL ok = WinHttpSendRequest(req, hdrs, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0,
                                 (DWORD)body_len, 0);
    if (ok) {
        const unsigned char *p = body;
        size_t left = body_len;
        while (left > 0) {
            DWORD chunk = (left > 1u << 20) ? (1u << 20) : (DWORD)left;
            DWORD put = 0;
            if (!WinHttpWriteData(req, p, chunk, &put) || put == 0) { ok = FALSE; break; }
            p += put;
            left -= put;
        }
    }
    if (ok) ok = WinHttpReceiveResponse(req, NULL);

    int code = ok ? (int)status_of(req) : 0;
    /* Drain the response so the connection can be reused and closed cleanly. */
    if (ok) {
        char buf[4096];
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            DWORD got = 0;
            if (!WinHttpReadData(req, buf, (avail > sizeof buf) ? (DWORD)sizeof buf : avail, &got))
                break;
            if (got == 0) break;
        }
    }
    WinHttpCloseHandle(req);
    return code;
}

static DWORD WINAPI worker_main(LPVOID arg) {
    WorkerCtx *ctx = (WorkerCtx *)arg;
    Worker *w = &ctx->cfg;
    const char *base = ctx->base;
    uint64_t deadline = mono_ms() + ctx->duration_ms;

    /* The upload body is allocated and filled by net_worker_start() before this
     * thread exists, so it is private to this worker with nothing to race. */
    const unsigned char *const body = ctx->body;

    int ok_streak = 0;

    while (mono_ms() < deadline && !atomic_load_i(&ctx->h.stop)) {
        int code;

        if (w->upload) {
            code = do_upload(base, body, NET_BODY_BYTES);
            /* Only a 2xx means the body actually landed. The add is atomic
             * because the six workers share one counter. */
            if (code >= 200 && code < 300) {
                if (w->counter) atomic_add_u64(w->counter, (uint64_t)NET_BODY_BYTES);
            } else {
                sleep_ms(code ? 50 : 100);
            }
        } else {
            uint64_t got = 0;
            code = do_download(base, w->chunk, w, &got);
            int ok = (code >= 200 && code < 300);
            if (dbg())
                fprintf(stderr, "[worker %s] url=%s/__down?bytes=%lu http=%d got=%llu chunk=%lu\n",
                        "down", base, w->chunk, code, (unsigned long long)got, w->chunk);
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

    /* CreateThread ignores a plain thread's return value; this is here so both
     * exits from the procedure are explicit rather than falling off the end. */
    return 0;
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

    ctx->h.thread = CreateThread(NULL, 0, worker_main, ctx, 0, NULL);
    if (!ctx->h.thread) { free(ctx->body); free(ctx); return NULL; }
    return &ctx->h;
}

void net_worker_stop(WorkerHandle *h) {
    if (!h) return;
    /* WorkerHandle is the first member of WorkerCtx, so the caller's handle and
     * the whole context share an address; recovering it this way is what lets
     * the body be released alongside the context below. */
    WorkerCtx *ctx = (WorkerCtx *)h;
    atomic_store_i(&ctx->h.stop, 1);

    /* The context is released ONLY once the wait confirms the thread really
     * exited. On the timeout path the context and its body are deliberately
     * leaked: a rare leak at exit is vastly preferable to freeing memory a live
     * thread is still reading. The invariant is that no path frees a context a
     * running worker can still reach. */
    DWORD wr = WaitForSingleObject(ctx->h.thread, NET_JOIN_MS);
    /* Closing a handle does not kill or invalidate the thread - the kernel keeps
     * the thread object alive until it exits - so this is safe on both paths. */
    CloseHandle(ctx->h.thread);
    if (wr != WAIT_OBJECT_0) {
        if (dbg())
            fprintf(stderr, "[worker] join timed out after %d ms, leaking context\n", NET_JOIN_MS);
        return; /* deliberate leak - see above */
    }
    free(ctx->body);
    free(ctx);
}
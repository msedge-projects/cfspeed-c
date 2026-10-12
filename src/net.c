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
#else
#  include <pthread.h>
#  include <time.h>
#  include <sys/time.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

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

static void curl_base(CURL *c, const char *url) {
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "cfspeed/1.0");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
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
#if defined(_WIN32)
            WSACleanup();
#else
            freeaddrinfo(res);
#endif
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

    /* /meta carries the colo, the client IP and the ASN. */
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
            if (json_str(sink.buf, "ip", tmp, sizeof tmp))
                snprintf(m->ip, sizeof m->ip, "%s", tmp);
            if (json_str(sink.buf, "colo", tmp, sizeof tmp))
                snprintf(m->colo, sizeof m->colo, "%s", tmp);
            if (json_str(sink.buf, "location", tmp, sizeof tmp))
                snprintf(m->loc, sizeof m->loc, "%s", tmp);
            if (json_str(sink.buf, "warp", tmp, sizeof tmp))
                snprintf(m->warp, sizeof m->warp, "%s", tmp);
            if (json_str(sink.buf, "httpProtocol", tmp, sizeof tmp))
                snprintf(m->http, sizeof m->http, "%s", tmp);

            /* asnOrg is a nested object; grab its two string fields. */
            if (json_str(sink.buf, "asnOrg", tmp, sizeof tmp)) { /* object, skip */ }
            const char *p = strstr(sink.buf, "\"asnOrg\"");
            if (p) {
                const char *o = strchr(p, '{');
                if (o && json_str(o, "name", tmp, sizeof tmp))
                    snprintf(m->asn_org, sizeof m->asn_org, "%s", tmp);
                const char *a = strstr(sink.buf, "\"asn\"");
                if (a && json_str(a, "asn", tmp, sizeof tmp))
                    snprintf(m->asn, sizeof m->asn, "%s", tmp);
            }
            const char *t = strstr(sink.buf, "\"tls\"");
            if (t) {
                const char *o = strchr(t, '{');
                if (o) {
                    if (json_str(o, "tlsVersion", tmp, sizeof tmp))
                        snprintf(m->tls, sizeof m->tls, "%s", tmp);
                    if (json_str(o, "cipher", tmp, sizeof tmp))
                        snprintf(m->tls_cipher, sizeof m->tls_cipher, "%s", tmp);
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
    volatile int stop;
};

typedef struct {
    struct WorkerHandle h;
    char base[256];
    uint64_t duration_ms;
    Worker cfg;
} WorkerCtx;

static size_t progress_cb(char *p, size_t sz, size_t n, void *ud) {
    (void)p;
    Worker *w = (Worker *)ud;
    /* Download progress is real: these bytes are off the socket and on the
     * wire, so counting here is legitimate and time-accurate. */
    if (!w->upload && w->counter)
        *w->counter += (uint64_t)(sz * n);
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

    /* A fixed 1 MiB body. Earlier this streamed an endless body and counted as
     * reqwest pulled chunks from it, which measures bytes handed to the kernel
     * rather than bytes delivered - it reported over 1 Gbps on a link that
     * sustains ~130 Mbps. Counting only on a 2xx response is ground truth. */
    static unsigned char body[1024 * 1024];
    for (size_t i = 0; i < sizeof body; i++)
        body[i] = (unsigned char)(i * 31u + 7u);

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

    while (mono_ms() < deadline && !ctx->h.stop) {
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
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)sizeof body);
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
                    (unsigned long long)(w->counter ? *w->counter : 0), w->chunk);

        if (w->upload) {
            /* Only a 2xx means the body actually landed. */
            if (rc == CURLE_OK && code >= 200 && code < 300) {
                if (w->counter) *w->counter += (uint64_t)sizeof body;
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
#if !defined(_WIN32)
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
#if defined(_WIN32)
    ctx->h.thread = CreateThread(NULL, 0, worker_main, ctx, 0, NULL);
    if (!ctx->h.thread) { free(ctx); return NULL; }
#else
    if (pthread_create(&ctx->h.thread, NULL, worker_main, ctx) != 0) { free(ctx); return NULL; }
#endif
    return &ctx->h;
}

void net_worker_stop(WorkerHandle *h) {
    if (!h) return;
    h->stop = 1;
#if defined(_WIN32)
    WaitForSingleObject(h->thread, 3000);
    CloseHandle(h->thread);
#else
    pthread_join(h->thread, NULL);
#endif
    free(h); /* WorkerHandle is the first member of WorkerCtx */
}
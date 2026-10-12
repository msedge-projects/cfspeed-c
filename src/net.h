/* HTTP transport.
 *
 * Everything the measurement engine does goes through here, so the transport
 * can be swapped without touching the phase logic. Backed by libcurl's multi
 * interface: one easy handle per in-flight request, driven from a single loop.
 */
#ifndef CFS_NET_H
#define CFS_NET_H

#include <stdint.h>
#include <stddef.h>

#define NET_OK        0
#define NET_ERR      -1
#define NET_REFUSED  -2   /* the server refused us; not a speed result */

/* --- lifecycle ----------------------------------------------------------- */
int  net_init(void);
void net_shutdown(void);

/* --- metadata ------------------------------------------------------------ */
typedef struct {
    char ip[64];
    char colo[16];
    char loc[64];
    char tls[32];
    char tls_cipher[64];
    char http[32];
    char warp[32];
    char asn[16];
    char asn_org[96];
    char edge_ips[8][72];
    int  edge_ip_count;
    double dns_ms;
    double tls_ms;
} Meta;

typedef struct {
    double avg, min, med, p25, p75, p95, p99, max, jitter, loss;
    int    samples;
} LatStats;

/* Fetch connection metadata. Never fails the run: on error the fields are left
 * empty and the UI shows placeholders. */
void net_fetch_meta(const char *base_url, Meta *out);

/* --- latency ------------------------------------------------------------- */
/* One probe. Returns 0 on success (ms written to *ms), NET_REFUSED when the
 * request did not come back, NET_ERR otherwise. A refused probe counts toward
 * loss; an errored one does not. */
int net_probe(const char *base_url, double *ms);

/* --- throughput ---------------------------------------------------------- */
typedef struct {
    uint64_t *counter;  /* cumulative confirmed bytes */
    const unsigned char *body;
    size_t   body_len;
    int      upload;    /* 1 = POST, 0 = GET */
    size_t   want;      /* download: bytes requested per request */
    unsigned long chunk;/* adaptive download request size */
    unsigned long chunk_min;
    unsigned long chunk_max;
    int       stalled;  /* last request was refused */
    uint64_t  duration_ms;
} Worker;

/* Start/stop a worker thread. Cancellation is cooperative: the worker checks
 * its deadline and its cancel flag between requests. */
typedef struct WorkerHandle WorkerHandle;

WorkerHandle *net_worker_start(const char *base_url, Worker *cfg);
void          net_worker_stop(WorkerHandle *h);

#endif /* CFS_NET_H */
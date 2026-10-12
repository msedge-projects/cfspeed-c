/* One definition of every number the app prints.
 *
 * The live tab, the detail views and the JSON writer each grew their own
 * formatting and they drifted: the same sample came out as "110 Mbps" on one
 * tab and "110M" on another. Fixed output shapes are what let a number mean the
 * same thing in a chart, in a table and in a report.
 *
 * Every function here is total. Hand one a cap of 0, a cap of 1, a NULL string
 * or a NaN and it returns a correctly clipped string or the "--" placeholder -
 * never a buffer overrun, and never "nan" or "inf" on a terminal.
 */
#include "fmt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* A value we could not measure. "--" rather than "0" because a missing sample
 * that reads as a real zero is the one formatting bug nobody notices. */
#define MISSING "--"

/* snprintf already clips to cap, so every write below is bounded by
 * construction. No call site indexes `out` with the snprintf return: that count
 * is the length the format *would* have produced, and using it to walk a short
 * buffer is exactly how these helpers would overrun. */

/* NaN fails every comparison and is caught by the finite test, but an infinity
 * sails straight through a >= 0.0 check - and "inf" printed into a terminal
 * cell is exactly the corruption this module exists to prevent. Hence the
 * explicit isfinite rather than the comparison alone. */
static int has_measurement(double v) { return isfinite(v) && v >= 0.0; }

static void fmt_missing(char *out, size_t cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", MISSING);
}

/* ------------------------------------------------------------------- rates */

/* Deliberately one fixed shape. The live tab used to drop the decimal above
 * 100 and switch to Gbps above 1000, which meant a 110.5 sample printed
 * "111 Mbps" on one screen and "110.5" on another. */
void fmt_rate(char *out, size_t cap, double mbps) {
    if (!out || cap == 0) return;
    if (!has_measurement(mbps)) { fmt_missing(out, cap); return; }
    snprintf(out, cap, "%.1f", mbps);
}

/* -------------------------------------------------------------- millisecs */

/* Past this the figure stops being a latency and becomes a column width; the
 * unit and the decimal are both noise at that magnitude. */
#define MS_PLAIN_MAX 9999.0

void fmt_ms(char *out, size_t cap, double ms) {
    if (!out || cap == 0) return;
    if (!has_measurement(ms)) { fmt_missing(out, cap); return; }
    if (ms > MS_PLAIN_MAX) { snprintf(out, cap, "%.0f", ms); return; }

    snprintf(out, cap, "%.1f", ms);
    /* "12" rather than "12.0": a trailing zero on a whole number costs a column
     * and says nothing the unit does not already say. The shapes documented in
     * fmt.h - "9.4", "12", "47.3", "812" - are what this reproduces. Cut at the
     * dot so the zero goes with it. */
    size_t n = strlen(out);
    if (n >= 2 && out[n - 2] == '.' && out[n - 1] == '0') out[n - 2] = '\0';
}

/* ------------------------------------------------------------------ bytes */

/* Shifts rather than powers of a double, so the thresholds are compared in
 * exact integer arithmetic and the largest count cannot round into the wrong
 * bucket on the way to being printed. */
#define BYTES_KB ((uint64_t)1 << 10)
#define BYTES_MB ((uint64_t)1 << 20)
#define BYTES_GB ((uint64_t)1 << 30)

void fmt_bytes(char *out, size_t cap, uint64_t bytes) {
    if (!out || cap == 0) return;
    /* The divisors are non-zero constants, so there is no divide-by-zero to
     * guard; they are spelled out per rung to keep each conversion exact. */
    if (bytes >= BYTES_GB)      snprintf(out, cap, "%.1f GB", (double)bytes / 1073741824.0);
    else if (bytes >= BYTES_MB) snprintf(out, cap, "%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= BYTES_KB) snprintf(out, cap, "%.0f KB", (double)bytes / 1024.0);
    else                        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
}

/* ---------------------------------------------------------------- percent */

void fmt_pct(char *out, size_t cap, double percent) {
    if (!out || cap == 0) return;
    /* Only non-finite is masked here. A negative loss cannot happen, but if one
     * ever arrives the sign is information and hiding it would be a lie about
     * the measurement rather than a formatting choice. */
    if (!isfinite(percent)) { fmt_missing(out, cap); return; }
    /* No trailing-zero trim, unlike fmt_ms: 0.0% loss is a result worth seeing,
     * and "0%" reads like an absent value. */
    snprintf(out, cap, "%.1f", percent);
}

/* --------------------------------------------------------------- timestamp */

/* 9999-12-31T23:59:59Z in milliseconds. Past this the year no longer fits the
 * printed shape and a 32-bit time_t would wrap to an unrelated date, so it is
 * treated as unprintable rather than guessed at. */
#define STAMP_MAX_MS INT64_C(253402300799000)

void fmt_stamp(char *out, size_t cap, int64_t unix_ms) {
    if (!out || cap == 0) return;
    /* Zero is the epoch. Printing 1970 for it would look like a real run from
     * 1970 rather than a run that never recorded a timestamp. */
    if (unix_ms <= 0 || unix_ms > STAMP_MAX_MS) { fmt_missing(out, cap); return; }

    int64_t whole = unix_ms / 1000;
    /* Narrowing to time_t wraps silently when it is 32 bits. The round trip
     * catches that instead of printing a confidently wrong date. */
    if ((int64_t)(time_t)whole != whole) { fmt_missing(out, cap); return; }

    time_t secs = (time_t)whole;
    struct tm tmv;
    int ok;
#if defined(_WIN32)
    ok = (localtime_s(&tmv, &secs) == 0);
#else
    ok = (localtime_r(&secs, &tmv) != NULL);
#endif
    /* A rejected timestamp is a missing one, not a crash and not a guess. */
    if (!ok) { fmt_missing(out, cap); return; }

    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

/* --------------------------------------------------------------- duration */

/* Clamped before the narrowing cast: converting a double outside the unsigned
 * range is undefined, and 1e9 seconds is already 31 years of uptime. */
#define DURATION_MAX_S 1.0e9

void fmt_duration(char *out, size_t cap, double seconds) {
    if (!out || cap == 0) return;
    /* The two values with no sensible clamp. They take the same path as a
     * missing measurement rather than reaching printf as inf/nan. */
    if (!isfinite(seconds)) { fmt_missing(out, cap); return; }
    /* A negative elapsed time is noise from a clock adjustment, not a duration
     * worth rendering with a leading minus. */
    if (seconds < 0.0) seconds = 0.0;
    if (seconds > DURATION_MAX_S) seconds = DURATION_MAX_S;

    /* Truncated like a stopwatch: 59.9s of uptime is 59 seconds, and rounding
     * up would print a minute that has not happened yet. */
    unsigned long long t = (unsigned long long)seconds;
    unsigned long long h = t / 3600u;
    unsigned long long m = (t / 60u) % 60u;
    unsigned long long s = t % 60u;

    /* Seconds drop once hours appear: past an hour the reader is looking at how
     * long the run took, not at what it is doing this second. */
    if (h > 0)      snprintf(out, cap, "%lluh %02llum", h, m);
    else if (m > 0) snprintf(out, cap, "%llum %02llus", m, s);
    else            snprintf(out, cap, "%llus", s);
}

/* --------------------------------------------------------------- truncate */

/* U+2026, the ellipsis the rest of the UI already uses: one column on screen,
 * three bytes in the buffer. A single ASCII dot would be a column cheaper and
 * would not look like the same UI. */
#define ELLIPSIS    "\xe2\x80\xa6"
#define ELLIPSIS_B  3u
#define ELLIPSIS_W  1u

void fmt_truncate(char *out, size_t cap, const char *s, size_t w) {
    if (!out || cap == 0) return;

    /* A NULL source is a field that was never filled in, not a crash. */
    const char *src = s ? s : "";
    size_t n = strlen(src);

    /* Nothing asked for, or nothing to show. */
    if (w == 0 || n == 0) { out[0] = '\0'; return; }

    if (n <= w) {
        size_t take = (n < cap - 1) ? n : cap - 1;
        memcpy(out, src, take);
        out[take] = '\0';
        return;
    }

    size_t room = cap - 1; /* bytes available for content plus the NUL */
    /* Not even the marker fits, so the honest answer is nothing rather than a
     * truncated glyph. */
    if (room < ELLIPSIS_B) { out[0] = '\0'; return; }

    size_t keep = (w > ELLIPSIS_W) ? w - ELLIPSIS_W : 0;
    /* cap and the ellipsis are paid out of the same budget. */
    if (keep > room - ELLIPSIS_B) keep = room - ELLIPSIS_B;
    /* Never stop between bytes of one glyph: a clipped code point shows up as a
     * replacement character, which is worse than giving up a column. */
    while (keep > 0 && ((unsigned char)src[keep] & 0xC0u) == 0x80u) keep--;

    memcpy(out, src, keep);
    memcpy(out + keep, ELLIPSIS, ELLIPSIS_B);
    out[keep + ELLIPSIS_B] = '\0';
}
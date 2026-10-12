#ifndef CFS_FMT_H
#define CFS_FMT_H
#include <stddef.h>
#include <stdint.h>

/* Rate in Mbps, always 1 decimal place: "110.5". A negative or non-finite
   input yields "--" so a missing measurement never looks like a real zero. */
void fmt_rate(char *out, size_t cap, double mbps);

/* Milliseconds with no decimal below 10 ("9.4", "12"), one decimal above
   ("47.3", "812"). Values over 9999 get no unit and no decimals: "12000".
   Negative/non-finite yields "--". */
void fmt_ms(char *out, size_t cap, double ms);

/* Bytes as a human unit: "5.2 MB", "812 KB", "1.4 GB". Binary units (KiB
   style divisors of 1024) but spelled KB/MB/GB, matching common convention.
   0 yields "0 B". */
void fmt_bytes(char *out, size_t cap, uint64_t bytes);

/* Loss / percent with 1 decimal, except exactly 0 which yields "0.0".
   Non-finite yields "--". */
void fmt_pct(char *out, size_t cap, double percent);

/* Wall-clock ms since the epoch rendered as "2026-10-12 14:03". Local time.
   A zero or negative input yields "--". */
void fmt_stamp(char *out, size_t cap, int64_t unix_ms);

/* Elapsed seconds as "1h 02m" / "4m 11s" / "38s". Never negative. */
void fmt_duration(char *out, size_t cap, double seconds);

/* Truncate `s` to width `w` columns, appending an ellipsis when it does not
   fit, always NUL-terminating and never writing past cap. */
void fmt_truncate(char *out, size_t cap, const char *s, size_t w);

#endif
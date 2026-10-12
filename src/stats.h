#ifndef CFS_STATS_H
#define CFS_STATS_H
#include <stddef.h>

/* Percentile via nearest-rank on an already-sorted array. p is 0..1.
   n==0 returns 0. Caller need not sort; these take unsorted input. */
double stats_percentile(const double *v, size_t n, double p);
double stats_mean(const double *v, size_t n);
double stats_median(const double *v, size_t n);
double stats_min(const double *v, size_t n);
double stats_max(const double *v, size_t n);
double stats_stddev(const double *v, size_t n);   /* population, not sample */

/* RFC 3550 style: mean absolute successive difference. n<2 -> 0. */
double stats_jitter(const double *v, size_t n);

/* Coefficient of variation as a PERCENT (stddev/mean*100). mean==0 -> 0. */
double stats_cv_percent(const double *v, size_t n);

/* Packet loss as a percent. sent==0 -> 0. */
double stats_loss_percent(size_t received, size_t sent);

/* Median of the difference between a loaded-latency sample and the idle
   median. loaded==NULL or idle<=0 -> 0. */
double stats_bufferbloat(const double *loaded, size_t n, double idle_median);

/* Letter grades. Deliberately coarse: return one of 'A'..'F' followed by
   nothing. Thresholds are documented per function.
   bufferbloat: <=5 'A', <=30 'B', <=50 'C', <=100 'D', <=200 'E', else 'F'
   stability (cv%): <=2 'A', <=4 'B', <=8 'C', <=15 'D', <=25 'E', else 'F'
   loss (%): ==0 'A', <=0.5 'B', <=1 'C', <=3 'D', <=8 'E', else 'F' */
char stats_grade_bufferbloat(double ms);
char stats_grade_stability(double cv_percent);
char stats_grade_loss(double loss_percent);

/* Outlier-resistant average: mean of the values between the 5th and 95th
   percentiles. Returns mean(v) when fewer than 8 samples. */
double stats_trimmed_mean(const double *v, size_t n);
#endif
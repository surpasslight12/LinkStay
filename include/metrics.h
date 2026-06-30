#ifndef LINKSTAY_METRICS_H
#define LINKSTAY_METRICS_H

/*
 * metrics.h — ping statistics aggregation.
 *
 * A small value type tracking counts, latency extremes, and uptime. All
 * accessors tolerate a null pointer and an empty sample set so callers on
 * the hot path need not special-case the "no data yet" state.
 */

#include "common.h"

typedef struct {
  uint64_t total_pings;
  uint64_t successful_pings;
  uint64_t failed_pings;
  double total_latency;
  double min_latency; /* -1.0 sentinel: not yet recorded */
  double max_latency; /* -1.0 sentinel: not yet recorded */
  uint64_t start_time_ms;
} metrics_t;

void metrics_init(metrics_t *metrics);
void metrics_record_success(metrics_t *metrics, double latency_ms);
void metrics_record_failure(metrics_t *metrics);
double metrics_success_rate(const metrics_t *metrics);
double metrics_avg_latency(const metrics_t *metrics);
uint64_t metrics_uptime_seconds(const metrics_t *metrics);

#endif /* LINKSTAY_METRICS_H */

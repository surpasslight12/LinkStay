#ifndef LINKSTAY_STATS_H
#define LINKSTAY_STATS_H

/*
 * stats.h — ping statistics value type.
 *
 * Aggregates counts, latency extremes, and uptime. Pure value semantics:
 * no I/O, no global state.
 */

#include "base.h"

typedef struct {
  uint64_t total;
  uint64_t ok;
  uint64_t failed;
  double latency_sum;      /* over successful pings, in ms */
  double latency_min;      /* -1.0 = no sample yet */
  double latency_max;      /* -1.0 = no sample yet */
  uint64_t started_at_ms;  /* monotonic start time */
} ls_stats_t;

void ls_stats_init(ls_stats_t *stats);
void ls_stats_add_ok(ls_stats_t *stats, double latency_ms);
void ls_stats_add_fail(ls_stats_t *stats);
[[nodiscard]] double ls_stats_success_rate(const ls_stats_t *stats); /* 0-100 */
[[nodiscard]] double ls_stats_avg_latency(const ls_stats_t *stats);
[[nodiscard]] uint64_t ls_stats_uptime_sec(const ls_stats_t *stats);

#endif /* LINKSTAY_STATS_H */

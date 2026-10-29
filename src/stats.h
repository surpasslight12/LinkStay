#ifndef LINKSTAY_STATS_H
#define LINKSTAY_STATS_H

/*
 * stats.h — cumulative probe statistics.
 *
 * The accumulator is a plain value type: app.h embeds it by value because the
 * design is heap-free. Latency is kept in microseconds so the running sum stays
 * integral; the latency fields hold no sample until the first success (they use
 * an internal "no sample" sentinel).
 */

#include "base.h"

typedef struct {
  uint64_t ok;
  uint64_t failed;
  uint64_t latency_sum_us;
  uint64_t latency_min_us;
  uint64_t latency_max_us;
  uint64_t started_at_ms;
} ls_stats_t;

/* Accumulators update the counters; getters derive display values (uptime
 * reads the monotonic clock). */
void ls_stats_init(ls_stats_t *restrict stats);
void ls_stats_add_ok(ls_stats_t *restrict stats, double latency_ms);
void ls_stats_add_fail(ls_stats_t *restrict stats);

[[nodiscard]] uint64_t ls_stats_total(const ls_stats_t *restrict stats);
[[nodiscard]] double ls_stats_success_rate(const ls_stats_t *restrict stats);
[[nodiscard]] double ls_stats_avg_latency_ms(const ls_stats_t *restrict stats);
[[nodiscard]] double ls_stats_min_latency_ms(const ls_stats_t *restrict stats);
[[nodiscard]] double ls_stats_max_latency_ms(const ls_stats_t *restrict stats);
[[nodiscard]] uint64_t ls_stats_uptime_sec(const ls_stats_t *restrict stats);

#endif /* LINKSTAY_STATS_H */

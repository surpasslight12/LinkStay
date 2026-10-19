/*
 * stats.c — cumulative probe counters and latency aggregates. Arithmetic plus
 * the monotonic clock for uptime; the caller owns the ls_stats_t.
 */

#include "stats.h"

/* Internal "no sample" sentinel and microsecond conversion factor. */
#define LS_STATS_NO_SAMPLE UINT64_MAX
#define LS_US_PER_MS_F ((double)LS_US_PER_MS)

void ls_stats_init(ls_stats_t *restrict stats) {
  *stats = (ls_stats_t){
      .latency_min_us = LS_STATS_NO_SAMPLE,
      .latency_max_us = LS_STATS_NO_SAMPLE,
      .started_at_ms = ls_now_ms(),
  };
}

void ls_stats_add_ok(ls_stats_t *restrict stats, double latency_ms) {
  if (LS_UNLIKELY(latency_ms < 0.0)) {
    latency_ms = 0.0;
  }
  stats->total = ls_add_sat(stats->total, 1);
  stats->ok = ls_add_sat(stats->ok, 1);
  uint64_t latency_us = (uint64_t)(latency_ms * LS_US_PER_MS_F);
  stats->latency_sum_us = ls_add_sat(stats->latency_sum_us, latency_us);
  if (latency_us < stats->latency_min_us) {
    stats->latency_min_us = latency_us;
  }
  /* The "no sample" sentinel is UINT64_MAX, which no sample can exceed, so the
   * first success has to clear it explicitly. */
  if (stats->latency_max_us == LS_STATS_NO_SAMPLE ||
      latency_us > stats->latency_max_us) {
    stats->latency_max_us = latency_us;
  }
}

void ls_stats_add_fail(ls_stats_t *restrict stats) {
  stats->total = ls_add_sat(stats->total, 1);
  stats->failed = ls_add_sat(stats->failed, 1);
}

double ls_stats_success_rate(const ls_stats_t *restrict stats) {
  if (stats->total == 0) {
    return 0.0;
  }
  return (double)stats->ok / (double)stats->total * 100.0;
}

double ls_stats_avg_latency_ms(const ls_stats_t *restrict stats) {
  if (stats->ok == 0) {
    return 0.0;
  }
  return (double)stats->latency_sum_us / (double)stats->ok / LS_US_PER_MS_F;
}

double ls_stats_min_latency_ms(const ls_stats_t *restrict stats) {
  if (stats->latency_min_us == LS_STATS_NO_SAMPLE) {
    return 0.0;
  }
  return (double)stats->latency_min_us / LS_US_PER_MS_F;
}

double ls_stats_max_latency_ms(const ls_stats_t *restrict stats) {
  if (stats->latency_max_us == LS_STATS_NO_SAMPLE) {
    return 0.0;
  }
  return (double)stats->latency_max_us / LS_US_PER_MS_F;
}

uint64_t ls_stats_uptime_sec(const ls_stats_t *restrict stats) {
  uint64_t now_ms = ls_now_ms();
  if (now_ms == UINT64_MAX || stats->started_at_ms == UINT64_MAX ||
      now_ms < stats->started_at_ms) {
    return 0;
  }
  return (now_ms - stats->started_at_ms) / LS_MS_PER_SEC;
}

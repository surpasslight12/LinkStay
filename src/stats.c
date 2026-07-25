#include "stats.h"

/* Microseconds per millisecond (as double for μs↔ms conversion). */
#define LS_US_PER_MS_F 1000.0

/* ---- Lifecycle ---- */

void ls_stats_init(ls_stats_t *stats) {
  if (stats == nullptr) {
    return;
  }
  *stats = (ls_stats_t){
      .latency_min_us = LS_STATS_NO_SAMPLE,
      .latency_max_us = LS_STATS_NO_SAMPLE,
      .started_at_ms = ls_now_ms(),
  };
}

/* ---- Accumulation ---- */

void ls_stats_add_ok(ls_stats_t *stats, double latency_ms) {
  if (LS_UNLIKELY(stats == nullptr)) {
    return;
  }
  /* Clamp negative values (clock-skew guard) to zero. */
  if (LS_UNLIKELY(latency_ms < 0.0)) {
    latency_ms = 0.0;
  }

  /* Saturating counter increment. */
  if (LS_LIKELY(stats->total < UINT64_MAX)) {
    stats->total++;
  }
  if (LS_LIKELY(stats->ok < UINT64_MAX)) {
    stats->ok++;
  }

  /* Convert to microseconds; guard against overflow of the conversion
   * itself (latency_ms would need to be > 1.8×10^13 ms ≈ 570 years). */
  uint64_t latency_us = (uint64_t)(latency_ms * LS_US_PER_MS_F);
  stats->latency_sum_us = ls_add_sat(stats->latency_sum_us, latency_us);

  if (latency_us < stats->latency_min_us) {
    stats->latency_min_us = latency_us;
  }
  if (latency_us > stats->latency_max_us) {
    stats->latency_max_us = latency_us;
  }
}

void ls_stats_add_fail(ls_stats_t *stats) {
  if (LS_UNLIKELY(stats == nullptr)) {
    return;
  }
  if (LS_LIKELY(stats->total < UINT64_MAX)) {
    stats->total++;
  }
  if (LS_LIKELY(stats->failed < UINT64_MAX)) {
    stats->failed++;
  }
}

/* ---- Query ---- */

double ls_stats_success_rate(const ls_stats_t *stats) {
  if (stats == nullptr || stats->total == 0) {
    return 0.0;
  }
  return (double)stats->ok / (double)stats->total * 100.0;
}

double ls_stats_avg_latency(const ls_stats_t *stats) {
  if (stats == nullptr || stats->ok == 0) {
    return 0.0;
  }
  return (double)stats->latency_sum_us / (double)stats->ok / LS_US_PER_MS_F;
}

double ls_stats_latency_min_ms(const ls_stats_t *stats) {
  if (stats == nullptr || stats->latency_min_us == LS_STATS_NO_SAMPLE) {
    return 0.0;
  }
  return (double)stats->latency_min_us / LS_US_PER_MS_F;
}

double ls_stats_latency_max_ms(const ls_stats_t *stats) {
  if (stats == nullptr || stats->latency_max_us == LS_STATS_NO_SAMPLE) {
    return 0.0;
  }
  return (double)stats->latency_max_us / LS_US_PER_MS_F;
}

uint64_t ls_stats_uptime_sec(const ls_stats_t *stats) {
  if (stats == nullptr) {
    return 0;
  }
  uint64_t now_ms = ls_now_ms();
  if (now_ms == UINT64_MAX || stats->started_at_ms == UINT64_MAX ||
      now_ms < stats->started_at_ms) {
    return 0;
  }
  return (now_ms - stats->started_at_ms) / LS_MS_PER_SEC;
}

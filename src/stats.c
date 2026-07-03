#include "stats.h"

void ls_stats_init(ls_stats_t *stats) {
  if (stats == nullptr) {
    return;
  }
  *stats = (ls_stats_t){
      .latency_min = -1.0,
      .latency_max = -1.0,
      .started_at_ms = ls_now_ms(),
  };
}

void ls_stats_add_ok(ls_stats_t *stats, double latency_ms) {
  if (LS_UNLIKELY(stats == nullptr)) {
    return;
  }
  if (LS_UNLIKELY(latency_ms < 0.0)) {
    latency_ms = 0.0;
  }
  stats->total++;
  stats->ok++;
  stats->latency_sum += latency_ms;
  if (LS_UNLIKELY(stats->latency_min < 0.0) ||
      latency_ms < stats->latency_min) {
    stats->latency_min = latency_ms;
  }
  if (LS_UNLIKELY(stats->latency_max < 0.0) ||
      latency_ms > stats->latency_max) {
    stats->latency_max = latency_ms;
  }
}

void ls_stats_add_fail(ls_stats_t *stats) {
  if (LS_UNLIKELY(stats == nullptr)) {
    return;
  }
  stats->total++;
  stats->failed++;
}

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
  return stats->latency_sum / (double)stats->ok;
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

#include "metrics.h"

void metrics_init(metrics_t *metrics) {
  if (metrics == NULL) {
    return;
  }
  metrics->total_pings = 0;
  metrics->successful_pings = 0;
  metrics->failed_pings = 0;
  metrics->total_latency = 0.0;
  metrics->min_latency = -1.0;
  metrics->max_latency = -1.0;
  metrics->start_time_ms = get_monotonic_ms();
}

void metrics_record_success(metrics_t *metrics, double latency_ms) {
  if (LINKSTAY_UNLIKELY(metrics == NULL)) {
    return;
  }
  if (LINKSTAY_UNLIKELY(latency_ms < 0.0)) {
    latency_ms = 0.0;
  }
  metrics->total_pings++;
  metrics->successful_pings++;
  metrics->total_latency += latency_ms;
  if (LINKSTAY_UNLIKELY(metrics->min_latency < 0.0) ||
      latency_ms < metrics->min_latency) {
    metrics->min_latency = latency_ms;
  }
  if (LINKSTAY_UNLIKELY(metrics->max_latency < 0.0) ||
      latency_ms > metrics->max_latency) {
    metrics->max_latency = latency_ms;
  }
}

void metrics_record_failure(metrics_t *metrics) {
  if (LINKSTAY_UNLIKELY(metrics == NULL)) {
    return;
  }
  metrics->total_pings++;
  metrics->failed_pings++;
}

double metrics_success_rate(const metrics_t *metrics) {
  if (metrics == NULL || metrics->total_pings == 0) {
    return 0.0;
  }
  return (double)metrics->successful_pings / (double)metrics->total_pings *
         100.0;
}

double metrics_avg_latency(const metrics_t *metrics) {
  if (metrics == NULL || metrics->successful_pings == 0) {
    return 0.0;
  }
  return metrics->total_latency / (double)metrics->successful_pings;
}

uint64_t metrics_uptime_seconds(const metrics_t *metrics) {
  if (metrics == NULL) {
    return 0;
  }
  uint64_t now_ms = get_monotonic_ms();
  if (now_ms == UINT64_MAX || metrics->start_time_ms == UINT64_MAX ||
      now_ms < metrics->start_time_ms) {
    return 0;
  }
  return (now_ms - metrics->start_time_ms) / LINKSTAY_MS_PER_SEC;
}

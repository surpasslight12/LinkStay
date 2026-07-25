#ifndef LINKSTAY_STATS_H
#define LINKSTAY_STATS_H

/*
 * stats.h — ping statistics value type.
 *
 * Aggregates counts, latency extremes (stored as integer microseconds to
 * avoid floating-point accumulation drift over very long runs), and uptime.
 * Pure value semantics: no I/O, no global state.
 */

#include "base.h"

/* Sentinel: no latency sample recorded yet. */
#define LS_STATS_NO_SAMPLE UINT64_MAX

typedef struct {
  uint64_t total;           /* total probes (saturated at UINT64_MAX) */
  uint64_t ok;              /* successful probes */
  uint64_t failed;          /* failed probes */
  uint64_t latency_sum_us;  /* accumulated latency in microseconds */
  uint64_t latency_min_us;  /* minimum latency in μs; LS_STATS_NO_SAMPLE = none */
  uint64_t latency_max_us;  /* maximum latency in μs; LS_STATS_NO_SAMPLE = none */
  uint64_t started_at_ms;   /* monotonic start time */
} ls_stats_t;

void ls_stats_init(ls_stats_t *stats);

/* latency_ms is the measured round-trip in fractional milliseconds.
 * It is converted to μs internally so the accumulator stays exact. */
void ls_stats_add_ok(ls_stats_t *stats, double latency_ms);
void ls_stats_add_fail(ls_stats_t *stats);

/* All latency accessors return fractional milliseconds. */
[[nodiscard]] double ls_stats_avg_latency(const ls_stats_t *stats);
[[nodiscard]] double ls_stats_latency_min_ms(const ls_stats_t *stats);
[[nodiscard]] double ls_stats_latency_max_ms(const ls_stats_t *stats);

[[nodiscard]] double ls_stats_success_rate(const ls_stats_t *stats); /* 0–100 */
[[nodiscard]] uint64_t ls_stats_uptime_sec(const ls_stats_t *stats);

#endif /* LINKSTAY_STATS_H */

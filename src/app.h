#ifndef LINKSTAY_APP_H
#define LINKSTAY_APP_H

/*
 * app.h — application assembly layer.
 *
 * Public surface: ls_app_init(), ls_app_run(), ls_app_destroy(). main.c owns
 * the ls_app_t storage and reads only its `log` member for the final exit
 * diagnostic; every other symbol in app.c is static. ls_stats_t is owned by
 * stats.h and appears here only because ls_app_t embeds it by value (the
 * design is heap-free, so the struct cannot be made opaque behind a pointer).
 *
 * Lifetime: ls_app_init() must succeed before ls_app_run(); ls_app_destroy()
 * is safe on a zero-initialized struct and after a failed init.
 */

#include "base.h"
#include "icmp.h"
#include "log.h"
#include "loop.h"
#include "notify.h"
#include "opts.h"
#include "stats.h"

/* ---- Application state ---- */

typedef struct {
  /* Resolved configuration (copied in by ls_app_init). */
  ls_opts_t opts;
  ls_log_t log;

  /* Runtime state. */
  ls_stats_t stats;
  ls_icmp_t icmp;
  ls_notify_t notify;

  struct sockaddr_storage dest;
  socklen_t dest_len;
  uint16_t identifier;

  ls_loop_t loop;
  ls_timer_t *ping_timer;
  ls_timer_t *reply_timer;
  ls_timer_t *watchdog_timer;

  uint64_t probe_sent_ns;
  uint16_t probe_sequence;
  int consecutive_fails;

  bool watchdog_last_failed;
  bool signal_stop;
} ls_app_t;

/* Resolves the target, opens the raw ICMP socket, and prepares statistics and
 * the notifier. On failure returns false with err set; ls_app_destroy() is
 * still safe to call. */
[[nodiscard]] bool ls_app_init(ls_app_t *restrict app,
                               const ls_opts_t *restrict opts,
                               ls_err_t *restrict err);

/* Registers the loop callbacks and runs until stopped. Returns the process
 * exit code (LS_EXIT_SUCCESS / LS_EXIT_FAILURE). */
[[nodiscard]] int ls_app_run(ls_app_t *restrict app);

/* Releases the ICMP socket and notifier socket, then zeroes the struct. */
void ls_app_destroy(ls_app_t *restrict app);

#endif /* LINKSTAY_APP_H */

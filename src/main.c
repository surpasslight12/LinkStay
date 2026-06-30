#include "config.h"
#include "logger.h"
#include "monitor.h"

static void main_log_early_error(bool enable_timestamp,
                                 const char *restrict error_msg) {
  logger_t logger;
  logger_init(&logger, LOG_LEVEL_ERROR, enable_timestamp);
  logger_error(&logger, "LinkStay failed: %s", error_msg);
}

int main(int argc, char **argv) {
  char error_msg[256];
  bool exit_requested = false;
  config_t config;

  if (!config_resolve(&config, argc, argv, &exit_requested, error_msg,
                      sizeof(error_msg))) {
    main_log_early_error(false, error_msg);
    return LINKSTAY_EXIT_FAILURE;
  }

  if (exit_requested) {
    return LINKSTAY_EXIT_SUCCESS;
  }

  linkstay_ctx_t ctx;
  if (!linkstay_ctx_init(&ctx, &config, error_msg, sizeof(error_msg))) {
    main_log_early_error(config_log_timestamps_enabled(&config), error_msg);
    return LINKSTAY_EXIT_FAILURE;
  }

  int rc = linkstay_reactor_run(&ctx);
  if (rc != LINKSTAY_EXIT_SUCCESS) {
    logger_error(&ctx.logger, "LinkStay exited with code %d", rc);
  }
  linkstay_ctx_destroy(&ctx);
  return rc;
}

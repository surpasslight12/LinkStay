#include "linkstay.h"
#include "monitor.h"

int main(int argc, char **argv) {
  char error_msg[256];
  bool exit_requested = false;
  config_t config;

  if (!config_resolve(&config, argc, argv, &exit_requested, error_msg,
                      sizeof(error_msg))) {
    logger_write(LOG_LEVEL_ERROR, false, "LinkStay failed: %s", error_msg);
    return 1;
  }

  if (exit_requested) {
    return 0;
  }

  linkstay_ctx_t ctx;
  if (!linkstay_ctx_init(&ctx, &config, error_msg, sizeof(error_msg))) {
    logger_write(LOG_LEVEL_ERROR, config_log_timestamps_enabled(&config),
                 "LinkStay failed: %s", error_msg);
    return 1;
  }

  int rc = linkstay_reactor_run(&ctx);
  if (rc != 0) {
    logger_error(&ctx.logger, "LinkStay exited with code %d", rc);
  }
  linkstay_ctx_destroy(&ctx);
  return rc;
}

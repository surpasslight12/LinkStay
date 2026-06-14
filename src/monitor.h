#ifndef LINKSTAY_MONITOR_H
#define LINKSTAY_MONITOR_H

#include "linkstay.h"

bool linkstay_ctx_init(linkstay_ctx_t *restrict ctx,
                      const config_t *restrict config,
                      char *restrict error_msg, size_t error_size);
void linkstay_ctx_destroy(linkstay_ctx_t *restrict ctx);
int linkstay_reactor_run(linkstay_ctx_t *restrict ctx);

#endif // LINKSTAY_MONITOR_H
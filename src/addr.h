#ifndef LINKSTAY_ADDR_H
#define LINKSTAY_ADDR_H

/*
 * addr.h — IPv4/IPv6 literal parsing.
 *
 * Single owner of the "target must be an IP literal; DNS is intentionally
 * unsupported" rule: opts.c asks ls_addr_is_literal() while validating the
 * option, and app.c asks ls_addr_parse() for the socket address it hands to
 * the ICMP transport. Neither caller re-implements the check.
 */

#include "base.h"

#include <sys/socket.h>

/* True when text parses as an IPv4 or IPv6 literal. NULL, empty strings, and
 * hostnames are all false. Use ls_addr_parse() when the reason matters. */
[[nodiscard]] bool ls_addr_is_literal(const char *restrict text);

/* Parses text into addr/addr_len (family and length set on success). On
 * failure returns false with err set, including a hint when the text looks
 * like a mangled literal. */
[[nodiscard]] bool ls_addr_parse(const char *restrict text,
                                 struct sockaddr_storage *restrict addr,
                                 socklen_t *restrict addr_len,
                                 ls_err_t *restrict err);

#endif /* LINKSTAY_ADDR_H */

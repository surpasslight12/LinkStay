/*
 * addr.c — IPv4/IPv6 literal parsing shared by configuration validation and
 * transport. No sockets are opened here; only inet_pton() is used.
 */

#include "addr.h"

#include <arpa/inet.h>
#include <string.h>

bool ls_addr_parse(const char *restrict text,
                   struct sockaddr_storage *restrict addr,
                   socklen_t *restrict addr_len, ls_err_t *restrict err) {
  if (text == nullptr || addr == nullptr || addr_len == nullptr) {
    return ls_err_set(err, "Invalid address parse arguments");
  }

  memset(addr, 0, sizeof(*addr));

  struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
  if (inet_pton(AF_INET, text, &addr4->sin_addr) == 1) {
    addr4->sin_family = AF_INET;
    *addr_len = sizeof(*addr4);
    return true;
  }

  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
  if (inet_pton(AF_INET6, text, &addr6->sin6_addr) == 1) {
    addr6->sin6_family = AF_INET6;
    *addr_len = sizeof(*addr6);
    return true;
  }

  const char *hint = "";
  if (strchr(text, ':') != nullptr) {
    hint = " (looks like an IPv6 literal but parsing failed)";
  } else if (strchr(text, '.') != nullptr) {
    hint = " (looks like an IPv4 literal but parsing failed)";
  }
  return ls_err_set(err, "Invalid IPv4/IPv6 address (DNS disabled): %s%s",
                    text, hint);
}

bool ls_addr_is_literal(const char *restrict text) {
  struct sockaddr_storage scratch;
  socklen_t scratch_len = 0;
  return ls_addr_parse(text, &scratch, &scratch_len, nullptr);
}

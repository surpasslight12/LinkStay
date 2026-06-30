#ifndef LINKSTAY_ICMP_H
#define LINKSTAY_ICMP_H

/*
 * icmp.h — raw-socket ICMP transport.
 *
 * Owns echo request construction, reply matching, and the optional kernel
 * BPF filter. Follows a zero-allocation model: send/receive buffers live
 * inside icmp_pinger_t and are reused across the hot path.
 */

#include "common.h"

#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>

#define LINKSTAY_ICMP_SEND_BUFFER_SIZE 256U

typedef struct {
  bool success;
  double latency_ms;
  char error_msg[256];
  uint16_t sequence; /* ICMP echo sequence number of the matched reply */
} ping_result_t;

typedef enum {
  ICMP_RECEIVE_NO_MORE = -1,
  ICMP_RECEIVE_IGNORED = 0,
  ICMP_RECEIVE_MATCHED = 1,
  ICMP_RECEIVE_ERROR = 2,
} icmp_receive_status_t;

typedef struct {
  int sockfd;
  int family;
  uint16_t sequence;

  /* Send buffer (stack-allocated, zero-alloc model).
   * Aligned to 4 so casts to struct icmphdr / icmp6_hdr are well-defined. */
  uint8_t send_buf[LINKSTAY_ICMP_SEND_BUFFER_SIZE] LINKSTAY_ALIGNED(4);

  /* Receive buffer reused across replies (zero-alloc model, mirrors send_buf).
   * 1500 bytes covers the largest standard Ethernet-MTU ICMP reply; aligned to
   * 16 so IP/ICMP header accesses are naturally aligned. Reusing it keeps the
   * hot receive path off a large per-call stack frame. */
  uint8_t recv_buf[1500] LINKSTAY_ALIGNED(16);
} icmp_pinger_t;

typedef char linkstay_assert_icmphdr_at_least_8_bytes[
  (sizeof(struct icmphdr) >= 8) ? 1 : -1];

bool icmp_pinger_init(icmp_pinger_t *restrict pinger, int family,
            char *restrict error_msg, size_t error_size);
void icmp_pinger_destroy(icmp_pinger_t *restrict pinger);
bool icmp_pinger_send_echo(
    icmp_pinger_t *restrict pinger,
    const struct sockaddr_storage *restrict dest_addr, socklen_t dest_addr_len,
    uint16_t identifier, size_t packet_len, char *restrict error_msg,
    size_t error_size);
icmp_receive_status_t icmp_pinger_receive_reply(
    icmp_pinger_t *restrict pinger,
    const struct sockaddr_storage *restrict dest_addr, uint16_t identifier,
    uint16_t expected_sequence, uint64_t send_time_ms, uint64_t now_ms,
    ping_result_t *restrict out_result);
bool icmp_resolve_target(
  const char *restrict target, struct sockaddr_storage *restrict addr,
  socklen_t *restrict addr_len, char *restrict error_msg, size_t error_size);

#endif /* LINKSTAY_ICMP_H */

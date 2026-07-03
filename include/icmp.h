#ifndef LINKSTAY_ICMP_H
#define LINKSTAY_ICMP_H

/*
 * icmp.h — raw-socket ICMP echo transport.
 *
 * Owns echo request construction, reply matching, and the optional kernel
 * BPF filter. Zero-allocation model: send/receive buffers live inside
 * ls_icmp_t and are reused across the hot path.
 */

#include "base.h"

#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>

#define LS_ICMP_SEND_BUFFER_SIZE 256U
#define LS_ICMP_RECV_BUFFER_SIZE 1500U

typedef struct {
  double latency_ms;
  uint16_t sequence; /* echo sequence of the matched reply */
} ls_icmp_reply_t;

typedef enum {
  LS_ICMP_RECV_NO_MORE = -1, /* socket drained (EAGAIN) */
  LS_ICMP_RECV_IGNORED = 0,  /* packet received but not our reply */
  LS_ICMP_RECV_MATCHED = 1,  /* matching echo reply */
  LS_ICMP_RECV_ERROR = 2,    /* recvfrom failed */
} ls_icmp_recv_status_t;

typedef struct {
  int sockfd;
  int family; /* AF_INET or AF_INET6 */
  uint16_t sequence;
  bool bpf_attached;
  int bpf_errno; /* non-fatal: filter attach failure reason */

  /* Reused packet buffers (zero-alloc model). Send buffer aligned to 4 so
   * casts to struct icmphdr / icmp6_hdr are well-defined; receive buffer
   * sized for a full Ethernet-MTU reply and aligned to 16 for natural
   * IP/ICMP header access. */
  alignas(4) uint8_t send_buf[LS_ICMP_SEND_BUFFER_SIZE];
  alignas(16) uint8_t recv_buf[LS_ICMP_RECV_BUFFER_SIZE];
} ls_icmp_t;

static_assert(sizeof(struct icmphdr) >= 8,
              "ICMP header must be at least 8 bytes");

/* Parses an IPv4/IPv6 literal into a socket address. DNS is rejected. */
[[nodiscard]] bool ls_icmp_resolve(const char *restrict target,
                                   struct sockaddr_storage *restrict addr,
                                   socklen_t *restrict addr_len,
                                   ls_err_t *restrict err);

/* Opens the raw socket (requires root or CAP_NET_RAW) and attempts to
 * attach the family-specific BPF filter (non-fatal on failure). */
[[nodiscard]] bool ls_icmp_open(ls_icmp_t *restrict icmp, int family,
                                ls_err_t *restrict err);
void ls_icmp_close(ls_icmp_t *restrict icmp);

/* Sends one echo request; advances icmp->sequence (1..65535, never 0). */
[[nodiscard]] bool ls_icmp_send_echo(
    ls_icmp_t *restrict icmp, const struct sockaddr_storage *restrict dest,
    socklen_t dest_len, uint16_t identifier, size_t packet_len,
    ls_err_t *restrict err);

/* Receives one packet and matches it against the expected reply. */
ls_icmp_recv_status_t ls_icmp_recv(
    ls_icmp_t *restrict icmp, const struct sockaddr_storage *restrict dest,
    uint16_t identifier, uint16_t expected_sequence, uint64_t send_time_ms,
    uint64_t now_ms, ls_icmp_reply_t *restrict out_reply,
    ls_err_t *restrict err);

#endif /* LINKSTAY_ICMP_H */

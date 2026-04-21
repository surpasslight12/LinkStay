#include "linkstay.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>

#include <netinet/ip.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ICMP checksum — RFC 1071 one's-complement sum.  Internal to icmp.c.
 * Reads 16-bit words in native byte order (matches how the kernel verifies),
 * stores the result as a native uint16_t — no htons() needed. */
static uint16_t calculate_checksum(const void *data, size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t sum = 0;

  for (size_t i = 0; i + 1 < len; i += 2) {
    uint16_t word;
    memcpy(&word, bytes + i, sizeof(word));
    sum += word;
  }

  if (len % 2) {
    sum += bytes[len - 1];
  }

  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  return (uint16_t)(~sum);
}

static bool
icmp_validate_send_args(const icmp_pinger_t *restrict pinger,
                        const struct sockaddr_storage *restrict dest_addr,
                        char *restrict error_msg, size_t error_size) {
  if (pinger == NULL || dest_addr == NULL || error_msg == NULL ||
      error_size == 0) {
    return false;
  }

  if (pinger->sockfd < 0) {
    snprintf(error_msg, error_size, "ICMP socket is not initialized");
    return false;
  }

  if (dest_addr->ss_family != AF_INET && dest_addr->ss_family != AF_INET6) {
    snprintf(error_msg, error_size, "Unsupported address family: %d",
             dest_addr->ss_family);
    return false;
  }

  return true;
}

static bool
icmp_receive_source_matches(const struct sockaddr_storage *restrict dest_addr,
                            const struct sockaddr_storage *restrict recv_addr) {
  if (dest_addr == NULL || recv_addr == NULL ||
      recv_addr->ss_family != dest_addr->ss_family) {
    return false;
  }

  if (dest_addr->ss_family == AF_INET) {
    const struct sockaddr_in *dest4 = (const struct sockaddr_in *)dest_addr;
    const struct sockaddr_in *recv4 = (const struct sockaddr_in *)recv_addr;
    return dest4->sin_addr.s_addr == recv4->sin_addr.s_addr;
  }

  const struct sockaddr_in6 *dest6 = (const struct sockaddr_in6 *)dest_addr;
  const struct sockaddr_in6 *recv6 = (const struct sockaddr_in6 *)recv_addr;
  return memcmp(&dest6->sin6_addr, &recv6->sin6_addr,
                sizeof(dest6->sin6_addr)) == 0;
}

static icmp_receive_status_t parse_ipv4_reply(const uint8_t *restrict recv_buf,
                                              size_t received,
                                              uint16_t identifier,
                                              uint16_t expected_sequence) {
  if (recv_buf == NULL || received < sizeof(struct ip)) {
    return ICMP_RECEIVE_IGNORED;
  }

  const struct ip *ip_hdr = (const struct ip *)recv_buf;
  if (ip_hdr->ip_p != IPPROTO_ICMP) {
    return ICMP_RECEIVE_IGNORED;
  }

  size_t ip_hdr_len = (size_t)ip_hdr->ip_hl * 4;
  if (ip_hdr_len < sizeof(struct ip) || ip_hdr_len > received ||
      ip_hdr_len + sizeof(struct icmphdr) > received) {
    return ICMP_RECEIVE_IGNORED;
  }

  const struct icmphdr *icmp_hdr =
      (const struct icmphdr *)(recv_buf + ip_hdr_len);
  if (icmp_hdr->type != ICMP_ECHOREPLY) {
    return ICMP_RECEIVE_IGNORED;
  }
  if (ntohs(icmp_hdr->un.echo.id) != identifier) {
    return ICMP_RECEIVE_IGNORED;
  }
  if (ntohs(icmp_hdr->un.echo.sequence) != expected_sequence) {
    return ICMP_RECEIVE_IGNORED;
  }

  return ICMP_RECEIVE_MATCHED;
}

static icmp_receive_status_t parse_ipv6_reply(const uint8_t *restrict recv_buf,
                                              size_t received,
                                              uint16_t identifier,
                                              uint16_t expected_sequence) {
  if (recv_buf == NULL || received < sizeof(struct icmp6_hdr)) {
    return ICMP_RECEIVE_IGNORED;
  }

  const struct icmp6_hdr *icmp6_hdr = (const struct icmp6_hdr *)recv_buf;
  if (icmp6_hdr->icmp6_type != ICMP6_ECHO_REPLY) {
    return ICMP_RECEIVE_IGNORED;
  }
  if (ntohs(icmp6_hdr->icmp6_id) != identifier) {
    return ICMP_RECEIVE_IGNORED;
  }
  if (ntohs(icmp6_hdr->icmp6_seq) != expected_sequence) {
    return ICMP_RECEIVE_IGNORED;
  }

  return ICMP_RECEIVE_MATCHED;
}

/* Non-fatal: BPF filter improves performance but is not required. */
static void icmp_attach_bpf_filter(int sockfd, struct sock_filter *filter,
                                   size_t filter_count) {
  struct sock_fprog fprog = {.len = (unsigned short)filter_count,
                             .filter = filter};
  (void)setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog,
                   sizeof(fprog));
}

bool icmp_pinger_init(icmp_pinger_t *restrict pinger, int family,
                      char *restrict error_msg, size_t error_size) {
  if (pinger == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  pinger->sockfd = -1;
  pinger->family = family;
  pinger->sequence = 0;

  int proto = (family == AF_INET6) ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

  /* Create raw socket, CLOEXEC for security, NONBLOCK for poll multiplexing */
  pinger->sockfd =
      socket(family, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, proto);
  if (pinger->sockfd < 0) {
    snprintf(
        error_msg, error_size,
        "Failed to create socket (family=%d): %s (require root or CAP_NET_RAW)",
        family, strerror(errno));
    return false;
  }

  /* Attach kernel BPF filter to drop irrelevant ICMP packets. */
  if (family == AF_INET) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS,
                 0), /* A = IP Header Length (ip[0]) */
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0f), /* A = A & 0x0f */
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, 4),    /* A = A * 4    */
        BPF_STMT(BPF_MISC | BPF_TAX, 0), /* X = A                        */
        BPF_STMT(BPF_LD | BPF_B | BPF_IND, 0), /* A = ip[X] (ICMP Type) */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP_ECHOREPLY, 0,
                 1),                       /* if A == ECHOREPLY, pass  */
        BPF_STMT(BPF_RET | BPF_K, 0xffff), /* Accept (keep full packet)    */
        BPF_STMT(BPF_RET | BPF_K, 0)       /* Reject (drop packet)         */
    };
    icmp_attach_bpf_filter(pinger->sockfd, filter,
                           LINKSTAY_ARRAY_LEN(filter));
  } else if (family == AF_INET6) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0), /* A = icmp6[0] (ICMPv6 Type) */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP6_ECHO_REPLY, 0,
                 1),                       /* if A == ECHO_REPLY, pass*/
        BPF_STMT(BPF_RET | BPF_K, 0xffff), /* Accept (keep full packet)    */
        BPF_STMT(BPF_RET | BPF_K, 0)       /* Reject (drop packet)         */
    };
    icmp_attach_bpf_filter(pinger->sockfd, filter,
                           LINKSTAY_ARRAY_LEN(filter));
  }

  return true;
}

void icmp_pinger_destroy(icmp_pinger_t *restrict pinger) {
  if (pinger == NULL) {
    return;
  }

  if (pinger->sockfd >= 0) {
    close(pinger->sockfd);
    pinger->sockfd = -1;
  }
}

/* Sequence 0 is reserved as the "not waiting" sentinel in monitor state. */
static uint16_t next_sequence(icmp_pinger_t *restrict pinger) {
  if (pinger == NULL) {
    return 1;
  }

  if (pinger->sequence == UINT16_MAX) {
    pinger->sequence = 1;
  } else {
    pinger->sequence = (uint16_t)(pinger->sequence + 1);
  }

  return pinger->sequence;
}

bool icmp_pinger_send_echo(icmp_pinger_t *restrict pinger,
                           const struct sockaddr_storage *restrict dest_addr,
                           socklen_t dest_addr_len, uint16_t identifier,
                           size_t packet_len, char *restrict error_msg,
                           size_t error_size) {
  if (!icmp_validate_send_args(pinger, dest_addr, error_msg, error_size)) {
    return false;
  }

  if (packet_len == 0 || packet_len > sizeof(pinger->send_buf)) {
    snprintf(error_msg, error_size, "Invalid ICMP packet size: %zu",
             packet_len);
    return false;
  }

  next_sequence(pinger);

  if (pinger->family == AF_INET6) {
    struct icmp6_hdr *icmp6_hdr = (struct icmp6_hdr *)pinger->send_buf;
    memset(icmp6_hdr, 0, sizeof(*icmp6_hdr));
    icmp6_hdr->icmp6_type = ICMP6_ECHO_REQUEST;
    icmp6_hdr->icmp6_code = 0;
    icmp6_hdr->icmp6_id = htons(identifier);
    icmp6_hdr->icmp6_seq = htons(pinger->sequence);
  } else {
    struct icmphdr *icmp_hdr = (struct icmphdr *)pinger->send_buf;
    memset(icmp_hdr, 0, sizeof(*icmp_hdr));
    icmp_hdr->type = ICMP_ECHO;
    icmp_hdr->code = 0;
    icmp_hdr->un.echo.id = htons(identifier);
    icmp_hdr->un.echo.sequence = htons(pinger->sequence);
    icmp_hdr->checksum = calculate_checksum(pinger->send_buf, packet_len);
  }

  ssize_t sent =
      sendto(pinger->sockfd, pinger->send_buf, packet_len, MSG_NOSIGNAL,
             (const struct sockaddr *)dest_addr, dest_addr_len);
  if (sent < 0) {
    snprintf(error_msg, error_size, "Failed to send packet: %s",
             strerror(errno));
    return false;
  }
  if ((size_t)sent != packet_len) {
    snprintf(error_msg, error_size, "Short ICMP send: %zd", sent);
    return false;
  }

  return true;
}

icmp_receive_status_t
icmp_pinger_receive_reply(const icmp_pinger_t *restrict pinger,
                          const struct sockaddr_storage *restrict dest_addr,
                          uint16_t identifier, uint16_t expected_sequence,
                          uint64_t send_time_ms, uint64_t now_ms,
                          ping_result_t *restrict out_result) {
  if (pinger == NULL || dest_addr == NULL || out_result == NULL) {
    return ICMP_RECEIVE_ERROR;
  }

  /* 1500 bytes covers the largest standard Ethernet-MTU ICMP reply we expect.
   * Align to 16 so IP/ICMP header accesses are naturally aligned. */
  uint8_t recv_buf[1500] __attribute__((aligned(16)));
  struct sockaddr_storage recv_addr;
  socklen_t recv_addr_len = sizeof(recv_addr);

  ssize_t received = recvfrom(pinger->sockfd, recv_buf, sizeof(recv_buf), 0,
                              (struct sockaddr *)&recv_addr, &recv_addr_len);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return ICMP_RECEIVE_NO_MORE;
    }

    out_result->success = false;
    out_result->latency_ms = -1.0;
    snprintf(out_result->error_msg, sizeof(out_result->error_msg),
             "recvfrom failed: %s", strerror(errno));
    return ICMP_RECEIVE_ERROR;
  }

  if (received == 0 || !icmp_receive_source_matches(dest_addr, &recv_addr)) {
    return ICMP_RECEIVE_IGNORED;
  }

  icmp_receive_status_t status = ICMP_RECEIVE_IGNORED;
  if (dest_addr->ss_family == AF_INET) {
    status = parse_ipv4_reply(recv_buf, (size_t)received, identifier,
                              expected_sequence);
  } else if (dest_addr->ss_family == AF_INET6) {
    status = parse_ipv6_reply(recv_buf, (size_t)received, identifier,
                              expected_sequence);
  }

  if (status == ICMP_RECEIVE_MATCHED) {
    out_result->success = true;
    /* Guard against impossible clock skew before recording latency. */
    out_result->latency_ms =
        (now_ms >= send_time_ms) ? (double)(now_ms - send_time_ms) : 0.0;
    out_result->error_msg[0] = '\0';
  }

  return status;
}

bool resolve_target(const char *restrict target,
                    struct sockaddr_storage *restrict addr,
                    socklen_t *restrict addr_len, char *restrict error_msg,
                    size_t error_size) {
  if (target == NULL || addr == NULL || addr_len == NULL || error_msg == NULL ||
      error_size == 0) {
    return false;
  }

  memset(addr, 0, sizeof(*addr));

  struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
  if (inet_pton(AF_INET, target, &addr4->sin_addr) == 1) {
    addr4->sin_family = AF_INET;
    *addr_len = sizeof(*addr4);
    return true;
  }

  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
  if (inet_pton(AF_INET6, target, &addr6->sin6_addr) == 1) {
    addr6->sin6_family = AF_INET6;
    *addr_len = sizeof(*addr6);
    return true;
  }

  const char *hint = "";
  if (strchr(target, ':') != NULL) {
    hint = " (looks like an IPv6 literal but parsing failed)";
  } else if (strchr(target, '.') != NULL) {
    hint = " (looks like an IPv4 literal but parsing failed)";
  }

  snprintf(error_msg, error_size,
           "Invalid IPv4/IPv6 address (DNS disabled): %s%s", target, hint);
  return false;
}

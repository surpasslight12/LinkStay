#include "icmp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <netinet/ip.h>
#include <string.h>
#include <unistd.h>

/* ---- Target resolution ---- */

bool ls_icmp_resolve(const char *restrict target,
                     struct sockaddr_storage *restrict addr,
                     socklen_t *restrict addr_len, ls_err_t *restrict err) {
  if (target == nullptr || addr == nullptr || addr_len == nullptr) {
    return ls_err_set(err, "Invalid ICMP resolve arguments");
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
  if (strchr(target, ':') != nullptr) {
    hint = " (looks like an IPv6 literal but parsing failed)";
  } else if (strchr(target, '.') != nullptr) {
    hint = " (looks like an IPv4 literal but parsing failed)";
  }
  return ls_err_set(err, "Invalid IPv4/IPv6 address (DNS disabled): %s%s",
                    target, hint);
}

/* ---- Socket setup ---- */

/* Non-fatal: the BPF filter improves performance but is not required. */
static int attach_bpf_filter(int sockfd, struct sock_filter *filter,
                             size_t filter_count) {
  struct sock_fprog fprog = {.len = (unsigned short)filter_count,
                             .filter = filter};
  if (setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog,
                 sizeof(fprog)) == 0) {
    return 0;
  }
  return errno;
}

static int attach_family_filter(int sockfd, int family) {
  if (family == AF_INET) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0), /* A = ip[0] (ver+IHL)     */
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0f), /* A &= 0x0f (IHL)     */
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, 4),    /* A *= 4 (hdr bytes)  */
        BPF_STMT(BPF_MISC | BPF_TAX, 0),           /* X = A               */
        BPF_STMT(BPF_LD | BPF_B | BPF_IND, 0),     /* A = ip[X] (type)    */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP_ECHOREPLY, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0xffff), /* accept full packet          */
        BPF_STMT(BPF_RET | BPF_K, 0),      /* drop                        */
    };
    return attach_bpf_filter(sockfd, filter, LS_ARRAY_LEN(filter));
  }
  struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0), /* A = icmp6[0] (type) */
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP6_ECHO_REPLY, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, 0xffff), /* accept full packet */
      BPF_STMT(BPF_RET | BPF_K, 0),      /* drop               */
  };
  return attach_bpf_filter(sockfd, filter, LS_ARRAY_LEN(filter));
}

bool ls_icmp_open(ls_icmp_t *restrict icmp, int family,
                  ls_err_t *restrict err) {
  if (icmp == nullptr || (family != AF_INET && family != AF_INET6)) {
    return ls_err_set(err, "Invalid ICMP socket request");
  }

  *icmp = (ls_icmp_t){.sockfd = -1, .family = family};

  int proto = (family == AF_INET6) ? IPPROTO_ICMPV6 : IPPROTO_ICMP;
  /* CLOEXEC for hygiene across the shutdown spawn, NONBLOCK for poll(2). */
  icmp->sockfd = socket(family, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                        proto);
  if (icmp->sockfd < 0) {
    return ls_err_set(err,
                      "Failed to create socket (family=%d): %s (require root "
                      "or CAP_NET_RAW)",
                      family, strerror(errno));
  }

  icmp->bpf_errno = attach_family_filter(icmp->sockfd, family);
  icmp->bpf_attached = icmp->bpf_errno == 0;
  return true;
}

void ls_icmp_close(ls_icmp_t *restrict icmp) {
  if (icmp == nullptr) {
    return;
  }
  if (icmp->sockfd >= 0) {
    close(icmp->sockfd);
    icmp->sockfd = -1;
  }
}

/* ---- Echo request construction ---- */

/* RFC 1071 one's-complement sum. Reads 16-bit words in native byte order
 * (matches how the kernel verifies); no htons() needed on the result. */
static uint16_t icmp_checksum(const void *data, size_t len) {
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

static void fill_payload(uint8_t *packet, size_t packet_len,
                         size_t header_len) {
  uint8_t *payload = packet + header_len;
  size_t payload_len = packet_len - header_len;
  for (size_t i = 0; i != payload_len; i++) {
    payload[i] = (uint8_t)(i & 0xFFU);
  }
}

/* Sequence is never 0: app.c clears its expected sequence to 0 while idle,
 * so a cleared probe can never match a live reply. */
static void next_sequence(ls_icmp_t *icmp) {
  icmp->sequence = (icmp->sequence == UINT16_MAX)
                       ? 1
                       : (uint16_t)(icmp->sequence + 1);
}

bool ls_icmp_send_echo(ls_icmp_t *restrict icmp,
                       const struct sockaddr_storage *restrict dest,
                       socklen_t dest_len, uint16_t identifier,
                       size_t packet_len, ls_err_t *restrict err) {
  if (icmp == nullptr || dest == nullptr || icmp->sockfd < 0) {
    return ls_err_set(err, "ICMP socket is not initialized");
  }
  if (dest->ss_family != AF_INET && dest->ss_family != AF_INET6) {
    return ls_err_set(err, "Unsupported address family: %d", dest->ss_family);
  }
  if (dest->ss_family != icmp->family) {
    return ls_err_set(
        err, "Address family mismatch: dest=%d, socket=%d",
        dest->ss_family, icmp->family);
  }
  /* Lower bound covers the echo header (8 bytes for both families) so the
   * payload length below can never underflow. */
  size_t header_len = (icmp->family == AF_INET6) ? sizeof(struct icmp6_hdr)
                                                 : sizeof(struct icmphdr);
  if (packet_len < header_len || packet_len > sizeof(icmp->send_buf)) {
    return ls_err_set(err, "Invalid ICMP packet size: %zu", packet_len);
  }

  next_sequence(icmp);

  if (icmp->family == AF_INET6) {
    struct icmp6_hdr *hdr = (struct icmp6_hdr *)icmp->send_buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->icmp6_type = ICMP6_ECHO_REQUEST;
    hdr->icmp6_code = 0;
    hdr->icmp6_id = htons(identifier);
    hdr->icmp6_seq = htons(icmp->sequence);
    fill_payload(icmp->send_buf, packet_len, sizeof(*hdr));
    /* Checksum stays 0: the kernel computes it for IPPROTO_ICMPV6. */
  } else {
    struct icmphdr *hdr = (struct icmphdr *)icmp->send_buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = ICMP_ECHO;
    hdr->code = 0;
    hdr->un.echo.id = htons(identifier);
    hdr->un.echo.sequence = htons(icmp->sequence);
    fill_payload(icmp->send_buf, packet_len, sizeof(*hdr));
    hdr->checksum = icmp_checksum(icmp->send_buf, packet_len);
  }

  ssize_t sent = sendto(icmp->sockfd, icmp->send_buf, packet_len, MSG_NOSIGNAL,
                        (const struct sockaddr *)dest, dest_len);
  if (sent < 0) {
    return ls_err_set(err, "Failed to send packet: %s", strerror(errno));
  }
  if ((size_t)sent != packet_len) {
    return ls_err_set(err, "Short ICMP send: %zd", sent);
  }
  return true;
}

/* ---- Reply matching ---- */

static bool source_matches(const struct sockaddr_storage *dest,
                           const struct sockaddr_storage *from) {
  if (from->ss_family != dest->ss_family) {
    return false;
  }
  if (dest->ss_family == AF_INET) {
    const struct sockaddr_in *dest4 = (const struct sockaddr_in *)dest;
    const struct sockaddr_in *from4 = (const struct sockaddr_in *)from;
    return dest4->sin_addr.s_addr == from4->sin_addr.s_addr;
  }
  const struct sockaddr_in6 *dest6 = (const struct sockaddr_in6 *)dest;
  const struct sockaddr_in6 *from6 = (const struct sockaddr_in6 *)from;
  return memcmp(&dest6->sin6_addr, &from6->sin6_addr,
                sizeof(dest6->sin6_addr)) == 0;
}

static bool parse_ipv4_reply(const uint8_t *buf, size_t received,
                             uint16_t identifier, uint16_t expected_sequence) {
  const struct ip *ip_hdr = (const struct ip *)buf;
  if (received < sizeof(*ip_hdr)) {
    return false;
  }
  if (ip_hdr->ip_p != IPPROTO_ICMP) {
    return false;
  }
  /* ip_hl is the IPv4 header length in 32-bit words — multiply by 4 for
   * bytes. This correctly handles IPv4 options that extend the header
   * beyond the base sizeof(*ip_hdr). */
  size_t ip_hdr_len = (size_t)ip_hdr->ip_hl * 4;
  if (ip_hdr_len < sizeof(*ip_hdr) || ip_hdr_len > received ||
      ip_hdr_len + sizeof(struct icmphdr) > received) {
    return false;
  }
  const struct icmphdr *hdr = (const struct icmphdr *)(buf + ip_hdr_len);
  return hdr->type == ICMP_ECHOREPLY &&
         ntohs(hdr->un.echo.id) == identifier &&
         ntohs(hdr->un.echo.sequence) == expected_sequence;
}

static bool parse_ipv6_reply(const uint8_t *buf, size_t received,
                             uint16_t identifier, uint16_t expected_sequence) {
  /* No IPv6 header to skip: on Linux, an IPPROTO_ICMPV6 raw socket delivers
   * only the ICMPv6 payload (the kernel strips the IPv6 header), whereas an
   * IPPROTO_ICMP raw socket includes the IPv4 header. This asymmetry is
   * standard raw-socket behavior, not an oversight. */
  const struct icmp6_hdr *hdr = (const struct icmp6_hdr *)buf;
  if (received < sizeof(*hdr)) {
    return false;
  }
  return hdr->icmp6_type == ICMP6_ECHO_REPLY &&
         ntohs(hdr->icmp6_id) == identifier &&
         ntohs(hdr->icmp6_seq) == expected_sequence;
}

ls_icmp_recv_status_t ls_icmp_recv(
    ls_icmp_t *restrict icmp, const struct sockaddr_storage *restrict dest,
    uint16_t identifier, uint16_t expected_sequence, ls_err_t *restrict err) {
  if (icmp == nullptr || dest == nullptr) {
    (void)ls_err_set(err, "Invalid ICMP receive arguments");
    return LS_ICMP_RECV_ERROR;
  }

  struct sockaddr_storage from;
  socklen_t from_len = sizeof(from);
  ssize_t received = recvfrom(icmp->sockfd, icmp->recv_buf,
                              sizeof(icmp->recv_buf), 0,
                              (struct sockaddr *)&from, &from_len);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return LS_ICMP_RECV_NO_MORE;
    }
    (void)ls_err_set(err, "recvfrom failed: %s", strerror(errno));
    return LS_ICMP_RECV_ERROR;
  }

  if (received == 0 || !source_matches(dest, &from)) {
    return LS_ICMP_RECV_IGNORED;
  }

  bool matched = (dest->ss_family == AF_INET)
                     ? parse_ipv4_reply(icmp->recv_buf, (size_t)received,
                                        identifier, expected_sequence)
                     : parse_ipv6_reply(icmp->recv_buf, (size_t)received,
                                        identifier, expected_sequence);
  return matched ? LS_ICMP_RECV_MATCHED : LS_ICMP_RECV_IGNORED;
}

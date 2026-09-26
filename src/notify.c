/*
 * notify.c — sd_notify channel setup and message sending. No policy: the
 * caller decides when READY/STATUS/WATCHDOG/STOPPING are appropriate.
 */

#include "notify.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LS_NOTIFY_MESSAGE_SIZE 256U
#define LS_NOTIFY_DEDUP_WINDOW_MS UINT64_C(2000)

static bool parse_uint64(const char *value, uint64_t *out_value) {
  errno = 0;
  char *endptr = nullptr;
  unsigned long long parsed = strtoull(value, &endptr, 10);
  if (errno != 0 || endptr == value || *endptr != '\0') {
    return false;
  }
  uint64_t converted = (uint64_t)parsed;
  if (converted != parsed) {
    return false;
  }
  *out_value = converted;
  return true;
}

/* systemd only expects watchdog pings from the process named in WATCHDOG_PID;
 * an unset or empty value means "this process". */
static bool watchdog_pid_matches_self(void) {
  const char *pid_str = getenv("WATCHDOG_PID");
  if (pid_str == nullptr || pid_str[0] == '\0') {
    return true;
  }
  errno = 0;
  char *endptr = nullptr;
  long parsed = strtol(pid_str, &endptr, 10);
  if (errno != 0 || endptr == pid_str || *endptr != '\0' || parsed <= 0) {
    return false;
  }
  return (pid_t)parsed == getpid();
}

/* Accepts both pathname and Linux abstract ("@name") sockets. */
static bool build_socket_addr(const char *path, struct sockaddr_un *addr,
                              socklen_t *addr_len) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;

  if (path[0] == '@') {
    size_t name_len = strlen(path + 1);
    if (name_len == 0 || name_len > sizeof(addr->sun_path) - 1) {
      return false;
    }
    addr->sun_path[0] = '\0';
    memcpy(addr->sun_path + 1, path + 1, name_len);
    *addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    return true;
  }

  size_t path_len = strlen(path);
  if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
    return false;
  }
  memcpy(addr->sun_path, path, path_len + 1);
  *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len +
                          1);
  return true;
}

static bool notify_send(ls_notify_t *notify, const char *message) {
  if (LS_UNLIKELY(!notify->enabled)) {
    return false;
  }

  ssize_t sent;
  do {
    sent = send(notify->sockfd, message, strlen(message), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  return sent >= 0;
}

/* STATUS= updates are deduplicated: an unchanged status within the window is
 * treated as already delivered. */
static bool notify_status(ls_notify_t *notify, const char *status) {
  if (!notify->enabled) {
    return false;
  }

  uint64_t now_ms = ls_now_ms();
  bool same = strcmp(notify->last_status, status) == 0;
  if (same && notify->last_status_ms != 0 && now_ms != UINT64_MAX &&
      now_ms - notify->last_status_ms < LS_NOTIFY_DEDUP_WINDOW_MS) {
    return true;
  }

  char message[LS_NOTIFY_MESSAGE_SIZE];
  (void)snprintf(message, sizeof(message), "STATUS=%.*s",
                 (int)LS_NOTIFY_STATUS_SIZE - 1, status);
  bool ok = notify_send(notify, message);
  if (ok) {
    (void)snprintf(notify->last_status, sizeof(notify->last_status), "%s",
                   status);
    notify->last_status_ms = (now_ms == UINT64_MAX) ? 0 : now_ms;
  }
  return ok;
}

ls_notify_state_t ls_notify_init(ls_notify_t *restrict notify,
                                 ls_err_t *restrict err) {
  *notify = (ls_notify_t){.sockfd = -1};

  const char *socket_path = getenv("NOTIFY_SOCKET");
  if (socket_path == nullptr) {
    return LS_NOTIFY_INACTIVE;
  }

  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    (void)ls_err_set(err, "Cannot create notify socket: %s", strerror(errno));
    return LS_NOTIFY_FAILED;
  }

  struct sockaddr_un addr;
  socklen_t addr_len;
  if (!build_socket_addr(socket_path, &addr, &addr_len)) {
    close(fd);
    (void)ls_err_set(err, "Invalid NOTIFY_SOCKET path: %s", socket_path);
    return LS_NOTIFY_FAILED;
  }

  if (connect(fd, (const struct sockaddr *)&addr, addr_len) != 0) {
    int connect_errno = errno;
    close(fd);
    (void)ls_err_set(err, "Cannot connect to NOTIFY_SOCKET %s: %s",
                     socket_path, strerror(connect_errno));
    return LS_NOTIFY_FAILED;
  }

  notify->sockfd = fd;
  notify->enabled = true;

  const char *watchdog_str = getenv("WATCHDOG_USEC");
  if (watchdog_str != nullptr && watchdog_pid_matches_self()) {
    uint64_t usec = 0;
    if (parse_uint64(watchdog_str, &usec)) {
      notify->watchdog_usec = usec;
    }
  }
  return LS_NOTIFY_ACTIVE;
}

void ls_notify_destroy(ls_notify_t *restrict notify) {
  if (notify->sockfd >= 0) {
    close(notify->sockfd);
    notify->sockfd = -1;
  }
  notify->enabled = false;
}

bool ls_notify_enabled(const ls_notify_t *restrict notify) {
  return notify->enabled;
}

bool ls_notify_ready(ls_notify_t *restrict notify) {
  return notify_send(notify, "READY=1");
}

bool ls_notify_statusf(ls_notify_t *restrict notify, const char *restrict fmt,
                       ...) {
  if (!notify->enabled) {
    return false;
  }
  char status[LS_NOTIFY_STATUS_SIZE];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(status, sizeof(status), fmt, args);
  va_end(args);
  return notify_status(notify, status);
}

bool ls_notify_stopping(ls_notify_t *restrict notify) {
  return notify_send(notify, "STOPPING=1");
}

bool ls_notify_watchdog(ls_notify_t *restrict notify) {
  if (LS_UNLIKELY(!notify->enabled || notify->watchdog_usec == 0)) {
    return false;
  }
  return notify_send(notify, "WATCHDOG=1");
}

uint64_t ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify) {
  if (!notify->enabled || notify->watchdog_usec == 0) {
    return 0;
  }
  uint64_t interval_ms = notify->watchdog_usec / (LS_MS_PER_SEC * 2);
  return interval_ms == 0 ? 1 : interval_ms;
}

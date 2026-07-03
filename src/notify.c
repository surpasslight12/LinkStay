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
#define LS_NOTIFY_RETRY_COUNT 3
#define LS_NOTIFY_RETRY_NS 10000000L

/* ---- Environment parsing ---- */

static bool parse_uint64(const char *restrict value,
                         uint64_t *restrict out_value) {
  if (value == nullptr) {
    return false;
  }
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

static bool watchdog_pid_matches_self(void) {
  const char *pid_str = getenv("WATCHDOG_PID");
  /* Unset means systemd did not request PID pinning, which is acceptable. */
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

/* Supports both abstract (@name) and filesystem notify socket paths. */
static bool build_socket_addr(const char *restrict path,
                              struct sockaddr_un *restrict addr,
                              socklen_t *restrict addr_len) {
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

/* ---- Message transmission ---- */

static bool send_message(ls_notify_t *restrict notify,
                         const char *restrict message) {
  if (LS_UNLIKELY(notify == nullptr || message == nullptr ||
                  !notify->enabled)) {
    return false;
  }

  size_t message_len = strlen(message);
  for (int attempt = 0; attempt < LS_NOTIFY_RETRY_COUNT; attempt++) {
    if (send(notify->sockfd, message, message_len, MSG_NOSIGNAL) >= 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
      struct timespec retry = {.tv_sec = 0, .tv_nsec = LS_NOTIFY_RETRY_NS};
      while (nanosleep(&retry, &retry) < 0 && errno == EINTR) {
      }
      continue;
    }
    break;
  }
  return false;
}

/* ---- Public API ---- */

void ls_notify_init(ls_notify_t *restrict notify) {
  if (notify == nullptr) {
    return;
  }
  *notify = (ls_notify_t){.sockfd = -1};

  const char *socket_path = getenv("NOTIFY_SOCKET");
  if (socket_path == nullptr) {
    return;
  }

  notify->sockfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (notify->sockfd < 0) {
    return;
  }

  struct sockaddr_un addr;
  socklen_t addr_len;
  if (!build_socket_addr(socket_path, &addr, &addr_len)) {
    close(notify->sockfd);
    notify->sockfd = -1;
    return;
  }

  int rc;
  do {
    rc = connect(notify->sockfd, (const struct sockaddr *)&addr, addr_len);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    close(notify->sockfd);
    notify->sockfd = -1;
    return;
  }

  notify->enabled = true;

  const char *watchdog_str = getenv("WATCHDOG_USEC");
  if (watchdog_str != nullptr && watchdog_pid_matches_self()) {
    uint64_t usec = 0;
    if (parse_uint64(watchdog_str, &usec)) {
      notify->watchdog_usec = usec;
    }
  }
}

void ls_notify_destroy(ls_notify_t *restrict notify) {
  if (notify == nullptr) {
    return;
  }
  if (notify->sockfd >= 0) {
    close(notify->sockfd);
    notify->sockfd = -1;
  }
  notify->enabled = false;
}

bool ls_notify_enabled(const ls_notify_t *restrict notify) {
  return notify != nullptr && notify->enabled;
}

bool ls_notify_ready(ls_notify_t *restrict notify) {
  return send_message(notify, "READY=1");
}

bool ls_notify_status(ls_notify_t *restrict notify,
                      const char *restrict status) {
  if (notify == nullptr || !notify->enabled || status == nullptr) {
    return false;
  }

  uint64_t now_ms = ls_now_ms();
  /* De-duplicate identical status messages within the window to reduce
   * socket traffic. strcmp is safe: both strings are NUL-terminated. */
  bool same = strcmp(notify->last_status, status) == 0;
  if (same && notify->last_status_ms != 0 && now_ms != UINT64_MAX &&
      now_ms - notify->last_status_ms < LS_NOTIFY_DEDUP_WINDOW_MS) {
    return true;
  }

  char message[LS_NOTIFY_MESSAGE_SIZE];
  (void)snprintf(message, sizeof(message), "STATUS=%.*s",
                 (int)LS_NOTIFY_STATUS_SIZE - 1, status);
  bool ok = send_message(notify, message);
  if (ok) {
    (void)snprintf(notify->last_status, sizeof(notify->last_status), "%s",
                   status);
    notify->last_status_ms = (now_ms == UINT64_MAX) ? 0 : now_ms;
  }
  return ok;
}

bool ls_notify_statusf(ls_notify_t *restrict notify, const char *restrict fmt,
                       ...) {
  if (notify == nullptr || !notify->enabled || fmt == nullptr) {
    return false;
  }
  char status[LS_NOTIFY_STATUS_SIZE];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(status, sizeof(status), fmt, args);
  va_end(args);
  return ls_notify_status(notify, status);
}

bool ls_notify_stopping(ls_notify_t *restrict notify) {
  return send_message(notify, "STOPPING=1");
}

bool ls_notify_watchdog(ls_notify_t *restrict notify) {
  if (LS_UNLIKELY(notify == nullptr || !notify->enabled ||
                  notify->watchdog_usec == 0)) {
    return false;
  }
  return send_message(notify, "WATCHDOG=1");
}

uint64_t ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify) {
  if (notify == nullptr || !notify->enabled || notify->watchdog_usec == 0) {
    return 0;
  }
  /* Heartbeat at half the systemd timeout for a comfortable safety margin. */
  uint64_t interval_ms = notify->watchdog_usec / (LS_MS_PER_SEC * 2);
  return interval_ms == 0 ? 1 : interval_ms;
}

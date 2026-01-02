#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int write_full(int fd, const void* buf, int n) {
  const char* p = (const char*)buf;
  int left = n;
  while (left > 0) {
    int w = (int)write(fd, p, (size_t)left);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (w == 0) return -1;
    p += w;
    left -= w;
  }
  return 0;
}

static int read_full(int fd, void* buf, int n) {
  char* p = (char*)buf;
  int left = n;
  while (left > 0) {
    int r = (int)read(fd, p, (size_t)left);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) return -1;
    p += r;
    left -= r;
  }
  return 0;
}

int net_listen(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost */
  addr.sin_port = htons((unsigned short)port);

  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 32) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

int net_port_of_listenfd(int listenfd) {
  struct sockaddr_in addr;
  socklen_t len = (socklen_t)sizeof(addr);
  if (getsockname(listenfd, (struct sockaddr*)&addr, &len) < 0) return -1;
  return (int)ntohs(addr.sin_port);
}

int net_accept(int listenfd) {
  int fd;
  for (;;) {
    fd = accept(listenfd, NULL, NULL);
    if (fd < 0 && errno == EINTR) continue;
    break;
  }
  return fd;
}

int net_connect(const char* host, int port) {
  (void)host; /* pre jednoduchý localhost scenár */
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((unsigned short)port);

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int net_send_msg(int fd, const Msg* m) {
  int header[2];
  header[0] = htonl(m->type);
  header[1] = htonl(m->len);
  if (write_full(fd, header, (int)sizeof(header)) < 0) return -1;
  if (m->len > 0) {
    if (m->len > (int)sizeof(m->data)) return -1;
    if (write_full(fd, m->data, m->len) < 0) return -1;
  }
  return 0;
}

int net_recv_msg(int fd, Msg* m) {
  int header[2];
  if (read_full(fd, header, (int)sizeof(header)) < 0) return -1;
  m->type = ntohl(header[0]);
  m->len = ntohl(header[1]);
  if (m->len < 0 || m->len > (int)sizeof(m->data)) return -1;
  if (m->len > 0) {
    if (read_full(fd, m->data, m->len) < 0) return -1;
  }
  return 0;
}

/* --- util --- */
long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long ms = (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
  return ms;
}

void sleep_ms(int ms) {
  if (ms <= 0) return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
  }
}

int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* --- registry: jednoduchý textový súbor + flock --- */

static int reg_open_locked(int flags) {
  int fd = open(REGISTRY_PATH, flags, 0644);
  if (fd < 0) return -1;
  if (flock(fd, LOCK_EX) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void reg_close_locked(int fd) {
  flock(fd, LOCK_UN);
  close(fd);
}

static int reg_read_all(int fd, ServerInfo out[], int max) {
  lseek(fd, 0, SEEK_SET);

  FILE* f = fdopen(dup(fd), "r");
  if (!f) return -1;

  int n = 0;
  char line[512];
  while (fgets(line, (int)sizeof(line), f)) {
    if (n >= max) break;
    ServerInfo s;
    memset(&s, 0, sizeof(s));

    /* pid port cur max mode world map */
    int k = sscanf(line, "%d %d\n",
                   &s.pid, &s.port);
    if (k >= 2) out[n++] = s;
  }

  fclose(f);
  return n;
}

static int reg_write_all(int fd, const ServerInfo in[], int n) {
  ftruncate(fd, 0);
  lseek(fd, 0, SEEK_SET);

  FILE* f = fdopen(dup(fd), "w");
  if (!f) return -1;

  for (int i = 0; i < n; i++) {
    const ServerInfo* s = &in[i];
    fprintf(f, "%d %d\n", s->pid, s->port);
  }
  fflush(f);
  fclose(f);
  return 0;
}

static int is_pid_alive(int pid) {
  if (pid <= 0) return 0;
  if (kill(pid, 0) == 0) return 1;
  return (errno == EPERM);
}

int reg_add(const ServerInfo* s) {
  int fd = reg_open_locked(O_RDWR);
  if (fd < 0) return -1;

  ServerInfo list[128];
  int n = reg_read_all(fd, list, 128);
  if (n < 0) { reg_close_locked(fd); return -1; }

  /* remove same pid if exists */
  int w = 0;
  for (int i = 0; i < n; i++) {
    if (list[i].pid != s->pid) list[w++] = list[i];
  }
  if (w < 128) list[w++] = *s;

  int rc = reg_write_all(fd, list, w);
  reg_close_locked(fd);
  return rc;
}

int reg_remove(int pid) {
  int fd = reg_open_locked(O_CREAT | O_RDWR);
  if (fd < 0) return -1;

  ServerInfo list[128];
  int n = reg_read_all(fd, list, 128);
  if (n < 0) { reg_close_locked(fd); return -1; }

  int w = 0;
  for (int i = 0; i < n; i++) {
    if (list[i].pid != pid) list[w++] = list[i];
  }

  int rc = reg_write_all(fd, list, w);
  reg_close_locked(fd);
  return rc;
}

int reg_list(ServerInfo out[], int max) {
  int fd = reg_open_locked(O_RDWR);
  if (fd < 0) return -1;

  ServerInfo list[256];
  int n = reg_read_all(fd, list, 256);
  if (n < 0) { reg_close_locked(fd); return -1; }

  /* prune dead while reading */
  int w = 0;
  for (int i = 0; i < n; i++) {
    if (is_pid_alive(list[i].pid)) list[w++] = list[i];
  }
  reg_write_all(fd, list, w);

  int outn = (w < max) ? w : max;
  for (int i = 0; i < outn; i++) out[i] = list[i];

  reg_close_locked(fd);
  return outn;
}

int reg_prune_dead(void) {
  int fd = reg_open_locked(O_CREAT | O_RDWR);
  if (fd < 0) return -1;

  ServerInfo list[256];
  int n = reg_read_all(fd, list, 256);
  if (n < 0) { reg_close_locked(fd); return -1; }

  int w = 0;
  for (int i = 0; i < n; i++) {
    if (is_pid_alive(list[i].pid)) list[w++] = list[i];
  }

  int removed = n - w;
  reg_write_all(fd, list, w);
  reg_close_locked(fd);
  return removed;
}

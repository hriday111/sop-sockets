#define _GNU_SOURCE
#include "sop-sockets-helper.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression)                                         \
  (__extension__({                                                             \
    long int __result;                                                         \
    do                                                                         \
      __result = (long int)(expression);                                       \
    while (__result == -1L && errno == EINTR);                                 \
    __result;                                                                  \
  }))
#endif

/* glue for labs: sockets, epoll, tiny buffers — use at own risk */

void skh_die(const char *ctx, const char *why) {
  /* exit after printing a clean error */
  fprintf(stderr, "%s: %s\n", ctx, why);
  exit(EXIT_FAILURE);
}

void skh_perror_die(const char *ctx) {
  /* perror + exit */
  perror(ctx);
  exit(EXIT_FAILURE);
}

void skh_warn(const char *ctx, const char *msg) {
  /* stderr only, keep going */
  fprintf(stderr, "%s: %s\n", ctx, msg);
}

int skh_errno_is_wouldblock(int e) { /* boring but saves typos */
  return e == EAGAIN || e == EWOULDBLOCK;
}

void skh_ignore_sigpipe(void) {
  /* writing to dead peers won't SIGPIPE you */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  (void)sigaction(SIGPIPE, &sa, NULL);
}

int skh_set_nonblocking(int fd, int on) {
  /* flip O_NONBLOCK */
  int fl = TEMP_FAILURE_RETRY(fcntl(fd, F_GETFL));
  if (fl < 0)
    return -1;
  if (on)
    fl |= O_NONBLOCK;
  else
    fl &= (int)~O_NONBLOCK;
  return TEMP_FAILURE_RETRY(fcntl(fd, F_SETFL, fl));
}

int skh_set_cloexec(int fd, int on) {
  /* don't leak fds to children */
  int fl = TEMP_FAILURE_RETRY(fcntl(fd, F_GETFD));
  if (fl < 0)
    return -1;
  if (on)
    fl |= FD_CLOEXEC;
  else
    fl &= ~FD_CLOEXEC;
  return TEMP_FAILURE_RETRY(fcntl(fd, F_SETFD, fl));
}

int skh_unlink_if_exists(const char *path) {
  /* ENOENT is fine */
  if (unlink(path) < 0 && errno != ENOENT)
    return -1;
  return 0;
}

int skh_tcp_listen(uint16_t port, int backlog) {
  /* IPv4 any, SO_REUSEADDR, then listen */
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    return -1;
  int one = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    close(s);
    return -1;
  }
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(s);
    return -1;
  }
  if (listen(s, backlog) < 0) {
    close(s);
    return -1;
  }
  return s;
}

int skh_tcp_connect_ipv4(const char *host, uint16_t port) {
  /* getaddrinfo walk until one connect sticks */
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portbuf[16];
  snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
  int g = getaddrinfo(host, portbuf, &hints, &res);
  if (g != 0) {
    errno = EINVAL;
    return -1;
  }
  int s = -1;
  for (struct addrinfo *p = res; p; p = p->ai_next) {
    s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (s < 0)
      continue;
    if (connect(s, p->ai_addr, p->ai_addrlen) == 0)
      break;
    close(s);
    s = -1;
  }
  freeaddrinfo(res);
  return s;
}

int skh_unix_stream_socket(void) {
  /* PF_LOCAL SOCK_STREAM */
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  return s;
}

int skh_unix_bind_listen(const char *path, int backlog) {
  /* unlink old path, bind sun_path, listen */
  if (skh_unlink_if_exists(path) < 0)
    return -1;
  int s = skh_unix_stream_socket();
  if (s < 0)
    return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    close(s);
    errno = ENAMETOOLONG;
    return -1;
  }
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  if (bind(s, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) < 0) {
    close(s);
    return -1;
  }
  if (listen(s, backlog) < 0) {
    close(s);
    return -1;
  }
  return s;
}

int skh_unix_connect(const char *path) {
  /* client side AF_UNIX stream */
  int s = skh_unix_stream_socket();
  if (s < 0)
    return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    close(s);
    errno = ENAMETOOLONG;
    return -1;
  }
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  if (connect(s, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) < 0) {
    close(s);
    return -1;
  }
  return s;
}

int skh_epoll_create(void) {
  /* CLOEXEC so forks don't inherit it by accident */
  int e = epoll_create1(EPOLL_CLOEXEC);
  if (e < 0)
    return -1;
  return e;
}

int skh_epoll_add(int epfd, int fd, uint32_t events, int u32_or_fd) {
  /* stash fd in ev.data.fd (common pattern) */
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = u32_or_fd;
  return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int skh_epoll_add_ptr(int epfd, int fd, uint32_t events, void *ptr) {
  /* when you want ev.data.ptr instead */
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.ptr = ptr;
  return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int skh_epoll_mod(int epfd, int fd, uint32_t events, int u32_or_fd) {
  /* change interest mask / payload */
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = u32_or_fd;
  return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

int skh_epoll_mod_ptr(int epfd, int fd, uint32_t events, void *ptr) {
  /* mod with ptr payload */
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.ptr = ptr;
  return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

int skh_epoll_del(int epfd, int fd) {
  /* drop fd from set */
  return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

ssize_t skh_read_some(int fd, void *buf, size_t cap) {
  /* one read(), EINTR retried */
  ssize_t n = TEMP_FAILURE_RETRY(read(fd, buf, cap));
  return n;
}

ssize_t skh_write_all(int fd, const void *buf, size_t len) {
  /* loop write until done or hard fail */
  const unsigned char *p = (const unsigned char *)buf;
  size_t done = 0;
  while (done < len) {
    ssize_t n = TEMP_FAILURE_RETRY(write(fd, p + done, len - done));
    if (n < 0)
      return -1;
    if (n == 0) {
      errno = EPIPE;
      return -1;
    }
    done += (size_t)n;
  }
  return (ssize_t)len;
}

void skh_lb_init(skh_linebuf *b) {
  /* empty growable buffer */
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void skh_lb_free(skh_linebuf *b) {
  /* free + zero */
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void skh_lb_clear(skh_linebuf *b) {
  /* logical empty, keep allocation */
  b->len = 0;
  if (b->data && b->cap > 0)
    b->data[0] = '\0';
}

int skh_lb_reserve(skh_linebuf *b, size_t need) {
  /* grow backing store if need be */
  if (need <= b->cap)
    return 0;
  size_t ncap = b->cap ? b->cap : 128;
  while (ncap < need) {
    if (ncap > SIZE_MAX / 2) {
      errno = ENOMEM;
      return -1;
    }
    ncap *= 2;
  }
  char *nd = (char *)realloc(b->data, ncap);
  if (!nd)
    return -1;
  b->data = nd;
  b->cap = ncap;
  return 0;
}

ssize_t skh_lb_read_append(skh_linebuf *b, int fd, size_t max_append) {
  /* read up to max_append bytes onto the end */
  if (max_append == 0)
    return 0;
  if (b->len > SIZE_MAX - max_append) {
    errno = ENOMEM;
    return -1;
  }
  if (skh_lb_reserve(b, b->len + max_append + 1) < 0)
    return -1;
  ssize_t n = skh_read_some(fd, b->data + b->len, max_append);
  if (n < 0)
    return -1;
  b->len += (size_t)n;
  b->data[b->len] = '\0';
  return n;
}

char *skh_lb_extract_line(skh_linebuf *b, size_t max_line_including_nul) {
  /* malloc one line including newline, shift buffer */
  if (!b->data || b->len == 0)
    return NULL;
  char *nl = memchr(b->data, '\n', b->len);
  if (!nl)
    return NULL;
  size_t line_len = (size_t)(nl - b->data) + 1u;
  if (line_len + 1u > max_line_including_nul) {
    errno = EMSGSIZE;
    return NULL;
  }
  char *out = (char *)malloc(line_len + 1u);
  if (!out)
    return NULL;
  memcpy(out, b->data, line_len);
  out[line_len] = '\0';
  skh_lb_drop_prefix(b, line_len);
  return out;
}

void skh_lb_drop_prefix(skh_linebuf *b, size_t n) {
  /* memmove leftovers */
  if (n >= b->len) {
    skh_lb_clear(b);
    return;
  }
  memmove(b->data, b->data + n, b->len - n);
  b->len -= n;
  b->data[b->len] = '\0';
}

void skh_rb_init(skh_ringbuf *r, size_t cap) {
  /* linear fifo-ish chunk, cap hint */
  size_t c = cap ? cap : 4096;
  r->data = (char *)malloc(c);
  r->cap = r->data ? c : 0;
  r->head = r->tail = 0;
}

void skh_rb_free(skh_ringbuf *r) {
  /* tear it down */
  free(r->data);
  r->data = NULL;
  r->cap = 0;
  r->head = r->tail = 0;
}

size_t skh_rb_used(const skh_ringbuf *r) {
  /* bytes sitting between head/tail */
  return r->tail - r->head;
}

size_t skh_rb_free_cap(const skh_ringbuf *r) {
  /* tailroom for one more read */
  if (!r->data || r->cap <= r->tail)
    return 0;
  return r->cap - r->tail;
}

ssize_t skh_rb_read_fd(skh_ringbuf *r, int fd) {
  /* compact if full, maybe grow, then read */
  if (!r->data || r->cap == 0) {
    errno = EINVAL;
    return -1;
  }
  if (r->tail == r->cap) {
    size_t used = r->tail - r->head;
    memmove(r->data, r->data + r->head, used);
    r->head = 0;
    r->tail = used;
  }
  if (r->tail >= r->cap) {
    size_t ncap = r->cap * 2u;
    char *nd = (char *)realloc(r->data, ncap);
    if (!nd)
      return -1;
    r->data = nd;
    r->cap = ncap;
  }
  ssize_t n = skh_read_some(fd, r->data + r->tail, r->cap - r->tail);
  if (n < 0)
    return -1;
  r->tail += (size_t)n;
  return n;
}

int skh_rb_find_byte(const skh_ringbuf *r, unsigned char c, size_t *out_pos) {
  /* linear scan for a delimiter */
  for (size_t i = r->head; i < r->tail; i++) {
    if ((unsigned char)r->data[i] == c) {
      if (out_pos)
        *out_pos = i;
      return 1;
    }
  }
  return 0;
}

void skh_rb_pop(skh_ringbuf *r, size_t n) {
  /* eat bytes from the front */
  if (n >= (r->tail - r->head)) {
    r->head = r->tail = 0;
    return;
  }
  r->head += n;
}

ssize_t skh_rb_write_fd(skh_ringbuf *r, int fd) {
  /* dump whole used region then reset */
  size_t used = r->tail - r->head;
  if (used == 0)
    return 0;
  ssize_t n = skh_write_all(fd, r->data + r->head, used);
  if (n < 0)
    return -1;
  r->head = r->tail = 0;
  return n;
}

int skh_accept_nonblocking(int listenfd) {
  /* accept + NB + cloexec */
  int c = TEMP_FAILURE_RETRY(accept(listenfd, NULL, NULL));
  if (c < 0)
    return -1;
  if (skh_set_nonblocking(c, 1) < 0) {
    close(c);
    return -1;
  }
  if (skh_set_cloexec(c, 1) < 0) {
    close(c);
    return -1;
  }
  return c;
}

char *skh_strdup_line(const char *start, size_t len) {
  /* tiny malloc copy */
  char *s = (char *)malloc(len + 1u);
  if (!s)
    return NULL;
  memcpy(s, start, len);
  s[len] = '\0';
  return s;
}

void skh_trim_crlf_inplace(char *s) {
  /* strip trailing \\r\\n */
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[n - 1] = '\0';
    n--;
  }
}

char *skh_split_first_colon(char *line, char **msg_out) {
  /* mutates line, returns addressee part */
  char *c = strchr(line, ':');
  if (!c) {
    *msg_out = NULL;
    return NULL;
  }
  *c = '\0';
  *msg_out = c + 1;
  return line;
}

int skh_parse_u16(const char *s, uint16_t *out) {
  /* strict decimal port */
  char *end = NULL;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (!s || !end || *end != '\0' || errno == ERANGE)
    return -1;
  if (v > 65535UL)
    return -1;
  *out = (uint16_t)v;
  return 0;
}

int skh_parse_positive_int(const char *s, int *out) {
  /* argv timeout style */
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (!s || !end || *end != '\0' || errno == ERANGE)
    return -1;
  if (v < 1 || v > INT_MAX)
    return -1;
  *out = (int)v;
  return 0;
}

size_t skh_strlcpy(char *dst, const char *src, size_t dstsz) {
  /* BSD-ish, always nul-terminate dst */
  if (dstsz == 0)
    return strlen(src);
  size_t i = 0;
  for (; i + 1 < dstsz && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
  return strlen(src);
}

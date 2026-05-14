#include "l7-common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t stop = 0;

static void on_sigint(int sig) {
  (void)sig;
  stop = 1;
}

int skh_parse_u16(const char *s, uint16_t *out) {
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

int skh_tcp_listen(uint16_t port, int backlog) {
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

void skh_trim_crlf_inplace(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[n - 1] = '\0';
    n--;
  }
}

/* read(2) with EINTR: if SIGINT set `stop`, return -1 and errno == EINTR. */
static ssize_t read_eintr(int fd, void *buf, size_t cap) {
  for (;;) {
    ssize_t n = read(fd, buf, cap);
    if (n >= 0)
      return n;
    if (errno != EINTR)
      return -1;
    if (stop)
      return -1;
  }
}

/* write(2) until len bytes or error; honors `stop` on EINTR. */
static int write_all_eintr(int fd, const void *buf, size_t len) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t done = 0;
  while (done < len) {
    ssize_t n = write(fd, p + done, len - done);
    if (n > 0) {
      done += (size_t)n;
      continue;
    }
    if (n == 0) {
      errno = EPIPE;
      return -1;
    }
    if (errno == EINTR) {
      if (stop)
        return -1;
      continue;
    }
    return -1;
  }
  return 0;
}

static int sum_digits(const char *s) {
  int sum = 0;
  for (; *s; s++) {
    if (isdigit((unsigned char)*s))
      sum += *s - '0';
  }
  return sum;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    exit(EXIT_FAILURE);
  }

  uint16_t port;
  if (skh_parse_u16(argv[1], &port) < 0) {
    fprintf(stderr, "bad port\n");
    exit(EXIT_FAILURE);
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, NULL) < 0)
    ERR("sigaction SIGINT");

  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  ign.sa_flags = 0;
  if (sigaction(SIGPIPE, &ign, NULL) < 0)
    ERR("sigaction SIGPIPE");

  int listenfd = skh_tcp_listen(port, 16);
  if (listenfd < 0)
    ERR("listen");

  int max_sum = 0;

  while (!stop) {
    int connfd = accept(listenfd, NULL, NULL);
    if (connfd < 0) {
      if (errno == EINTR) {
        if (stop)
          break;
        continue;
      }
      perror("accept");
      break;
    }

    char buf[4096];
    size_t total = 0;
    int read_err = 0;

    for (;;) {
      if (stop) {
        read_err = 1;
        break;
      }
      ssize_t n = read_eintr(connfd, buf + total, sizeof(buf) - 1u - total);
      if (n < 0) {
        if (!(errno == EINTR && stop))
          perror("read");
        read_err = 1;
        break;
      }
      if (n == 0)
        break;
      total += (size_t)n;
      if (total >= sizeof(buf) - 1u)
        break;
    }

    if (read_err) {
      close(connfd);
      continue;
    }

    buf[total] = '\0';
    skh_trim_crlf_inplace(buf);

    int sum = sum_digits(buf);
    if (sum > max_sum)
      max_sum = sum;

    int16_t reply = (int16_t)sum;
    uint16_t wire = htons((uint16_t)reply);

    if (write_all_eintr(connfd, &wire, sizeof(wire)) < 0) {
      if (!(errno == EINTR && stop) && errno != EPIPE && errno != ECONNRESET)
        perror("write");
    }
    close(connfd);
  }

  close(listenfd);
  printf("HIGH SUM=%d\n", max_sum);
  return EXIT_SUCCESS;
}

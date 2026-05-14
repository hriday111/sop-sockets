#include "l7-common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
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
void skh_trim_crlf_inplace(char *s) {
  /* strip trailing \\r\\n */
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[n - 1] = '\0';
    n--;
  }
}
ssize_t skh_read_some(int fd, void *buf, size_t cap) {
  /* one read(), EINTR retried */
  ssize_t n = TEMP_FAILURE_RETRY(read(fd, buf, cap));
  return n;
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
    for (;;) {
      if (connect(s, p->ai_addr, p->ai_addrlen) == 0)
        goto connected;
      if (errno != EINTR) {
        close(s);
        s = -1;
        break;
      }
    }
  }
connected:
  freeaddrinfo(res);
  return s;
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
int main(int argc, char **argv) {
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    if (sigaction(SIGPIPE, &ign, NULL) < 0)
        ERR("sigaction SIGPIPE");

    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    uint16_t port;
    if(skh_parse_u16(argv[2], &port)<0){ERR("bad port");}

    pid_t pid = getpid();

    printf("PID=%d\n", pid);
    int fd = skh_tcp_connect_ipv4(argv[1], port);
    if(fd<0){ERR("tcp connect ipv4");}
    char msg[64];

    int len = snprintf(msg, sizeof(msg), "%d\n", pid);
    if (len < 0 || (size_t)len >= sizeof(msg)) {
        ERR("snprintf");
    }

    if (skh_write_all(fd, msg, (size_t)len) < 0) {
        ERR("Write all");
    }

    if (shutdown(fd, SHUT_WR) < 0) {
        ERR("shutdown");
    }

    unsigned char rbuf[2];
    size_t got = 0;
    while (got < sizeof(rbuf)) {
        ssize_t n;
        do {
            n = read(fd, rbuf + got, sizeof(rbuf) - got);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            fprintf(stderr, "read: %s\n", strerror(errno));
            close(fd);
            return EXIT_FAILURE;
        }
        if (n == 0) {
            fprintf(stderr, "unexpected EOF from server\n");
            exit(EXIT_FAILURE);
        }
        got += (size_t)n;
    }

    uint16_t wire;
    memcpy(&wire, rbuf, sizeof(wire));
    int16_t sum = (int16_t)ntohs(wire);
    printf("SUM=%d\n", (int)sum);

    close(fd);
    return EXIT_SUCCESS;
}

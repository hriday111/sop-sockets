#ifndef SOP_SOCKETS_HELPER_H
#define SOP_SOCKETS_HELPER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/types.h>

#define SKH_ERRBUF 256

void skh_die(const char *ctx, const char *why);
void skh_perror_die(const char *ctx);
void skh_warn(const char *ctx, const char *msg);
int skh_errno_is_wouldblock(int e);

void skh_ignore_sigpipe(void);

int skh_set_nonblocking(int fd, int on);
int skh_set_cloexec(int fd, int on);

int skh_unlink_if_exists(const char *path);

int skh_tcp_listen(uint16_t port, int backlog);
int skh_tcp_connect_ipv4(const char *host, uint16_t port);

int skh_unix_stream_socket(void);
int skh_unix_bind_listen(const char *path, int backlog);
int skh_unix_connect(const char *path);

int skh_epoll_create(void);
int skh_epoll_add(int epfd, int fd, uint32_t events, int u32_or_fd);
int skh_epoll_add_ptr(int epfd, int fd, uint32_t events, void *ptr);
int skh_epoll_mod(int epfd, int fd, uint32_t events, int u32_or_fd);
int skh_epoll_mod_ptr(int epfd, int fd, uint32_t events, void *ptr);
int skh_epoll_del(int epfd, int fd);

ssize_t skh_read_some(int fd, void *buf, size_t cap);
ssize_t skh_write_all(int fd, const void *buf, size_t len);

typedef struct skh_linebuf {
  char *data;
  size_t len;
  size_t cap;
} skh_linebuf;

void skh_lb_init(skh_linebuf *b);
void skh_lb_free(skh_linebuf *b);
void skh_lb_clear(skh_linebuf *b);
int skh_lb_reserve(skh_linebuf *b, size_t need);
ssize_t skh_lb_read_append(skh_linebuf *b, int fd, size_t max_append);
char *skh_lb_extract_line(skh_linebuf *b, size_t max_line_including_nul);
void skh_lb_drop_prefix(skh_linebuf *b, size_t n);

typedef struct skh_ringbuf {
  char *data;
  size_t cap;
  size_t head;
  size_t tail;
} skh_ringbuf;

void skh_rb_init(skh_ringbuf *r, size_t cap);
void skh_rb_free(skh_ringbuf *r);
size_t skh_rb_used(const skh_ringbuf *r);
size_t skh_rb_free_cap(const skh_ringbuf *r);
ssize_t skh_rb_read_fd(skh_ringbuf *r, int fd);
int skh_rb_find_byte(const skh_ringbuf *r, unsigned char c, size_t *out_pos);
void skh_rb_pop(skh_ringbuf *r, size_t n);
ssize_t skh_rb_write_fd(skh_ringbuf *r, int fd);

int skh_accept_nonblocking(int listenfd);

char *skh_strdup_line(const char *start, size_t len);
void skh_trim_crlf_inplace(char *s);
char *skh_split_first_colon(char *line, char **msg_out);

int skh_parse_u16(const char *s, uint16_t *out);
int skh_parse_positive_int(const char *s, int *out);

size_t skh_strlcpy(char *dst, const char *src, size_t dstsz);

#endif

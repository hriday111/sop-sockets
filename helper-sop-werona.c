#include "l7-common.h"
#include "sop-sockets-helper.h"
#include <errno.h>
#include <stdbool.h>

void usage(char *name) {
  printf("%s <timeout>\n", name);
  printf("  timeout - max waiting time after receiving the last "
         "message/connection (in seconds)\n");
  exit(EXIT_FAILURE);
}

#define SWAP(a, b)                                                             \
  do {                                                                         \
    __typeof__(a) __a = (a);                                                   \
    __typeof__(b) __b = (b);                                                   \
    __typeof__(*__a) __tmp = *__a;                                             \
    *__a = *__b;                                                               \
    *__b = __tmp;                                                              \
  } while (0)

#define MAX_CLIENTS 10
#define MAX_PAIRS 3
#define UNIX_SK_NAME "Laurenty"
#define MAX_MSG_LEN 63
#define BACKLOG 3
#define MAX_EPOLL_EVENTS (MAX_CLIENTS + 3)

typedef struct client {
  int fd;
  int partner;
  char name[MAX_MSG_LEN + 1];
  char name_of_beloved[MAX_MSG_LEN + 1];
  char buff[MAX_MSG_LEN + 1];
  int buff_size;
} client_t;

void initialize_clients(client_t *clients) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    clients[i].fd = -1;
    clients[i].partner = -1;
    clients[i].name[0] = '\0';
    clients[i].buff[0] = '\0';
    clients[i].name_of_beloved[0] = '\0';
    clients[i].buff_size = 0;
  }
}

void delete_client(client_t *clients, int idx) {
  if (idx < 0 || idx >= MAX_CLIENTS)
    return;
  int p = clients[idx].partner;
  if (p >= 0 && p < MAX_CLIENTS && clients[p].partner == idx)
    clients[p].partner = -1;
  if (clients[idx].fd != -1)
    close(clients[idx].fd);
  clients[idx].fd = -1;
  clients[idx].partner = -1;
  clients[idx].name[0] = '\0';
  clients[idx].name_of_beloved[0] = '\0';
  clients[idx].buff[0] = '\0';
  clients[idx].buff_size = 0;
}

void close_all_clients(client_t *clients) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].fd != -1)
      delete_client(clients, i);
  }
}

static void consume_line(client_t *c, char *nl) {
  int remainder = c->buff_size - (int)(nl - c->buff) - 1;
  if (remainder < 0)
    remainder = 0;
  memmove(c->buff, nl + 1, (size_t)remainder);
  c->buff_size = remainder;
  c->buff[c->buff_size] = '\0';
}

int find_free_client_index(client_t *clients) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].fd == -1) {
      return i;
    }
  }
  return -1;
}

int find_client_index(client_t *clients, int fd) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].fd == fd) {
      return i;
    }
  }
  return -1;
}

int find_client_by_name(client_t *clients, const char *name) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].fd < 0)
      continue;
    if (strcmp(clients[i].name, name) == 0)
      return i;
  }
  return -1;
}

void perform_wedding(client_t *clients, int i, int j) {
  const char *a = clients[i].name;
  const char *b = clients[i].name_of_beloved;

  printf("%s and %s got married!\n", a, b);

  char msg[MAX_MSG_LEN + 64];
  int n = snprintf(msg, sizeof(msg), "Congratulations, %s and %s!\n", a, b);
  if (n < 0 || (size_t)n >= sizeof(msg))
    ERR("snprintf");

  if (skh_write_all(clients[i].fd, msg, (size_t)n) < 0)
    ERR("skh_write_all");
  if (skh_write_all(clients[j].fd, msg, (size_t)n) < 0)
    ERR("skh_write_all");

  clients[i].partner = j;
  clients[j].partner = i;
}

int find_partner_index(client_t *clients, int i) {
  if (clients[i].fd < 0)
    return -1;
  if (clients[i].name[0] == '\0' || clients[i].name_of_beloved[0] == '\0')
    return -1;

  for (int j = 0; j < MAX_CLIENTS; j++) {
    if (j == i)
      continue;
    if (clients[j].fd < 0)
      continue;
    if (clients[j].name[0] == '\0' || clients[j].name_of_beloved[0] == '\0')
      continue;

    if (strcmp(clients[i].name, clients[j].name_of_beloved) == 0 &&
        strcmp(clients[j].name, clients[i].name_of_beloved) == 0) {
      return j;
    }
  }
  return -1;
}

static void relay_chat_line(client_t *clients, int from_idx, char *nl) {
  int p = clients[from_idx].partner;
  if (p < 0 || p >= MAX_CLIENTS || clients[p].fd < 0) {
    consume_line(&clients[from_idx], nl);
    return;
  }

  const char *line = clients[from_idx].buff;
  size_t L = strlen(line);
  if (L > (size_t)MAX_MSG_LEN) {
    consume_line(&clients[from_idx], nl);
    return;
  }

  char out[MAX_MSG_LEN + 2];
  memcpy(out, line, L);
  out[L] = '\n';
  if (skh_write_all(clients[p].fd, out, L + 1) < 0)
    ERR("skh_write_all");

  consume_line(&clients[from_idx], nl);
}

static void consume_stdin_prefix(char *buf, int *size, char *nl) {
  int remainder = *size - (int)(nl - buf) - 1;
  if (remainder < 0)
    remainder = 0;
  memmove(buf, nl + 1, (size_t)remainder);
  *size = remainder;
  buf[*size] = '\0';
}

static void process_stdin_line(char *line, client_t *clients) {
  char *msg = NULL;
  char *addressee = skh_split_first_colon(line, &msg);
  if (!addressee || !msg) {
    fprintf(stderr, "Invalid stdin line (expected addressee:message)\n");
    return;
  }
  while (*msg == ' ' || *msg == '\t')
    msg++;

  if (addressee[0] == '\0' || msg[0] == '\0') {
    fprintf(stderr, "Invalid stdin line (empty addressee or message)\n");
    return;
  }

  int idx = find_client_by_name(clients, addressee);
  if (idx < 0) {
    fprintf(stderr, "Unknown addressee: %s\n", addressee);
    return;
  }

  size_t mlen = strlen(msg);
  if (mlen > (size_t)MAX_MSG_LEN) {
    fprintf(stderr, "Message too long\n");
    return;
  }

  char out[MAX_MSG_LEN + 2];
  memcpy(out, msg, mlen);
  out[mlen] = '\n';
  if (skh_write_all(clients[idx].fd, out, mlen + 1) < 0)
    ERR("skh_write_all");
}

void doServer(int local_listen_socket, int timeout) {
  int epoll_descriptor = skh_epoll_create();
  if (epoll_descriptor < 0)
    ERR("skh_epoll_create");

  if (skh_set_nonblocking(STDIN_FILENO, 1) < 0)
    ERR("skh_set_nonblocking stdin");

  struct epoll_event events[MAX_EPOLL_EVENTS];

  if (skh_epoll_add(epoll_descriptor, local_listen_socket, EPOLLIN,
                   local_listen_socket) < 0) {
    ERR("skh_epoll_add listen");
  }

  bool stdin_epoll = true;
  if (skh_epoll_add(epoll_descriptor, STDIN_FILENO, EPOLLIN, STDIN_FILENO) <
      0) {
    fprintf(stderr, "epoll_ctl(STDIN): %s (stdin relay disabled)\n",
            strerror(errno));
    stdin_epoll = false;
  }

  int nfds;
  client_t clients[MAX_CLIENTS];
  initialize_clients(clients);

  char stdin_buff[MAX_MSG_LEN + 1];
  int stdin_size = 0;
  stdin_buff[0] = '\0';

  for (;;) {
    if ((nfds = epoll_wait(epoll_descriptor, events, MAX_EPOLL_EVENTS,
                           timeout * 1000)) == -1) {
      ERR("epoll_wait");
    }
    if (nfds == 0) {
      printf("No one needs my help anymore!\n");
      close_all_clients(clients);
      close(epoll_descriptor);
      return;
    }

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;
      if (fd == local_listen_socket) {
        for (;;) {
          int new_client = skh_accept_nonblocking(local_listen_socket);
          if (new_client < 0) {
            if (skh_errno_is_wouldblock(errno))
              break;
            ERR("accept");
          }

          printf("Another young person %d needs my help!\n", new_client);

          int client_idx = find_free_client_index(clients);
          if (client_idx < 0) {
            close(new_client);
            continue;
          }
          clients[client_idx].fd = new_client;
          if (skh_epoll_add(epoll_descriptor, new_client, EPOLLIN, new_client) <
              0) {
            ERR("skh_epoll_add client");
          }
        }
      } else if (stdin_epoll && fd == STDIN_FILENO) {
        size_t space = sizeof(stdin_buff) - 1 - (size_t)stdin_size;
        if (space == 0) {
          fprintf(stderr, "Stdin buffer overflow, line too long\n");
          stdin_size = 0;
          stdin_buff[0] = '\0';
          continue;
        }
        ssize_t br =
            skh_read_some(STDIN_FILENO, stdin_buff + stdin_size, space);
        if (br < 0) {
          if (skh_errno_is_wouldblock(errno))
            continue;
          ERR("read stdin");
        }
        if (br == 0) {
          (void)skh_epoll_del(epoll_descriptor, STDIN_FILENO);
          stdin_size = 0;
          continue;
        }
        stdin_size += (int)br;
        stdin_buff[stdin_size] = '\0';

        char *nl;
        while ((nl = strchr(stdin_buff, '\n')) != NULL) {
          *nl = '\0';
          process_stdin_line(stdin_buff, clients);
          consume_stdin_prefix(stdin_buff, &stdin_size, nl);
        }
      } else {
        int client_idx = find_client_index(clients, fd);
        if (client_idx < 0)
          continue;

        size_t space = sizeof(clients[client_idx].buff) - 1 -
                       (size_t)clients[client_idx].buff_size;
        if (space == 0) {
          const char *dn =
              clients[client_idx].name[0] ? clients[client_idx].name : "??";
          printf("I lost contact with %s\n", dn);
          delete_client(clients, client_idx);
          continue;
        }

        ssize_t bytes_read =
            skh_read_some(fd, clients[client_idx].buff + clients[client_idx].buff_size,
                          space);
        if (bytes_read < 0) {
          if (skh_errno_is_wouldblock(errno))
            continue;
          ERR("read");
        }
        if (bytes_read == 0) {
          const char *dn =
              clients[client_idx].name[0] ? clients[client_idx].name : "??";
          printf("I lost contact with %s\n", dn);
          delete_client(clients, client_idx);
          continue;
        }

        clients[client_idx].buff_size += (int)bytes_read;
        clients[client_idx].buff[clients[client_idx].buff_size] = '\0';

        char *nl;
        while ((nl = strchr(clients[client_idx].buff, '\n')) != NULL) {
          *nl = '\0';
          if (clients[client_idx].name[0] == '\0') {
            strncpy(clients[client_idx].name, clients[client_idx].buff,
                    MAX_MSG_LEN);
            clients[client_idx].name[MAX_MSG_LEN] = '\0';
            consume_line(&clients[client_idx], nl);
          } else if (clients[client_idx].name_of_beloved[0] == '\0') {

            strncpy(clients[client_idx].name_of_beloved,
                    clients[client_idx].buff, MAX_MSG_LEN);

            clients[client_idx].name_of_beloved[MAX_MSG_LEN] = '\0';
            printf("%s wants to marry %s\n", clients[client_idx].name,
                   clients[client_idx].name_of_beloved);

            int partner_idx = find_partner_index(clients, client_idx);
            if (partner_idx >= 0) {
              perform_wedding(clients, client_idx, partner_idx);
              consume_line(&clients[client_idx], nl);
            } else {
              consume_line(&clients[client_idx], nl);
            }
          } else {
            if (clients[client_idx].partner >= 0) {
              relay_chat_line(clients, client_idx, nl);
            } else {
              consume_line(&clients[client_idx], nl);
            }
          }
        }
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  int timeout;
  if (skh_parse_positive_int(argv[1], &timeout) < 0) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  skh_ignore_sigpipe();

  int local_listen_socket = skh_unix_bind_listen(UNIX_SK_NAME, BACKLOG);
  if (local_listen_socket < 0)
    ERR("skh_unix_bind_listen");
  if (skh_set_nonblocking(local_listen_socket, 1) < 0)
    ERR("skh_set_nonblocking listen");

  doServer(local_listen_socket, timeout);

  close(local_listen_socket);
  if (skh_unlink_if_exists(UNIX_SK_NAME) < 0)
    ERR("unlink");

  return EXIT_SUCCESS;
}

#include "l7-common.h"
#include <errno.h>

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
#define MAX_EPOLL_EVENTS (MAX_CLIENTS + 1)

typedef struct client {
  int fd;
  char name[MAX_MSG_LEN + 1];
  char name_of_beloved[MAX_MSG_LEN + 1];
  char buff[MAX_MSG_LEN + 1];
  int buff_size;
} client_t;

void initialize_clients(client_t *clients) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    clients[i].fd = -1;
    clients[i].name[0] = '\0';
    clients[i].buff[0] = '\0';
    clients[i].name_of_beloved[0] = '\0';
    clients[i].buff_size = 0;
  }
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

void delete_client(client_t *clients, int idx) {
  if (idx < 0 || idx >= MAX_CLIENTS)
    return;
  if (clients[idx].fd != -1)
    close(clients[idx].fd);
  clients[idx].fd = -1;
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

void doServer(int local_listen_socket, int timeout) {
  int epoll_descriptor;
  if ((epoll_descriptor = epoll_create1(0)) < 0) {
    ERR("epoll_create:");
  }
  struct epoll_event event, events[MAX_EPOLL_EVENTS];
  event.events = EPOLLIN;
  event.data.fd = local_listen_socket;
  if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, local_listen_socket, &event) ==
      -1) {
    ERR("epoll_ctl");
  }
  int nfds;
  client_t clients[MAX_CLIENTS];
  initialize_clients(clients);

  while (1) {
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
          int new_client = add_new_client(local_listen_socket);
          if (new_client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;
            ERR("accept");
          }
          int flags = fcntl(new_client, F_GETFL) | O_NONBLOCK;
          if (fcntl(new_client, F_SETFL, flags) == -1)
            ERR("fcntl");

          printf("Another young person %d needs my help!\n", new_client);

          int client_idx = find_free_client_index(clients);
          if (client_idx < 0) {
            close(new_client);
            continue;
          }
          clients[client_idx].fd = new_client;
          event.data.fd = new_client;
          if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, new_client, &event) ==
              -1) {
            ERR("epoll_ctl");
          }
        }
      } else {
        int client_idx = find_client_index(clients, fd);
        if (client_idx < 0)
          continue;

        size_t space =
            sizeof(clients[client_idx].buff) - 1 - (size_t)clients[client_idx].buff_size;
        if (space == 0) {
          const char *dn = clients[client_idx].name[0] ? clients[client_idx].name
                                                         : "??";
          printf("I lost contact with %s\n", dn);
          delete_client(clients, client_idx);
          continue;
        }

        ssize_t bytes_read =
            read(fd, clients[client_idx].buff + clients[client_idx].buff_size, space);
        if (bytes_read < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
          ERR("read");
        }
        if (bytes_read == 0) {
          const char *dn = clients[client_idx].name[0] ? clients[client_idx].name
                                                         : "??";
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
            delete_client(clients, client_idx);
            break;
          } else {
            consume_line(&clients[client_idx], nl);
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

  int timeout = atoi(argv[1]);
  if (timeout < 1) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  sethandler(SIG_IGN, SIGPIPE);

  int local_listen_socket, new_flags;
  local_listen_socket = bind_local_socket(UNIX_SK_NAME, BACKLOG);
  new_flags = fcntl(local_listen_socket, F_GETFL) | O_NONBLOCK;
  fcntl(local_listen_socket, F_SETFL, new_flags);

  doServer(local_listen_socket, timeout);

  close(local_listen_socket);
  unlink(UNIX_SK_NAME);
  return EXIT_SUCCESS;
}

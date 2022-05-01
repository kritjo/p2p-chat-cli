#include <stdarg.h>
#include "network_utils.h"
#include "send_packet.h"
#include "common.h"

int cmp_addr_port(struct sockaddr_storage first, struct sockaddr_storage second) {
  char first_buf_addr[INET6_ADDRSTRLEN];
  char first_buf_port[7];
  char second_buf_addr[INET6_ADDRSTRLEN];
  char second_buf_port[7];
  get_addr(first, first_buf_addr, INET6_ADDRSTRLEN);
  get_port(first, first_buf_port);
  get_addr(second, second_buf_addr, INET6_ADDRSTRLEN);
  get_port(second, second_buf_port);
  return (strcmp(first_buf_addr, second_buf_addr) == 0 && strcmp(first_buf_port, second_buf_port) == 0) ? 1 : -1;
}

char *get_addr(struct sockaddr_storage addr, char *buf, size_t buflen) {
  if (addr.ss_family == AF_INET) {
    QUIT_ON_NULL("inet_ntop", inet_ntop(addr.ss_family, &((struct sockaddr_in *) &addr)->sin_addr, buf, buflen))
  } else {
    QUIT_ON_NULL("inet_ntop", inet_ntop(addr.ss_family, &((struct sockaddr_in6 *) &addr)->sin6_addr, buf, buflen))
  }
  return buf;
}

socklen_t get_addr_len(struct sockaddr_storage addr) {
  if (addr.ss_family == AF_INET) {
    return INET_ADDRSTRLEN;
  } else {
    return INET6_ADDRSTRLEN;
  }
}

char *get_port(struct sockaddr_storage addr, char *buf) {
  unsigned short port;
  if (addr.ss_family == AF_INET) {
    port = ntohs(((struct sockaddr_in *) &addr)->sin_port);
  } else {
    port = ntohs(((struct sockaddr_in6 *) &addr)->sin6_port);
  }
  if (sprintf(buf, "%hu", port) < 0) {
    fprintf(stderr, "spritntf() failed in get_port()\n");
    exit(EXIT_FAILURE);
  }
  return buf;
}

int get_bound_socket(struct addrinfo hints, char *name, char *service) {
  int rc; // Return code
  struct addrinfo *res, *curr_res;
  int curr_socket;
  if ((rc = getaddrinfo(name, service, &hints, &res)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    return -1;
  }

  // The resulting addrinfo struct res could have multiple Internet adresses
  // bind to the first valid
  for (curr_res = res; curr_res != NULL; curr_res = curr_res->ai_next) {
    if ((curr_socket = socket(
        curr_res->ai_family,
        curr_res->ai_socktype,
        curr_res->ai_protocol)
        ) == -1) {
      perror("socket");
      fprintf(stderr, "Could not aquire socket, trying next result.\n");
      continue; // If socket return error, try next.
    }

    if (bind(curr_socket, curr_res->ai_addr, curr_res->ai_addrlen) != -1)
      break; // Bound socket, exit loop.

    perror("bind");
    fprintf(stderr, "Could not bind socket, trying next result.\n");
    close(curr_socket);
  }

  freeaddrinfo(res);

  if (curr_res == NULL) {
    return -1;
  }
  return curr_socket;
}

size_t send_ack(int socketfd, struct sockaddr_storage addr, char *pkt_num, int num_args, ...) {
  va_list args;
  va_start(args, num_args);

  char **args_each = alloca(num_args * sizeof(char *));

  // Get total message length
  size_t msg_len = 5 + strlen(pkt_num); // "ACK 0"
  int n = num_args;
  while (n) {
    msg_len += 1; // Leading space
    args_each[num_args - n] = va_arg(args, char *);
    msg_len += strlen(args_each[num_args - n]);
    n--;
  }

  char msg[msg_len];
  strcpy(msg, "ACK ");
  strcat(msg, pkt_num);
  while (n < num_args) {
    strcat(msg, " ");
    strcat(msg, args_each[n]);
    n++;
  }

  return send_packet(socketfd, msg, msg_len, 0, (struct sockaddr *) &addr, get_addr_len(addr));
}

void print_err_from(char *msg, struct sockaddr_storage addr) {
  char addr_str[INET6_ADDRSTRLEN];
  char port_str[7];
  get_addr(addr, addr_str, INET6_ADDRSTRLEN);
  get_port(addr, port_str);
  fprintf(stderr, "Recived %s from: %s:%s\n", msg, addr_str, port_str);
}
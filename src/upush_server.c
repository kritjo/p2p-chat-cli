#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <search.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>

#include "send_packet.h"

#define MAX_CLIENTS 100
#define MAX_MSG 1460 // Longest msg can be 20 char + 2*nicklen + message
                     // Max message length is 1400.
                     // Assume that pkt num is 0 or 1.
                     // Nicklen is max 20 char

typedef struct {
    struct sockaddr_storage *addr;
    time_t *registered_time;
} nick_t;

int send_ack(struct sockaddr_storage addr, char *pkt_num, int num_args, ...);

char *get_addr(struct sockaddr_storage addr, char *buf, size_t buflen);

char *get_port(struct sockaddr_storage addr, char *buf, size_t buflen);

void handle_exit(void);

void handle_sigint();

int get_bound_socket(struct addrinfo hints, char *name, char *service);

void print_illegal_dram(struct sockaddr_storage addr);

socklen_t get_addr_len(struct sockaddr_storage addr);

int socketfd = 0;
char *nicks[MAX_CLIENTS];
int reg_clients = 0;
const char msg_delim = ' ';

int main(int argc, char **argv) {
  char *port, loss_probability;

  // Create a hash table to store client registrations
  if (!hcreate((size_t) MAX_CLIENTS)) {
    perror("hcreate");
  }

  signal(SIGINT, handle_sigint);

  // Check that we have enough and correct CLI arguments
  if (argc == 3) {
    port = argv[1];

    // Using strtol to avoid undefined behaviour
    long tmp;
    // TODO: DO NOT ASSUME THAT WE GET CORRECT PORT NUM
    tmp = strtol(argv[2], NULL, 10);
    // TODO: change <= to < on release
    if (0 <= tmp && tmp <= 100) {
      loss_probability = (char) tmp;
    } else {
      printf("Illegal loss probability. Enter a number between 0 and 100.\n");
      handle_exit();
      EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong
    }
  } else {
    printf("USAGE: %s <port> <loss_probability>\n", argv[0]);
    handle_exit();
    return EXIT_SUCCESS; // Return success as this is not an error, but expected without args.
  }

  set_loss_probability((float) loss_probability / 100.0f);

  // Make the addrinfo struct ready
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;
  socketfd = get_bound_socket(hints, NULL, port);
  if (socketfd == -1) {
    fprintf(stderr, "get_bound_socket() in main() failed.\n");
    handle_exit();
    return EXIT_FAILURE;
  }

  /*
   **************** MAIN LOOP ****************
   */
  printf("Press CTRL+c to quit.\n");

  char buf[MAX_MSG + 1]; // Incoming message buffer
  ssize_t bytes_received;
  socklen_t addrlen = sizeof(struct sockaddr_storage);
  while (1) {
    // Block and receive incoming connection
    struct sockaddr_storage incoming;
    bytes_received = recvfrom(
        socketfd,
        (void *) buf,
        MAX_MSG,
        0,
        (struct sockaddr *) &incoming, // Store the origin adr of incoming dgram
        &addrlen
    );

    switch (bytes_received) {
      case 0:
        // Zero length datagrams are allowed. This is not an error.
        continue;
      case -1:
        // -1 is error, and we should quit the server.
        perror("recvfrom");
        handle_exit();
        exit(EXIT_FAILURE);
      default:
        // We should not do anything otherwise, but default case is added to show that explicitly
        break;
    }

    buf[bytes_received] = '\0';

    // Get first part of msg, should be "PKT"
    char *msg_part = strtok(buf, &msg_delim);
    if (msg_part == NULL || strcmp(msg_part, "PKT") != 0) {
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    }

    // Get second part of msg, should be "0" or "1"
    msg_part = strtok(NULL, &msg_delim);
    if (msg_part == NULL ||
    (strcmp(msg_part, "0") != 0 && strcmp(msg_part, "1") != 0)) {
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    }

    // Packet number. The server part of the stop-and-wait is lazy. It interfaces as a stop-and-wait to the senders
    // (incoming connections), but it really does not care about packet numbers.
    // In case of a retransmission the server will just replace the old registration.
    char *pkt_num = msg_part;

    // The command from the user.
    enum command {
        REG, LOOKUP
    };
    enum command curr_command;
    // Get third part of msg, should be "REG" or "LOOKUP"
    msg_part = strtok(NULL, &msg_delim);
    if (msg_part == NULL) {
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    } else if (strcmp(msg_part, "REG") == 0) {
      curr_command = REG;
    } else if (strcmp(msg_part, "LOOKUP") == 0) {
      curr_command = LOOKUP;
    } else {
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    }

    // Get final part of msg, should be a nickname.
    msg_part = strtok(NULL, &msg_delim);
    size_t nick_len;
    if (msg_part == NULL || (nick_len = strlen(msg_part)) < 1 || nick_len > 20) {
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    }

    // Remove newline from the nickname (if there is one)
    char nick[nick_len];
    if (msg_part[nick_len - 1] == '\n') {
      strncpy(nick, msg_part, nick_len - 1);
      nick[nick_len - 1] = '\0';
    }

    // Check that the nick is legal. That is: only ascii characters and only alpha characters.
    for (size_t i = 0; i < nick_len; i++) {
      if (!isascii(nick[i]) || !isalpha(nick[i])) {
        // Illegal datagrams is expected so this is not an error.
        print_illegal_dram(incoming);
        continue;
      }
    }
    ENTRY e, *e_res;
    e.key = nick;
    e_res = hsearch(e, FIND);

    if (curr_command == REG) {
      // Check if nick exists, in that case, replace current address in table.
      if (e_res == NULL) {
        // Now we know that the nick does not exist.
        nick_t *curr_nick = malloc(sizeof(nick_t));
        curr_nick->registered_time = malloc(sizeof(time_t));
        curr_nick->addr = malloc(sizeof(struct sockaddr_storage));
        *curr_nick->addr = incoming;
        time(curr_nick->registered_time);

        if (*curr_nick->registered_time == (time_t) -1) {
          perror("Could not get system time");
          handle_exit();
          exit(EXIT_FAILURE);
        }

        e.key = malloc(sizeof(char *));
        strcpy(e.key, nick);
        e.data = curr_nick;
        e_res = hsearch(e, ENTER);

        if (e_res == NULL) {
          fprintf(stderr, "Nick table is full\n");
          free(curr_nick->registered_time);
          free(curr_nick);
          continue; // TODO: Should probably check if we are missing heartbeats.
        }

        // Registration completed send ACK
        if (send_ack(incoming, pkt_num, 1, "OK") == -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
        nicks[reg_clients++] = nick;
      } else {
        // Update the nick with the incoming adr and current time.
        // This implies that the code flow will end up here for both updates
        // to an entry from a new addr, and heartbeats.
        nick_t *curr_nick = (nick_t *) e_res->data;

        *curr_nick->addr = incoming;
        time(curr_nick->registered_time);
        if (*curr_nick->registered_time == (time_t) -1) {
          perror("Could not get system time");
          handle_exit();
          exit(EXIT_FAILURE);
        }
        if (send_ack(incoming, pkt_num, 1, "OK") == -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
      }
    } else {
      // We can be certain that we are now in lookup phase due to the binary possibilities of the conditional variable.
      if (e_res == NULL) {
        if (send_ack(incoming, pkt_num, 1, "NOT FOUND") == -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
        continue;
      }

      time_t current_time;
      time(&current_time);
      nick_t *curr_nick = (nick_t *) e_res->data;

      if (current_time - *curr_nick->registered_time > 30) {
        if (send_ack(incoming, pkt_num, 1, "NOT FOUND") == -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
        continue;
      }
      char addr_str[INET6_ADDRSTRLEN];
      char *port_str[6];
      if (send_ack(incoming,pkt_num, 5,
               "NICK",
               nick,
               get_addr(incoming, (char *) &addr_str, INET6_ADDRSTRLEN),
               "PORT",
               get_port(incoming, (char *) port_str, 6)) == -1) {
        fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
      }
    }
  }
}

void print_illegal_dram(struct sockaddr_storage addr) {
  char addr_str[INET6_ADDRSTRLEN];
  char *port_str[6];
  printf("Recived illegal datagram from: %s:%s\n",
         get_addr(addr, addr_str, INET6_ADDRSTRLEN),
         get_port(addr, (char *) port_str, 6));
}

int get_bound_socket(struct addrinfo hints, char* name, char *service) {
  int rc; // Return code
  struct addrinfo *res, *curr_res;
  int curr_socket;
  if ((rc = getaddrinfo(name, service, &hints, &res)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    handle_exit();
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

int send_ack(struct sockaddr_storage addr, char *pkt_num, int num_args, ...) {
  va_list args;
  va_start(args, num_args);

  char **args_each = alloca(num_args * sizeof(char *));

  // Get total message length
  size_t msg_len = 6; // "ACK 0"
  int n = num_args;
  while (n) {
    printf("adding arg\n");
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

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = addr.ss_family;
  hints.ai_socktype = SOCK_DGRAM;

  // Get socket for sending ack
  char addr_buf[INET6_ADDRSTRLEN];
  char port_buf[6];
  int send_sock = get_bound_socket(hints,
                                   get_addr(addr, addr_buf, INET6_ADDRSTRLEN),
                                   get_port(addr, port_buf, 6));
  if (send_sock == -1) {
    fprintf(stderr, "get_bound_socket() in send_ack() failed.\n");
    return -1;
  }

  send_packet(send_sock, msg, msg_len, 0, (struct sockaddr *) &addr, get_addr_len(addr));

  close(send_sock);
  return 0;
}

char *get_addr(struct sockaddr_storage addr, char *buf, size_t buflen) {
  if (addr.ss_family == AF_INET) {
    inet_ntop(addr.ss_family, &((struct sockaddr_in*) &addr)->sin_addr, buf, buflen);
  } else {
    inet_ntop(addr.ss_family, &((struct sockaddr_in6*) &addr)->sin6_addr, buf, buflen);
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

char *get_port(struct sockaddr_storage addr, char *buf, size_t buflen) {
  if (addr.ss_family == AF_INET) {
    inet_ntop(addr.ss_family, &((struct sockaddr_in*) &addr)->sin_port, buf, buflen);
  } else {
    inet_ntop(addr.ss_family, &((struct sockaddr_in6*) &addr)->sin6_port, buf, buflen);
  }
  return buf;
}

void handle_sigint() {
  handle_exit();
  exit(EXIT_SUCCESS);
}

void handle_exit(void) {
  if (socketfd != 0) close(socketfd);
  while (--reg_clients >= 0) {
    ENTRY e, *e_res;
    e.key = nicks[reg_clients];
    e_res = hsearch(e, FIND);
    if (e_res == NULL) {
      fprintf(stderr, "Could not find client that we expected to be in table!\n");
      continue; // Should continue to free as much memory as possible
    }
    nick_t *curr_nick = (nick_t *) e_res->data;
    free(curr_nick->registered_time);
    free(curr_nick->addr);
    free(curr_nick);
    free(e.key);
  }
  hdestroy();
}

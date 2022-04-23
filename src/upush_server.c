#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <ctype.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>

#include "send_packet.h"
#include "nick_node.h"
#include "network_utils.h"

#define MAX_MSG 1460 // Longest msg can be 20 char + 2*nicklen + message
                     // Max message length is 1400.
                     // Assume that pkt num is 0 or 1.
                     // Nicklen is max 20 char

size_t send_ack(struct sockaddr_storage addr, char *pkt_num, int num_args, ...);

void handle_exit(void);

void handle_sig_terminate(int sig);

int get_bound_socket(struct addrinfo hints, char *name, char *service);

void print_illegal_dram(struct sockaddr_storage addr);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void handle_sig_ignore(int sig) {}
#pragma GCC diagnostic pop

int socketfd = 0;

int main(int argc, char **argv) {
  char *port, loss_probability;

  // Handle termination signals gracefully in order to free memory
  signal(SIGINT, handle_sig_terminate);
  signal(SIGTERM, handle_sig_terminate);

  // Explicitly ignore unused USR signals
  signal(SIGUSR1, handle_sig_ignore);
  signal(SIGUSR2, handle_sig_ignore);

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

    if (bytes_received < 0) {
      // -1 is error, and we should quit the server.
      perror("recvfrom");
      handle_exit();
      exit(EXIT_FAILURE);
    } else if (bytes_received < 12) {
      // Zero length datagrams are allowed and not error.
      // Min valid format datagram is "PKT 0 REG a" this is likely due to transmission error, do not send ack.
      print_illegal_dram(incoming);
      continue;
    }

    buf[bytes_received] = '\0';

    char *msg_delim = " ";

    // Get first part of msg, should be "PKT"
    char *msg_part = strtok(buf, msg_delim);

    // On all checks, we test if msg_part is NULL first as strcmp declares that the parameters should not be null
    if (msg_part == NULL || strcmp(msg_part, "PKT") != 0) {
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    }

    // Get second part of msg, should be "0" or "1"
    msg_part = strtok(NULL, msg_delim);
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
    msg_part = strtok(NULL, msg_delim);
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
    msg_part = strtok(NULL, msg_delim);
    size_t nick_len = strlen(msg_part);
    if (msg_part == NULL || nick_len < 1 || nick_len > 20) {
      // Assume that clients should send well-formed nicknames, and if they do not, we will not send an ACK as it is
      // likely due to a transmission problem.
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    }

    // Remove newline from the nickname (if there is one)
    char nick[nick_len];
    if (msg_part[nick_len - 1] == '\n') {
      strncpy(nick, msg_part, nick_len - 1);
      nick[nick_len - 1] = '\0';
      nick_len--;
    }

    char legal_nick = 1;
    // Check that the nick is legal. That is: only ascii characters and only alpha characters.
    for (size_t i = 0; i < nick_len; i++) {
      if (!isascii(nick[i]) || !isalpha(nick[i]) || isdigit(nick[i])) {
        legal_nick = 0;
      }
    }
    if (!legal_nick) {
      // Assume that clients should send well-formed nicknames, and if they do not, we will not send an ACK as it is
      // likely due to a transmission problem.
      // Illegal datagrams is expected so this is not an error.
      print_illegal_dram(incoming);
      continue;
    }

    nick_node_t *result_node = find_nick_node(nick);

    if (curr_command == REG) {
      // Check if nick exists, in that case, replace current address in table.
      if (result_node == NULL) {
        // Now we know that the nick does not exist.
        nick_node_t *curr_nick = malloc(sizeof(nick_node_t));
        curr_nick->nick = malloc((nick_len + 1) * sizeof(char));
        strcpy(curr_nick->nick, nick);
        curr_nick->registered_time = malloc(sizeof(time_t));
        curr_nick->addr = malloc(sizeof(struct sockaddr_storage));
        *curr_nick->addr = incoming;
        time(curr_nick->registered_time);
        curr_nick->next = 0;
        curr_nick->prev = 0;

        if (*curr_nick->registered_time == (time_t) -1) {
          perror("Could not get system time");
          handle_exit();
          exit(EXIT_FAILURE);
        }


        if (insert_nick_node(curr_nick) == -1) {
          fprintf(stderr, "Nick table is full. Trying to clean.\n");
          delete_old_nick_nodes();

          if (insert_nick_node(curr_nick) == -1) {
            fprintf(stderr, "Nick table is full also after cleanup.\n");
            free_nick_node(curr_nick);
            continue;
          }
        }

        // Registration completed send ACK
        if (send_ack(incoming, pkt_num, 1, "OK") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
      } else {
        // Update the nick with the incoming adr and current time.
        // This implies that the code flow will end up here for both updates
        // to an entry from a new addr, and heartbeats.
        nick_node_t *curr_nick = find_nick_node(nick);

        *curr_nick->addr = incoming;
        time(curr_nick->registered_time);
        if (*curr_nick->registered_time == (time_t) -1) {
          perror("Could not get system time");
          handle_exit();
          exit(EXIT_FAILURE);
        }
        if (send_ack(incoming, pkt_num, 1, "OK") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
      }
    } else {
      // We can be certain that we are now in lookup phase due to the binary possibilities of the conditional variable.
      if (result_node == NULL) {
        if (send_ack(incoming, pkt_num, 1, "NOT FOUND") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
        continue;
      }

      time_t current_time;
      time(&current_time);

      if (current_time - *result_node->registered_time > TIMEOUT) {
        if (send_ack(incoming, pkt_num, 1, "NOT FOUND") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
        continue;
      }
      char addr_str[INET6_ADDRSTRLEN];
      char port_str[7];
      if (send_ack(incoming,pkt_num, 5,
               "NICK",
               nick,
               get_addr(incoming, (char *) &addr_str, INET6_ADDRSTRLEN),
               "PORT",
               get_port(incoming, (char *) port_str)) == (size_t) -1) {
        fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
      }
    }
  }
}

void print_illegal_dram(struct sockaddr_storage addr) {
  char addr_str[INET6_ADDRSTRLEN];
  char port_str[7];
  get_addr(addr, addr_str, INET6_ADDRSTRLEN);
  get_port(addr, port_str);
  printf("Recived illegal datagram from: %s:%s\n", addr_str, port_str);
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

size_t send_ack(struct sockaddr_storage addr, char *pkt_num, int num_args, ...) {
  va_list args;
  va_start(args, num_args);

  char **args_each = alloca(num_args * sizeof(char *));

  // Get total message length
  size_t msg_len = 6; // "ACK 0"
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

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = addr.ss_family;
  hints.ai_socktype = SOCK_DGRAM;

  return send_packet(socketfd, msg, msg_len, 0, (struct sockaddr *) &addr, get_addr_len(addr));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void handle_sig_terminate(int sig) {
#pragma GCC diagnostic pop
  handle_exit();
  exit(EXIT_SUCCESS);
}

void handle_exit(void) {
  if (socketfd != 0) close(socketfd);
  delete_all_nick_nodes();
}
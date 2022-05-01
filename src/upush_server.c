#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>

#include "upush_server.h"
#include "send_packet.h"
#include "linked_list.h"
#include "network_utils.h"
#include "common.h"

#define TIMEOUT 30
static int socketfd = 0;
static node_t **nick_head = NULL;

// UPS TODO: Mange forskjellige navn registrert på samme ip

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
    char *endptr = alloca(sizeof(char));
    tmp = strtol(argv[2], &endptr, 10);
    if (*endptr == '\0' && (0 <= tmp && tmp <= 100)) {
      loss_probability = (char) tmp;
      srand48(time(0)); // Seed the rand
      set_loss_probability((float) loss_probability / 100.0f);
    } else {
      printf("Illegal loss probability. Enter a number between 0 and 100 (inclusive).\n");
      handle_exit();
      return EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong num
    }
  } else {
    printf("USAGE: %s <port> <loss_probability>\n", argv[0]);
    handle_exit();
    return EXIT_SUCCESS; // Return success as this is not an error, but expected without args.
  }


  // Make the addrinfo struct ready. Do not use SO_REUSEADDR as the server should give error msg on port in use.
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV; // Fill out my ip for me, and explicitly specify that service arg is
                                                // a port number. This is done for added rigidity.
  socketfd = get_bound_socket(hints, NULL, port);
  if (socketfd == -1) {
    fprintf(stderr, "get_bound_socket() in main() failed.\n");
    handle_exit();
    return EXIT_FAILURE;
  }

  /*
   **************** MAIN LOOP ****************
   */
  nick_head = malloc(sizeof(node_t *));
  QUIT_ON_NULL("malloc", nick_head)
  *nick_head = NULL;
  printf("Press CTRL+c to quit.\n");

  if (signal(SIGALRM, handle_sig_alarm) == SIG_ERR) {
    fprintf(stderr, "signal() failed in main()\n");
    handle_exit();
    exit(EXIT_FAILURE);
  }
  alarm(TIMEOUT * 2); // Cleanup once a minute.

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

    if (bytes_received == -1) {
      // -1 is error, and we should quit the server.
      perror("recvfrom");
      handle_exit();
      exit(EXIT_FAILURE);
    } else if (bytes_received < 12) {
      // Zero length datagrams are allowed and not error.
      // Min valid format datagram is "PKT 0 REG a" this is likely due to transmission error, do not send ack.
      print_err_from("illegal datagram", incoming);
      continue;
    }

    buf[bytes_received] = '\0';

    char *msg_delim = " ";

    // Get first part of msg, should be "PKT"
    char *msg_part = strtok(buf, msg_delim);

    // On all checks, we test if msg_part is NULL first as strcmp declares that the parameters should not be null
    if (msg_part == NULL || strcmp(msg_part, "PKT") != 0) {
      print_err_from("illegal datagram", incoming);
      continue;
    }

    // Get second part of msg, should be "0" or "1"
    msg_part = strtok(NULL, msg_delim);
    if (msg_part == NULL ||
    (strcmp(msg_part, "0") != 0 && strcmp(msg_part, "1") != 0)) {
      print_err_from("illegal datagram", incoming);
      continue;
    }

    // Packet number. The server part of the stop-and-wait is lazy. It interfaces as a stop-and-wait to the senders
    // (incoming connections), but it really does not care about packet numbers.
    // In case of a retransmission the server will just replace the old registration.
    char *pkt_num = msg_part;

    // The command from the user.
    enum command { REG, LOOKUP };
    enum command curr_command;
    // Get third part of msg, should be "REG" or "LOOKUP"
    msg_part = strtok(NULL, msg_delim);
    if (msg_part == NULL) { print_err_from("illegal datagram", incoming); continue; }

    if (strcmp(msg_part, "REG") == 0) curr_command = REG;
    else if (strcmp(msg_part, "LOOKUP") == 0) curr_command = LOOKUP;
    else {
      print_err_from("illegal datagram", incoming);
      continue;
    }

    // Get final part of msg, should be a nickname.
    msg_part = strtok(NULL, msg_delim);
    size_t nick_len = strlen(msg_part);
    if (msg_part == NULL || nick_len < 1 || nick_len > 20) {
      // Assume that clients should send well-formed nicknames, and if they do not, we will not send an ACK as it is
      // likely due to a transmission problem.
      print_err_from("illegal datagram", incoming);
      continue;
    }

    if (!is_legal_nick(msg_part)) {
      // Assume that clients should send well-formed nicknames, and if they do not, we will not send an ACK as it is
      // likely due to a transmission problem.
      print_err_from("illegal datagram", incoming);
      continue;
    }

    node_t *result_node = find_node(nick_head, msg_part);

    if (curr_command == REG) {
      // Check if nick exists, in that case, replace current address in table.
      if (result_node == NULL) {
        // Now we know that the nick does not exist.
        nick_node_t *curr_nick = malloc(sizeof(nick_node_t));
        QUIT_ON_NULL("malloc", curr_nick)
        curr_nick->registered_time = malloc(sizeof(time_t));
        QUIT_ON_NULL("malloc", curr_nick->registered_time)
        curr_nick->addr = malloc(sizeof(struct sockaddr_storage));
        QUIT_ON_NULL("malloc", curr_nick->addr)
        *curr_nick->addr = incoming;
        QUIT_ON_MINUSONE("time", time(curr_nick->registered_time))

        if (*curr_nick->registered_time == (time_t) -1) {
          perror("Could not get system time");
          handle_exit();
          exit(EXIT_FAILURE);
        }

        insert_node(nick_head, msg_part, curr_nick);

        // Registration completed send ACK
        if (send_ack(socketfd, incoming, pkt_num, 1, "OK") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
      } else {
        // Update the nick with the incoming adr and current time.
        // This implies that the code flow will end up here for both updates
        // to an entry from a new addr, and heartbeats.
        nick_node_t *curr_nick = (nick_node_t *) result_node->data;

        *curr_nick->addr = incoming;
        QUIT_ON_MINUSONE("time", time(curr_nick->registered_time))
        if (*curr_nick->registered_time == (time_t) -1) {
          perror("Could not get system time");
          handle_exit();
          exit(EXIT_FAILURE);
        }
        if (send_ack(socketfd, incoming, pkt_num, 1, "OK") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
      }
    } else {
      // We can be certain that we are now in lookup phase due to the binary possibilities of the conditional variable.
      if (result_node == NULL) {
        if (send_ack(socketfd, incoming, pkt_num, 1, "NOT FOUND") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
        continue;
      }

      time_t current_time;
      if (time(&current_time) == (time_t) -1) {
        perror("time");
        handle_exit();
        exit(EXIT_FAILURE);
      }
      nick_node_t *curr_nick = (nick_node_t *) result_node->data;

      if (current_time - *curr_nick->registered_time > TIMEOUT) {
        if (send_ack(socketfd, incoming, pkt_num, 1, "NOT FOUND") == (size_t) -1) {
          fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
        }
        continue;
      }
      char addr_str[INET6_ADDRSTRLEN];
      char port_str[7];
      get_addr(*curr_nick->addr, (char *) &addr_str, INET6_ADDRSTRLEN);
      get_port(*curr_nick->addr, (char *) port_str);
      if (send_ack(socketfd, incoming,pkt_num, 5,
               "NICK",
               msg_part,
               addr_str,
               "PORT",
               port_str) == (size_t) -1) {
        fprintf(stderr, "send_ack() failed.\n"); // Is without side effects, so we can continue
      }
    }
  }
}

void handle_sig_terminate(__attribute__((unused)) int sig) {
  handle_exit();
  exit(EXIT_SUCCESS);
}

void handle_exit(void) {
  if (socketfd != 0) close(socketfd);
  delete_all_nodes(nick_head, free_nick_node);
  free(nick_head);
}

void free_nick_node(node_t *node) {
  nick_node_t *curr_nick = (nick_node_t *) node->data;
  free(curr_nick->registered_time);
  free(curr_nick->addr);
  free(curr_nick);
}

void handle_sig_alarm(__attribute__((unused)) int sig) {
  node_t *curr = *nick_head;
  time_t current_time;
  if (time(&current_time) == -1) {
    perror("time");
    handle_exit();
    exit(EXIT_FAILURE);
  }
  while (curr != NULL) {
    nick_node_t *node = curr->data;
    node_t *tmp = curr;
    curr = curr->next;
    if (current_time - *node->registered_time > TIMEOUT) {
      delete_node(nick_head, tmp, free_nick_node);
    }
  }
  alarm(TIMEOUT * 2);
}
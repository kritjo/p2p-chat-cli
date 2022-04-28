#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>

#include "upush_client.h"
#include "send_packet.h"
#include "network_utils.h"
#include "nick_node_client.h"
#include "send_node.h"
#include "real_time.h"
#include "utils.h"

static int socketfd;
static char *heartbeat_msg;
static struct sockaddr_storage server;

void register_with_server();

static recv_node_t *first_recv_node = NULL;
static char *my_nick;
static nick_node_t server_node;
static long timeout;
static block_node_t *first_blocked = NULL;

// TODO: Maybe add SA_RESTART flag to recvfrom

int main(int argc, char **argv) {
  char *server_addr, *server_port, loss_probability;

  if (argc == 6) {
    my_nick = argv[1];

    // Check that the nick is legal. That is: only ascii characters and only alpha characters. Max len 20 char
    if (!is_legal_nick(my_nick)) {
      printf("Illegal nickname, only ascii alpha characters are allowed. Max len 20.\n");
    }

    server_addr = argv[2];
    server_port = argv[3];

    timeout = strtol(argv[4], NULL, 10);
    if (timeout < 0 || UINT_MAX < timeout) { // used in alarm that takes unsigned int input
      printf("Illegal timeout. Has to be 0 or larger, up to %ud\n", UINT_MAX);
      return EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong num
    }

    // Using strtol to avoid undefined behaviour
    long tmp;
    // TODO: DO NOT ASSUME THAT WE GET CORRECT PORT NUM
    tmp = strtol(argv[5], NULL, 10);
    // TODO: change <= to < on release
    if (0 <= tmp && tmp <= 100) {
      loss_probability = (char) tmp;
      srand48(time(0)); // Seed the probability
      set_loss_probability((float) loss_probability / 100.0f);
    } else {
      printf("Illegal loss probability. Enter a number between 0 and 100.\n");
      return EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong num
    }

  } else {
    printf("USAGE: %s <nick> <address> <port> <timeout> <loss_probability>\n", argv[0]);
    return EXIT_SUCCESS; // Return success as this is not an error, but expected without args.
  }

  // Initialize hints used to get socket
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV; // Fill client ip, and specify NUMERICSERV explicitly as we will use
  // "0" service.

  socketfd = get_bound_socket(hints, NULL, "0");
  if (socketfd == -1) {
    fprintf(stderr, "get_bound_socket() in main() failed.\n");
    return EXIT_FAILURE;
  }

  // Make reg message. We only want to do this once as it is frequently used by heartbeat
  size_t msg_len = 11 + strlen(my_nick);
  char msg[msg_len];
  sprintf(msg, "PKT 0 REG %s", my_nick);
  heartbeat_msg = malloc(msg_len * sizeof(char));
  if (heartbeat_msg == NULL) {
    fprintf(stderr, "malloc() failed in main()\n");
    exit(EXIT_FAILURE);
  }
  strcpy(heartbeat_msg, msg);

  // Initialize hints and getaddrinfo of server to store in sockaddr_storage.
  struct addrinfo server_hints, *server_res;
  memset(&server_hints, 0, sizeof(server_hints));
  server_hints.ai_family = AF_UNSPEC;
  server_hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_NUMERICSERV;

  int rc;
  if ((rc = getaddrinfo(server_addr, server_port, &server_hints, &server_res)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    return -1;
  }

  memset(&server, 0, sizeof(server));
  memcpy(&server, server_res->ai_addr, server_res->ai_addrlen);
  freeaddrinfo(server_res);

  register_with_server();

  // Create a file descriptor timer for heartbeat. This is very useful in the select operation below as it does not
  // trigger signal that errors select. Also it is better than select timeout as interval is constant and not dependent
  // on how long time until the loop restarts.
  int heartbeatfd = timerfd_create(CLOCK_REALTIME, 0);
  struct itimerspec timespec;
  memset(&timespec, 0, sizeof(timespec));
  timespec.it_value.tv_sec = 10;
  timespec.it_interval.tv_sec = 10;
  timerfd_settime(heartbeatfd, 0, &timespec, NULL);

  // Create a signal file descriptor and mask out SIGUSR1. The timeoutfd will be set when SIGUSR1 signal is recieved
  // this lets us create a separate timer for each packet that we want timeout from.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  sigprocmask(SIG_BLOCK, &mask, NULL);
  int timeoutfd = signalfd(-1, &mask, 0);


  // Initialize the server send_node, we only need one of this so it can be constant to save cycles.
  server_node.type = SERVER;
  server_node.available_to_send = 1;
  server_node.next_pkt_num = "0";
  server_node.addr = &server;
  server_node.lookup_node = NULL;

  /*
   **************** MAIN LOOP ****************
   */
  char buf[MAX_MSG + 1];
  ssize_t bytes_received;
  socklen_t addrlen = sizeof(struct sockaddr_storage);
  struct sockaddr_storage incoming;
  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(socketfd, &fds);
    FD_SET(heartbeatfd, &fds);
    FD_SET(timeoutfd, &fds);

    if ((select(FD_SETSIZE, &fds, NULL, NULL, 0)) == -1) {
      perror("select");
      break;
    }

    if (FD_ISSET(heartbeatfd, &fds)) {
      // Extract signal from heartbeatfd to clear
      uint64_t ign;
      read(heartbeatfd, &ign, sizeof(uint64_t));
      handle_heartbeat();
    }

    if (FD_ISSET(timeoutfd, &fds)) {
      // Extract signal from timeoutfd to clear
      struct signalfd_siginfo info;
      read(timeoutfd, &info, sizeof(struct signalfd_siginfo));
      usr1_sigval_t *signal_ptr = (usr1_sigval_t *) info.ssi_ptr;
      // If do_not_honour is set, we have already received an ack. This is used to avoid race conditions between incoming
      // ACK and signal. E.g. signal is sent by timer while we do logic to check incoming ACK.
      if (signal_ptr->do_not_honour == 0) {
        send_node(signal_ptr->timed_out_send_node);
      }
    }

    if (FD_ISSET(socketfd, &fds)) {
      // Got message from socket
      bytes_received = recvfrom(
          socketfd,
          (void *) buf,
          MAX_MSG,
          0,
          (struct sockaddr *) &incoming, // Store the origin adr of incoming dgram
          &addrlen
      );
      buf[bytes_received] = '\0';

      if (bytes_received < 0) {
        // -1 is error, and we should quit the server.
        perror("recvfrom");
        break;
      } else if (bytes_received == 0) {
        // Zero length datagrams are allowed and not error.
        continue;
      }

      char *msg_delim = " ";

      // Get first part of msg, should be "PKT"
      char *msg_part = strtok(buf, msg_delim);

      // On all checks, we test if msg_part is NULL first as strcmp declares that the parameters should not be null
      if (msg_part == NULL) {
        continue;
      } else if (strcmp(msg_part, "PKT") == 0) {
        handle_pkt(msg_delim, incoming);
      } else if (strcmp(msg_part, "ACK") == 0) {
        handle_ack(msg_delim, incoming);
      } else {
        continue;
      }
    }

    if (FD_ISSET(STDIN_FILENO, &fds)) {
      int c;
      int count = 0;
      // Check that all characters are ascii. Right now we only ignore those that are not
      while ((c = getchar()) != EOF && c != '\n' && count < 1421) {
        if (isascii(c)) {
          buf[count++] = (char) c;
        }
      }
      buf[count] = '\0';

      if (strcmp(buf, "QUIT") == 0) {
        free(heartbeat_msg);
        close(socketfd);
        close(heartbeatfd);
        free_recv_nodes();
        delete_all_nick_nodes();
        delete_all_send_nodes();
        return EXIT_SUCCESS;
      }

      if (buf[0] == 'B' &&
          buf[1] == 'L' &&
          buf[2] == 'O' &&
          buf[3] == 'C' &&
          buf[4] == 'K' &&
          buf[5] == ' ') {
        if (!is_legal_nick(&buf[6])) {
          printf("Illegal nick.\n");
          continue;
        }
        insert_block_node(&buf[6]);
      } else if (buf[0] == 'U' &&
                 buf[1] == 'N' &&
                 buf[2] == 'B' &&
                 buf[3] == 'L' &&
                 buf[4] == 'O' &&
                 buf[5] == 'C' &&
                 buf[6] == 'K' &&
                 buf[7] == ' ') {
        if (!is_legal_nick(&buf[8])) {
          printf("Illegal nick.\n");
          continue;
        }
        delete_blocked(&buf[8]);
      }

      // Illegal command check
      if (buf[0] != '@') {
        printf("Illegal command received\n");
        continue;
      }

      // Extract nick and message. Not used strtok to avoid editing the buf.
      int startmsg = 1;
      char nick[21];
      while (startmsg < 22) {
        if (buf[startmsg] == ' ') break;
        nick[startmsg - 1] = buf[startmsg];
        startmsg++;
      }
      nick[startmsg - 1] = '\0';

      if (buf[startmsg] != ' ' || startmsg == 1) {
        continue; // Illegal nick if it does not end with ' ' or is zero-length
      }

      if (is_blocked(nick) == 1) continue;

      // Alloc place for the message so that it is persistent
      char *new_msg = malloc(strlen(&buf[startmsg + 1]) + 1 * sizeof(char));
      strcpy(new_msg, &buf[startmsg + 1]);

      // See if we already have a nick_node in the cache or if we need to do a new lookup.
      nick_node_t *search_result = find_nick_node(nick);
      if (search_result == NULL) {
        new_lookup(nick, startmsg, new_msg);
        continue;
      }

      // We got a successful search result, maybe send if client is ready
      // Add msg to back of queue
      add_msg(search_result, new_msg);
      // If client is available to send to search_result, add client to send_node and send msg
      next_msg(search_result);
    }
  }

  free(heartbeat_msg);
  close(socketfd);
  close(heartbeatfd);
  free_recv_nodes();
  return EXIT_FAILURE;
}

void register_with_server() {
  // Send the registration messsage and set alarm for timeout.
  handle_heartbeat();
  signal(SIGALRM, handle_sig_alarm);
  alarm(timeout);

  // Enter loop to recieve inital ack from server
  while (1) {
    ssize_t bytes_received;
    struct sockaddr_storage incoming;
    char buf[MAX_MSG];
    socklen_t addrlen = sizeof(struct sockaddr_storage);
    bytes_received = recvfrom(
        socketfd,
        (void *) buf,
        MAX_MSG,
        0,
        (struct sockaddr *) &incoming, // Store the origin adr of incoming dgram
        &addrlen
    );
    buf[bytes_received] = '\0';

    if (bytes_received < 0) {
      // -1 is error, and we should quit the client.
      perror("recvfrom");
      exit(EXIT_FAILURE);
    }
    // Then check if the msg we got comes from the server.
    if (cmp_addr_port(incoming, server) == -1) {
      // Got incoming bytes from other than server. Ignore.
      continue;
    }
    // Then check correct format "ACK 0 OK"
    if (bytes_received < 9 || strcmp(buf, "ACK 0 OK") != 0) {
      // Got too few incoming bytes. Ignore.
      continue;
    }
    // Got correct ACK. Cancel alarm.
    alarm(0);
    signal(SIGALRM, SIG_DFL);
    break;
  }
}

void new_lookup(char nick[21], int startmsg, char *new_msg) {
  // Lookup
  char *nick_persistant = malloc(startmsg * sizeof(char));
  strcpy(nick_persistant, nick);

  // Create a new nick node for the lookup, this is not inserted in linked list until we have gotten ip adr and
  // port from server.
  nick_node_t *new_nick_node = malloc(sizeof(nick_node_t));
  new_nick_node->nick = nick_persistant;
  new_nick_node->msg_to_send = NULL;

  // Create a send node for the message that the user wants to send
  send_node_t *new_send_node = malloc(sizeof(send_node_t));

  // We should free the pkt num when this send node is destroyed as it will be malloced
  // (as opposed to std "0"/"1" pkt nums)
  new_send_node->should_free_pkt_num = 1;

  // Add a pointer to the new nick node that should be further initialized upon receiving ip adr and port
  new_send_node->nick_node = new_nick_node;

  // Set the pkt num to the current time to get unique value for this transaction
  long time_s = time(0);
  char *long_buf = malloc(256 * sizeof(char));
  snprintf(long_buf, 256, "%ld", time_s);
  new_send_node->pkt_num = long_buf;

  new_send_node->msg = new_msg;
  new_send_node->type = MSG;

  // WAIT_INIT is not used for anything currently, except as an assurance that this will not be sent until lookup
  // is complete. (This should never happen).
  new_send_node->num_tries = WAIT_INIT;

  // Alloc and create a new timer, do not set timeout yet. The same timer will be used throughout the send node's
  // lifetime.
  new_send_node->timeout_timer = malloc(sizeof(usr1_sigval_t));
  new_send_node->timeout_timer->timed_out_send_node = new_send_node;
  register_usr1_custom_sig(new_send_node->timeout_timer);

  insert_send_node(new_send_node);

  queue_lookup(new_nick_node, 0);
  next_lookup();
}

void next_msg(nick_node_t *node) {
  if (node == NULL) return;
  if (node->available_to_send == 1) {
    if (node->msg_to_send == NULL) return;
    node->available_to_send = 0;

    // Create a new send node for this message
    send_node_t *new_node = malloc(sizeof(send_node_t));
    new_node->nick_node = node;

    // pkt num is string literal and should not be freed!
    new_node->should_free_pkt_num = 0;

    new_node->pkt_num = node->next_pkt_num;
    node->next_pkt_num = strcmp(node->next_pkt_num, "1") == 0 ? "0" : "1";

    new_node->msg = pop_msg(node);
    new_node->num_tries = 0;
    new_node->type = MSG;

    // Alloc and create a new timer, do not set timeout yet. The same timer will be used throughout the send node's
    // lifetime.
    new_node->timeout_timer = malloc(sizeof(usr1_sigval_t));
    new_node->timeout_timer->timed_out_send_node = new_node;
    register_usr1_custom_sig(new_node->timeout_timer);
    insert_send_node(new_node);

    // Send the message
    send_node(new_node);
  }
}

void next_lookup() {
  if (server_node.available_to_send == 1) {
    if (server_node.lookup_node == NULL) return;
    server_node.available_to_send = 0;
    send_node_t *new_node = malloc(sizeof(send_node_t));

    // Pkt num should not be freed as it is string literal
    new_node->should_free_pkt_num = 0;
    new_node->pkt_num = server_node.next_pkt_num;
    server_node.next_pkt_num = strcmp(server_node.next_pkt_num, "1") == 0 ? "0" : "1";

    new_node->next = NULL;
    new_node->prev = NULL;

    lookup_node_t *lookup = pop_lookup(&server_node);
    new_node->nick_node = lookup->waiting_node;
    new_node->msg = lookup->nick;
    free(lookup);

    new_node->num_tries = 0;
    new_node->type = LOOKUP;

    // Alloc and create a new timer, do not set timeout yet. The same timer will be used throughout the send node's
    // lifetime.
    new_node->timeout_timer = malloc(sizeof(usr1_sigval_t));
    new_node->timeout_timer->timed_out_send_node = new_node;
    register_usr1_custom_sig(new_node->timeout_timer);

    // Insert send node in linked list and send the lookup msg
    insert_send_node(new_node);
    send_node(new_node);
  }
}

void queue_lookup(nick_node_t *node, int callback) {
  if (callback) {
    add_lookup(&server_node, node->nick, node);
  } else {
    add_lookup(&server_node, node->nick, NULL);
  }
}

void send_node(send_node_t *node) {
  if (node->type == LOOKUP) {
    send_lookup(node);
  } else {
    send_msg(node);
  }
}

void send_msg(send_node_t *node) {
  if ((node->num_tries >= 0 && node->num_tries < 2) || (node->num_tries >= RE_0 && node->num_tries < RE_2)) {
    node->num_tries++;

    unsigned long pkt_len =
        20 + strlen(node->pkt_num) + strlen(my_nick) + strlen(node->nick_node->nick) + strlen(node->msg);
    char pkt[pkt_len];
    sprintf(pkt, "PKT %s FROM %s TO %s MSG %s", node->pkt_num, my_nick, node->nick_node->nick, node->msg);

    // Send message and set timeout for ack
    send_packet(socketfd, pkt, pkt_len, 0, (struct sockaddr *) node->nick_node->addr,
                get_addr_len(*node->nick_node->addr));
    set_time_usr1_timer(node->timeout_timer, timeout);
    node->timeout_timer->do_not_honour = 0;

  } else if (node->num_tries == DO_NEW_LOOKUP) {
    // If we have tried 2 times unsuccsessfully, try new lookup with callback to this when done.
    node->num_tries = WAIT_FOR_LOOKUP;
    queue_lookup(node->nick_node, 1);
    next_lookup();
    printf("Doing new lookup\n");

  } else if (node->num_tries == WAIT_FOR_LOOKUP || node->num_tries == WAIT_INIT) {
    // Do nothing if we have WAIT states.

  } else {
    // Too many tries
    // Discard msg and get next if any.
    node->nick_node->available_to_send = 1;
    free(node->msg);
    delete_send_node(node);
    next_lookup();
  }
}

void send_lookup(send_node_t *node) {
  // LOOKUP TYPE
  if (node->num_tries < 2) {
    node->num_tries++;

    unsigned long pkt_len = 13 + strlen(node->msg) + 1;
    char pkt[pkt_len];
    sprintf(pkt, "PKT %s LOOKUP %s", node->pkt_num, node->msg);

    send_packet(socketfd, pkt, pkt_len, 0, (struct sockaddr *) &server, get_addr_len(server));

    // Set timer to timeout and say that we should honour it
    set_time_usr1_timer(node->timeout_timer, timeout);
    node->timeout_timer->do_not_honour = 0;
  } else {
    printf("NICK %s NOT REACHABLE\n", node->msg);

    // Cleanup send and nick nodes
    char nick[strlen(node->msg)];
    strcpy(nick, node->msg);

    for (int i = 0; i < 2; i++) { // Do twice to get both LOOKUP and possible MSG nodes
      send_node_t *search_send = find_send_node(nick);

      if (search_send != NULL && search_send->type == MSG && search_send->num_tries == -1) {
        free(search_send->nick_node->nick);
        free(search_send->nick_node);
      }

      if (search_send != NULL) {
        // If this is a MSG type, free the MSG as it will never get sent.
        if (search_send->type == MSG) {
          free(search_send->msg);
        }
        delete_send_node(search_send);
      }
    }

    nick_node_t *search = find_nick_node(nick);
    if (search != NULL) delete_nick_node(search);
    server_node.available_to_send = 1;

    next_lookup();
  }
}

void handle_ack(char *msg_delim, struct sockaddr_storage incoming) {
  // Get pkt num
  char *msg_part = strtok(NULL, msg_delim);
  char pkt_num[256];
  strcpy(pkt_num, msg_part);

  // Get ack type
  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL) { return; }
  if (strcmp(msg_part, "OK") == 0) {
    handle_ok_ack(incoming);
  } else if (strcmp(msg_part, "WRONG") == 0) {
    handle_wrong_ack(incoming, msg_delim);
  } else if (strcmp(msg_part, "NICK") == 0) {
    handle_nick_ack(incoming, msg_delim, pkt_num);
  } else if (strcmp(msg_part, "NOT") == 0) {
    handle_not_ack();
  }
}

void handle_not_ack() {
  // Find the lookup node that failed, print error and continue with next if there are any
  send_node_t *curr = find_send_node(NULL);
  while (curr != NULL) {
    if (curr->type == LOOKUP) break;
    curr = curr->next;
  }
  if (curr == NULL) fprintf(stderr, "Could not find lookup node!\n");
  else {
    printf("NICK %s NOT REGISTERED\n", curr->msg);
    delete_send_node(curr);
    server_node.available_to_send = 1;
    next_lookup();
  }
}

void handle_nick_ack(struct sockaddr_storage incoming, char *msg_delim, char pkt_num[256]) {
  // Get the nick, the IP and the port and insert to cache. Check if there are any dependency nodes that we should "wake"
  send_node_t *curr = find_send_node(NULL);
  while (curr != NULL) {
    if (curr->type == LOOKUP) {
      break;
    }
    curr = curr->next;
  }
  if (curr == NULL) {
    fprintf(stderr, "Could not find lookup node to ack\n");
    return;
  }
  if (strcmp(curr->pkt_num, pkt_num) != 0) {
    return; // Discard wrong ACK.
  }

  // Get nick
  char *msg_part = strtok(NULL, msg_delim);
  if (strcmp(msg_part, curr->msg) != 0) {
    return; // Discard wrong NICK.
  }

  // Cancel timer. We are now certain that it is correct ack.
  set_time_usr1_timer(curr->timeout_timer, 0);
  curr->timeout_timer->do_not_honour = 1;

  // Save nick for later use.
  char *nick = malloc((strlen(msg_part) + 1) * sizeof(char));
  strcpy(nick, msg_part);

  // Get addr
  msg_part = strtok(NULL, msg_delim);
  struct sockaddr_storage addr;
  memset(&addr, 0, sizeof(struct sockaddr_storage));
  int addrfam;
  if (strchr(msg_part, ':')) {
    addrfam = AF_INET6;
  } else {
    addrfam = AF_INET;
  }

  char tmp_addr[INET6_ADDRSTRLEN];
  strcpy(tmp_addr, msg_part);

  // Get port
  msg_part = strtok(NULL, msg_delim);
  if (strcmp(msg_part, "PORT") != 0) {
    free(nick);
    return;
  }
  msg_part = strtok(NULL, msg_delim);

  // Get the sockaddr_storage initialized
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = addrfam;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

  int rc;
  if ((rc = getaddrinfo(tmp_addr, msg_part, &hints, &res)) != 0) {
    fprintf(stderr, "Got illegal address/port: %s.\n", gai_strerror(rc));
    free(nick);
    return;
  }

  addr.ss_family = addrfam;
  memcpy(&addr, res->ai_addr, res->ai_addrlen);

  // TODO: Not sure what this does, deletes send_node. Why??
  char should_delete = 1;
  // If this is new LOOKUP for existing cache
  if (curr->nick_node != NULL) {
    free(nick);
    should_delete = 0;
    nick_node_t *new_node = curr->nick_node;
    *new_node->addr = addr;
    new_node->type = CLIENT;
    new_node->available_to_send = 1;

    send_node_t *notify = find_send_node(NULL);
    while (notify != NULL) {
      if (curr->nick_node == notify->nick_node) {
        break;
      }
      notify = notify->next;
    }
    if (notify == NULL) {
      fprintf(stderr, "Could not find expected notify node.\n");
      return;
    }
    notify->num_tries = RE_0;
    send_node(notify);
  } else {
    send_node_t *curr_n = find_send_node(NULL);
    while (curr_n != NULL) {
      if (curr_n->type == MSG && strcmp(curr_n->nick_node->nick, nick) == 0) {
        break;
      }
      curr_n = curr_n->next;
    }
    free(nick);
    if (curr_n == NULL) {
      fprintf(stderr, "Could not find expected notify node.\n");
      return;
    }
    nick_node_t *new_node = curr_n->nick_node;
    new_node->addr = malloc(sizeof(struct sockaddr_storage));
    *new_node->addr = addr;
    new_node->type = CLIENT;
    new_node->available_to_send = 0;
    new_node->msg_to_send = NULL;
    new_node->next_pkt_num = "1";
    insert_nick_node(new_node);

    curr_n->num_tries = 0;
    send_node(curr_n);
  }

  if (should_delete) {
    delete_send_node(curr);
  }
  server_node.available_to_send = 1;
  next_lookup();
}

void handle_wrong_ack(struct sockaddr_storage incoming, char *msg_delim) {
  char *msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL) { return; }
  send_node_t *curr = find_send_node(NULL);
  while (curr != NULL) {
    if (cmp_addr_port(*curr->nick_node->addr, incoming) == 1) break;
    curr = curr->next;
  }
  if (curr == NULL) {
    fprintf(stderr, "Could not find lookup node.\n");
  }
  fprintf(stderr, "Sent illegal %s to %s\n", msg_part, curr->nick_node->nick);
  curr->nick_node->available_to_send = 1;
  nick_node_t *curr_n = curr->nick_node;
  delete_send_node(curr);
  next_msg(curr_n);
}

void handle_ok_ack(struct sockaddr_storage storage) {
  char server_ack = 0; // State holder 0 = not from server, 1 = from server
  if (cmp_addr_port(storage, server) == 1) server_ack = 1;
  if (server_ack == 1) return; // Ignore registration OK

  // Find the node to ack
  send_node_t *curr = find_send_node(NULL);
  while (curr != NULL) {
    if (curr->type == MSG && cmp_addr_port(*curr->nick_node->addr, storage) == 1) {
      break;
    }
    curr = curr->next;
  }
  if (curr == NULL) {
    fprintf(stderr, "Could not find lookup node to ack\n");
    return;
  }

  // Cancel timer
  set_time_usr1_timer(curr->timeout_timer, 0);
  curr->timeout_timer->do_not_honour = 1;
  nick_node_t *curr_n = curr->nick_node;
  free(curr->msg);
  delete_send_node(curr);
  curr_n->available_to_send = 1;

  // Check if there are more messages queued
  next_msg(curr_n);
  return;

}

void handle_pkt(char *msg_delim, struct sockaddr_storage incoming) {
  char *msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL) {
    return;
  }

  char pkt_num[256];
  strcpy(pkt_num, msg_part);

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, "FROM") != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  size_t nick_len = strlen(msg_part);
  if (msg_part == NULL || nick_len < 1 || nick_len > 20) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    return;
  }

  char nick[nick_len + 1];
  strcpy(nick, msg_part);
  if (!is_legal_nick(nick)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    return;
  }
  if (is_blocked(nick) == 1) return;

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, "TO") != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, my_nick) != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG NAME");
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, "MSG") != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    return;
  }

  msg_part = strtok(NULL, ""); // Get the rest
  if (msg_part == NULL || 1400 < strlen(msg_part)) {
    return;
  }

  recv_node_t *recv_node = find_or_insert_recv_node(nick);

  // If this is the first ever message or a new init msg, save stamp.
  if (strcmp(pkt_num, "0") != 0 && strcmp(pkt_num, "1") != 0) {
    recv_node->expected_msg = "1";
    // TODO: Should probably check endptr
    long new_stamp = strtol(pkt_num, NULL, 10);
    if (recv_node->stamp != new_stamp) printf("%s: %s\n", nick, msg_part); // If this is not a retransmit
    recv_node->stamp = new_stamp;
    send_ack(socketfd, incoming, pkt_num, 1, "OK");
    return;
  }


  // Send ack if this is previous message but only print if this is new msg.
  send_ack(socketfd, incoming, pkt_num, 1, "OK");
  if (strcmp(pkt_num, recv_node->expected_msg) == 0) {
    printf("%s: %s\n", nick, msg_part);
    recv_node->expected_msg = strcmp(pkt_num, "0") == 0 ? "1" : "0";
  }
}

recv_node_t *find_recv_node(char *nick) {
  recv_node_t *curr = first_recv_node;
  while (curr != NULL) {
    if (strcmp(curr->nick, nick) == 0) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
}

recv_node_t *find_or_insert_recv_node(char *nick) {
  recv_node_t *curr = find_recv_node(nick);
  if (curr != NULL) return curr;
  if (first_recv_node == NULL) {
    first_recv_node = malloc(sizeof(recv_node_t));
    first_recv_node->nick = nick;
    first_recv_node->expected_msg = "-1";
    first_recv_node->stamp = 0;
    first_recv_node->next = NULL;
    return first_recv_node;
  }
  curr = malloc(sizeof(recv_node_t));
  curr->nick = nick;
  curr->expected_msg = "-1";
  first_recv_node->stamp = 0;
  curr->next = first_recv_node;
  first_recv_node = curr;
  return curr;
}

void free_recv_nodes(void) {
  recv_node_t *curr = first_recv_node;
  recv_node_t *tmp;
  while (curr != NULL) {
    tmp = curr;
    curr = curr->next;
    free(tmp);
  }
}

void handle_heartbeat() {
  send_packet(socketfd, heartbeat_msg, strlen(heartbeat_msg) + 1, 0, (struct sockaddr *) &server, get_addr_len(server));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void handle_sig_alarm(int sig) {
#pragma GCC diagnostic pop
  printf("Timeout. Did not get ACK from server on registration.\n");
  free(heartbeat_msg);
  exit(EXIT_SUCCESS); // This is not an error in the program.
}

void insert_block_node(char *nick) {
  if (is_blocked(nick) == 1) return;
  block_node_t *blocked = malloc(sizeof(block_node_t));
  blocked->nick = malloc(strlen(nick) + 1);
  strcpy(blocked->nick, nick);

  if (first_blocked == NULL) {
    first_blocked = blocked;
    first_blocked->next = NULL;
    first_blocked->prev = NULL;
    return;
  }

  first_blocked->prev = blocked;
  blocked->next = first_blocked;
  first_blocked = blocked;
}

char is_blocked(char *nick) {
  block_node_t *blocked = first_blocked;
  while (blocked != NULL) {
    if (strcmp(blocked->nick, nick) == 0) return 1;
    blocked = blocked->next;
  }
  return 0;
}

void delete_blocked(char *nick) {
  block_node_t *blocked = first_blocked;
  while (blocked != NULL) {
    if (strcmp(blocked->nick, nick) == 0) break;
    blocked = blocked->next;
  }
  if (blocked == NULL) return;

  if (blocked->prev == 0) {
    first_blocked = blocked->next;
    if (first_blocked != NULL) first_blocked->prev = 0;
    free(blocked->nick);
    free(blocked);
  } else if (blocked->next == 0) {
    blocked->prev->next = 0;
    free(blocked->nick);
    free(blocked);
  } else {
    blocked->prev->next = blocked->next;
    blocked->next->prev = blocked->prev;
    free(blocked->nick);
    free(blocked);
  }
}
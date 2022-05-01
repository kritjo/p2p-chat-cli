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
#include "real_time.h"
#include "common.h"
#include "linked_list.h"

static int socketfd;
static int heartbeatfd;
static int exitsigfd;
static int timeoutfd;

static char *heartbeat_msg;
static struct sockaddr_storage server;
static nick_node_t server_node;
static char *my_nick;
static long timeout;

static node_t **blocked_head = NULL;
static node_t **recv_head = NULL;
static node_t **send_head = NULL;
static node_t **nick_head = NULL;
static node_t **timer_head = NULL;

int main(int argc, char **argv) {
  char *server_addr, *server_port, loss_probability;

  if (argc == 6) {
    my_nick = argv[1];

    // Check that the nick is legal. That is: only ascii characters and only alpha characters. Max len 20 char
    if (!is_legal_nick(my_nick)) {
      printf("Illegal nickname, only ascii alpha characters are allowed. Max len 20.\n");
      return EXIT_SUCCESS;
    }

    server_addr = argv[2];
    server_port = argv[3];

    char *endptr = alloca(sizeof(char));
    timeout = strtol(argv[4], &endptr, 10);
    if (*endptr != '\0' || timeout < 0 || UINT_MAX < timeout) { // used in alarm that takes unsigned int input
      printf("Illegal timeout. Has to be 0 or larger, up to %ud\n", UINT_MAX);
      return EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong num
    }

    // Using strtol to avoid undefined behaviour
    long tmp;
    tmp = strtol(argv[5], &endptr, 10);
    if (*endptr == '\0' && (0 <= tmp && tmp <= 100)) {
      loss_probability = (char) tmp;
      srand48(time(0)); // Seed the probability
      set_loss_probability((float) loss_probability / 100.0f);
    } else {
      printf("Illegal loss probability. Enter a number between 0 and 100 (inclusive).\n");
      return EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong num
    }

  } else {
    printf("USAGE: %s <nick> <address> <port> <timeout> <loss_probability>\n", argv[0]);
    return EXIT_SUCCESS; // Return success as this is not an error, but expected without args.
  }

  // Initialize hints and getaddrinfo of server to store in sockaddr_storage.
  struct addrinfo server_hints, *server_res;
  memset(&server_hints, 0, sizeof(server_hints));
  server_hints.ai_family = AF_UNSPEC;
  server_hints.ai_socktype = SOCK_DGRAM;
  server_hints.ai_flags = AI_NUMERICSERV;

  int rc;
  if ((rc = getaddrinfo(server_addr, server_port, &server_hints, &server_res)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    return -1;
  }

  // Set static global struct to keep track of server address
  memset(&server, 0, sizeof(server));
  memcpy(&server, server_res->ai_addr, server_res->ai_addrlen);
  freeaddrinfo(server_res);

  // Initialize hints used to get socket
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = MY_SOCK_TYPE;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV; // Fill client ip, and specify NUMERICSERV explicitly as we will use
  // "0" service.

  socketfd = get_bound_socket(hints, NULL, "0");
  QUIT_ON_MINUSONE("get_bound_socket", socketfd)

  // Make reg message. We only want to do this once as it is frequently used by heartbeat
  size_t msg_len = 11 + strlen(my_nick);
  char msg[msg_len];
  if (sprintf(msg, "PKT 0 REG %s", my_nick) < 0) {
    fprintf(stderr, "sprintf() failed in main()\n");
    exit(EXIT_FAILURE);
  }
  heartbeat_msg = malloc(msg_len * sizeof(char));
  QUIT_ON_NULL("malloc", heartbeat_msg)

  // Handle termination signals gracefully in order to free memory
  if (signal(SIGINT, handle_sig_terminate_pre_reg) == SIG_ERR) {
    perror("signal");
    exit(EXIT_FAILURE);
  }
  if (signal(SIGTERM, handle_sig_terminate_pre_reg) == SIG_ERR) {
    perror("signal");
    exit(EXIT_FAILURE);
  }

  // Explicitly ignore unused USR signals, SIGUSR1 is used.
  if (signal(SIGUSR2, handle_sig_ignore) == SIG_ERR) {
    perror("signal");
    exit(EXIT_FAILURE);
  }

  strcpy(heartbeat_msg, msg);

  register_with_server();

  // Create a file descriptor timer for heartbeat. This is very useful in the select operation below as it does not
  // trigger signal that errors select. Also it is better than select timeout as interval is constant and not dependent
  // on how long time until the loop restarts.
  heartbeatfd = timerfd_create(CLOCK_REALTIME, 0);
  QUIT_ON_MINUSONE("timerfd_create", heartbeatfd)
  struct itimerspec timespec;
  memset(&timespec, 0, sizeof(timespec));
  timespec.it_value.tv_sec = 10;
  timespec.it_interval.tv_sec = 10;
  QUIT_ON_MINUSONE("timerfd_settime", timerfd_settime(heartbeatfd, 0, &timespec, NULL))

  // Create a signal file descriptor and usr_set out SIGUSR1. The timeoutfd will be set when SIGUSR1 signal is recieved
  // this lets us create a separate timer for each packet that we want timeout from.
  sigset_t usr_set;
  sigemptyset(&usr_set);
  sigaddset(&usr_set, SIGUSR1);
  sigprocmask(SIG_BLOCK, &usr_set, NULL);
  timeoutfd = signalfd(-1, &usr_set, 0);

  // Create a signal file descriptor to catch SIGINT and SIGTERM
  sigset_t term_set;
  QUIT_ON_MINUSONE("sigemptyset", sigemptyset(&term_set))
  QUIT_ON_MINUSONE("sigaddset", sigaddset(&term_set, SIGINT))
  QUIT_ON_MINUSONE("sigaddset", sigaddset(&term_set, SIGTERM))
  QUIT_ON_MINUSONE("sigprocmask", sigprocmask(SIG_BLOCK, &term_set, NULL))
  exitsigfd = signalfd(-1, &term_set, 0);
  QUIT_ON_MINUSONE("signalfd", exitsigfd);


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

  blocked_head = malloc(sizeof(node_t *));
  QUIT_ON_NULL("malloc", blocked_head)
  *blocked_head = NULL;

  recv_head = malloc(sizeof(node_t *));
  QUIT_ON_NULL("malloc", recv_head)
  *recv_head = NULL;

  send_head = malloc(sizeof (node_t *));
  QUIT_ON_NULL("malloc", send_head)
  *send_head = NULL;

  nick_head = malloc(sizeof (node_t *));
  QUIT_ON_NULL("malloc", nick_head)
  *nick_head = NULL;

  timer_head = malloc(sizeof (node_t *));
  QUIT_ON_NULL("malloc", timer_head)
  *timer_head = NULL;

  // Handle termination signals gracefully in order to free memory
  if (signal(SIGINT, handle_sig_terminate) == SIG_ERR) {
    perror("signal");
    exit(EXIT_FAILURE);
  }
  if (signal(SIGTERM, handle_sig_terminate) == SIG_ERR) {
    perror("signal");
    exit(EXIT_FAILURE);
  }
  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(socketfd, &fds);
    FD_SET(heartbeatfd, &fds);
    FD_SET(timeoutfd, &fds);
    FD_SET(exitsigfd, &fds);

    if ((select(FD_SETSIZE, &fds, NULL, NULL, 0)) == -1) {
      perror("select");
      break;
    }

    // SIGINT or SIGTERM
    if (FD_ISSET(exitsigfd, &fds)) {
      handle_exit(EXIT_SUCCESS);
    }

    if (FD_ISSET(heartbeatfd, &fds)) {
      // Extract signal from heartbeatfd to clear
      uint64_t ign;
      read(heartbeatfd, &ign, sizeof(uint64_t));
      handle_heartbeat();
    }

    // SIGUSR1
    if (FD_ISSET(timeoutfd, &fds)) {
      // Extract signal from timeoutfd to clear
      struct signalfd_siginfo info;
      read(timeoutfd, &info, sizeof(struct signalfd_siginfo));
      usr1_sigval_t *signal_ptr = (usr1_sigval_t *) info.ssi_ptr;
      // If do_not_honour is set, we have already received an ack. This is used to avoid race conditions between incoming
      // ACK and signal. E.g. signal is sent by timer while we do logic to check incoming ACK.
      if (signal_ptr->do_not_honour == 0) {
        send_node(signal_ptr->timed_out_send_node);
      } else {
        delete_node_by_key(timer_head, signal_ptr->id, free_timer_node);
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

      HANDLE_EXIT_ON_MINUS_ONE("recvfrom", bytes_received);
      if (bytes_received == 0) {
        // Zero length datagrams are allowed and not error.
        continue;
      }

      char *msg_delim = " ";

      // Get first part of msg, should be "PKT"
      char *msg_part = strtok(buf, msg_delim);

      // On all checks, we test if msg_part is NULL first as strcmp declares that the parameters should not be null
      if (msg_part == NULL) { continue; }

      if (strcmp(msg_part, "PKT") == 0) {
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
        handle_exit(EXIT_SUCCESS);
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
        if (!find_node(blocked_head, &buf[6])) {
          insert_node(blocked_head, &buf[6], (void *) 1);
        }
        continue;
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
        delete_node_by_key(blocked_head, &buf[8], NULL);
        continue;
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

      if (find_node(blocked_head, nick)) continue;

      // Alloc place for the message so that it is persistent
      char *new_msg = malloc(strlen(&buf[startmsg + 1]) + 1 * sizeof(char));
      QUIT_ON_NULL("malloc", new_msg)
      strcpy(new_msg, &buf[startmsg + 1]);

      // See if we already have a nick_node in the cache or if we need to do a new lookup.
      node_t *search = find_node(nick_head, nick);
      if (search == NULL) {
        new_lookup(nick, startmsg, new_msg);
        continue;
      }

      nick_node_t *search_result = (nick_node_t *) search->data;
      // We got a successful search result, maybe send if client is ready
      // Add msg to back of queue
      add_msg(search_result, new_msg);
      // If client is available to send to search, add client to send_node and send msg
      next_msg(search_result);
    }
  }

  handle_exit(EXIT_FAILURE);
}

void register_with_server() {
  // Send the registration messsage and set alarm for timeout.
  handle_heartbeat();
  if (signal(SIGALRM, handle_sig_alarm) == SIG_ERR) {
    perror("signal");
    exit(EXIT_FAILURE);
  }
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

    QUIT_ON_MINUSONE("recvfrom", bytes_received);

    // Then check correct format "ACK 0 OK"
    if (bytes_received < 9 || strcmp(buf, "ACK 0 OK") != 0) {
      // Got too few incoming bytes. Ignore.
      continue;
    }
    // Got correct ACK. Cancel alarm.
    alarm(0);
    if (signal(SIGALRM, SIG_DFL) == SIG_ERR) {
      perror("signal");
      exit(EXIT_FAILURE);
    }
    break;
  }
}

void new_lookup(char nick[21], int startmsg, char *new_msg) {
  // Lookup
  char *nick_persistant = malloc(startmsg * sizeof(char));
  QUIT_ON_NULL("malloc", nick_persistant)
  strcpy(nick_persistant, nick);

  // Create a new nick node for the lookup, this is not inserted in linked list until we have gotten ip adr and
  // port from server.
  nick_node_t *new_nick_node = malloc(sizeof(nick_node_t));
  QUIT_ON_NULL("malloc", new_nick_node)
  new_nick_node->nick = nick_persistant;
  new_nick_node->msg_to_send = NULL;

  // Create a send node for the message that the user wants to send
  send_node_t *new_send_node = malloc(sizeof(send_node_t));
  QUIT_ON_NULL("malloc", new_send_node)

  // We should free the pkt num when this send node is destroyed as it will be malloced
  // (as opposed to std "0"/"1" pkt nums)
  new_send_node->should_free_pkt_num = 1;

  // Add a pointer to the new nick node that should be further initialized upon receiving ip adr and port
  new_send_node->nick_node = new_nick_node;

  // Set the pkt num to the current time to get unique value for this transaction
  long time_s = time(0);
  char *long_buf = malloc(256 * sizeof(char));
  QUIT_ON_NULL("malloc", long_buf)
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
  QUIT_ON_NULL("malloc", new_send_node->timeout_timer)
  new_send_node->timeout_timer->id = malloc(256);
  QUIT_ON_NULL("malloc", new_send_node->timeout_timer->id)
  snprintf(new_send_node->timeout_timer->id, 256, "%ld", time(0));
  new_send_node->timeout_timer->timed_out_send_node = new_send_node;
  register_usr1_custom_sig(new_send_node->timeout_timer);
  insert_node(timer_head, new_send_node->timeout_timer->id, new_send_node->timeout_timer);

  insert_node(send_head, new_nick_node->nick, new_send_node);

  queue_lookup(new_nick_node, 0, 0);
  next_lookup();
}

// Send next message from node's queue if any.
void next_msg(nick_node_t *node) {
  if (node == NULL) return;
  if (node->available_to_send == 1) {
    if (node->msg_to_send == NULL) return;
    node->available_to_send = 0;

    // Create a new send node for this message
    send_node_t *new_node = malloc(sizeof(send_node_t));
    QUIT_ON_NULL("malloc", new_node)
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
    QUIT_ON_NULL("malloc", new_node->timeout_timer)
    new_node->timeout_timer->id = malloc(256);
    QUIT_ON_NULL("malloc", new_node->timeout_timer->id)
    snprintf(new_node->timeout_timer->id, 256, "%ld", time(0));
    new_node->timeout_timer->timed_out_send_node = new_node;
    register_usr1_custom_sig(new_node->timeout_timer);
    insert_node(timer_head, new_node->timeout_timer->id, new_node->timeout_timer);

    insert_node(send_head, node->nick, new_node);

    // Send the message
    send_node(new_node);
  }
}

// Send next lookup from server queue if there is one
void next_lookup() {
  if (server_node.available_to_send == 1) {
    if (server_node.lookup_node == NULL) return;
    server_node.available_to_send = 0;
    send_node_t *new_node = malloc(sizeof(send_node_t));
    QUIT_ON_NULL("malloc", new_node)

    // Pkt num should not be freed as it is string literal
    new_node->should_free_pkt_num = 0;
    new_node->pkt_num = server_node.next_pkt_num;
    server_node.next_pkt_num = strcmp(server_node.next_pkt_num, "1") == 0 ? "0" : "1";

    lookup_node_t *lookup = pop_lookup(&server_node);
    new_node->nick_node = lookup->waiting_node;
    new_node->msg = lookup->nick;
    free(lookup);

    new_node->num_tries = 0;
    new_node->type = LOOKUP;

    // Alloc and create a new timer, do not set timeout yet. The same timer will be used throughout the send node's
    // lifetime.
    new_node->timeout_timer = malloc(sizeof(usr1_sigval_t));
    QUIT_ON_NULL("malloc", new_node->timeout_timer)
    new_node->timeout_timer->id = malloc(256);
    QUIT_ON_NULL("malloc", new_node->timeout_timer->id)
    snprintf(new_node->timeout_timer->id, 256, "%ld", time(0));
    new_node->timeout_timer->timed_out_send_node = new_node;
    register_usr1_custom_sig(new_node->timeout_timer);
    insert_node(timer_head, new_node->timeout_timer->id, new_node->timeout_timer);

    // Insert send node in linked list and send the lookup msg
    insert_node(send_head, new_node->msg, new_node);
    send_node(new_node);
  }
}

void queue_lookup(nick_node_t *node, int callback, int free_node) {
  if (callback) {
    add_lookup(&server_node, node->nick, node);
  } else {
    add_lookup(&server_node, node->nick, NULL);
  }
  if (free_node == 1) free(node);
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
    if (sprintf(pkt, "PKT %s FROM %s TO %s MSG %s", node->pkt_num, my_nick, node->nick_node->nick, node->msg)
    < 0) {
      fprintf(stderr, "sprintf() failed in send_msg\n");
      handle_exit(EXIT_FAILURE);
    }

    // Send message and set timeout for ack
    send_packet(socketfd, pkt, pkt_len, 0, (struct sockaddr *) node->nick_node->addr,
                get_addr_len(*node->nick_node->addr));
    set_time_usr1_timer(node->timeout_timer, timeout);
    node->timeout_timer->do_not_honour = 0;

  } else if (node->num_tries == DO_NEW_LOOKUP) {
    // If we have tried 2 times unsuccsessfully, try new lookup with callback to this when done.
    node->num_tries = WAIT_FOR_LOOKUP;
    queue_lookup(node->nick_node, 1, 0);
    next_lookup();
    printf("Doing new lookup\n");
  } else if (node->num_tries == WAIT_FOR_LOOKUP || node->num_tries == WAIT_INIT) {
    // Do nothing if we have WAIT states.
  } else {
    // Too many tries
    // Discard msg and get next if any.
    node->nick_node->available_to_send = 1;
    delete_node_by_key(send_head, node->nick_node->nick, free_send);
    next_lookup();
  }
}

void send_lookup(send_node_t *node) {
  // LOOKUP TYPE
  if (node->num_tries < 2) {
    node->num_tries++;

    unsigned long pkt_len = 13 + strlen(node->msg) + 1;
    char pkt[pkt_len];
    if (sprintf(pkt, "PKT %s LOOKUP %s", node->pkt_num, node->msg) < 0) {
      fprintf(stderr, "sprintf() failed in send_lookup()\n");
      handle_exit(EXIT_FAILURE);
    }

    send_packet(socketfd, pkt, pkt_len, 0, (struct sockaddr *) &server, get_addr_len(server));

    // Set timer to timeout and say that we should honour it
    set_time_usr1_timer(node->timeout_timer, timeout);
    node->timeout_timer->do_not_honour = 0;
  } else {
    fprintf(stderr, "NICK %s UNREACHABLE\n", node->msg);

    // Cleanup send and nick nodes
    char nick[strlen(node->msg)];
    strcpy(nick, node->msg);

    for (int i = 0; i < 2; i++) { // Do twice to get both LOOKUP and possible MSG nodes
      node_t *search = find_node(send_head, nick);
      if (search == NULL) continue;
      send_node_t *search_send = search->data;

      if (search_send != NULL && search_send->type == MSG && search_send->num_tries == -1) {
        free(search_send->nick_node->nick);
        free(search_send->nick_node);
      }

      if (search_send != NULL) {
        search_send->timeout_timer->do_not_honour = 1;
        // If this is a MSG type, free the MSG as it will never get sent.
        delete_node(send_head, search, free_send);
      }
    }

    node_t *search = find_node(nick_head, nick);
    if (search != NULL) delete_node(nick_head, search, free_nick);
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
    handle_nick_ack(msg_delim, pkt_num);
  } else if (strcmp(msg_part, "NOT") == 0) {
    handle_not_ack();
  }
}

void handle_not_ack() {
  // Find the lookup node that failed, print error and continue with next if there are any
  node_t *curr = *send_head;
  while (curr != NULL) {
    if (((send_node_t *)curr->data)->type == LOOKUP) break;
    curr = curr->next;
  }
  if (curr == NULL) fprintf(stderr, "Could not find lookup node!\n");
  else {
    send_node_t *node = ((send_node_t *)curr->data);
    fprintf(stderr, "NICK %s NOT REGISTERED\n", node->msg);
    char nick[strlen(node->msg)+1];
    strcpy(nick, node->msg);
    node->timeout_timer->do_not_honour = 1;
    delete_node(send_head, curr, free_send);
    send_node_t *msg_node = find_node(send_head, nick)->data;
    free(msg_node->nick_node->nick);
    free(msg_node->nick_node);
    delete_node_by_key(send_head, nick, free_send);
    server_node.available_to_send = 1;
    next_lookup();
  }
}

void handle_nick_ack(char *msg_delim, char pkt_num[256]) {
  // Get the nick, the IP and the port and insert to cache. Check if there are any dependency nodes that we should "wake"
  node_t *search = *send_head;
  while (search != NULL) {
    if (((send_node_t *)search->data)->type == LOOKUP) {
      break;
    }
    search = search->next;
  }
  if (search == NULL) {
    fprintf(stderr, "Could not find lookup node to ack\n");
    return;
  }
  send_node_t *curr = ((send_node_t *)search->data);

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
  QUIT_ON_NULL("malloc", nick)
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
  freeaddrinfo(res);

  // If this is new LOOKUP for existing cache
  if (curr->nick_node != NULL) {
    free(nick);
    nick_node_t *new_node = curr->nick_node;
    *new_node->addr = addr;
    new_node->type = CLIENT;
    new_node->available_to_send = 1;

    node_t *notify = *send_head;
    while (notify != NULL) {
      if (((send_node_t *) notify->data)->type == MSG &&
      curr->nick_node == ((send_node_t *) notify->data)->nick_node) {
        break;
      }
      notify = notify->next;
    }
    if (notify == NULL) {
      fprintf(stderr, "Could not find expected notify node.\n");
      return;
    }
    ((send_node_t *) notify->data)->num_tries = RE_0;
    send_node( ((send_node_t *) notify->data));
  } else {
    node_t *curr_n = *send_head;
    while (curr_n != NULL) {
      if ( ((send_node_t *) curr_n->data)->type == MSG &&
      strcmp(((send_node_t *) curr_n->data)->nick_node->nick, nick) == 0) {
        break;
      }
      curr_n = curr_n->next;
    }
    if (curr_n == NULL) {
      fprintf(stderr, "Could not find expected notify node.\n");
      return;
    }
    nick_node_t *new_node = ((send_node_t *) curr_n->data)->nick_node;
    new_node->addr = malloc(sizeof(struct sockaddr_storage));
    QUIT_ON_NULL("malloc", new_node->addr)
    *new_node->addr = addr;
    new_node->type = CLIENT;
    new_node->available_to_send = 0;
    new_node->msg_to_send = NULL;
    new_node->next_pkt_num = "1";
    insert_node(nick_head, nick, new_node);
    free(nick);

    ((send_node_t *) curr_n->data)->num_tries = 0;
    send_node(((send_node_t *) curr_n->data));
  }

  send_node_t *node = ((send_node_t *)search->data);
  node->timeout_timer->do_not_honour = 1;
  delete_node(send_head, search, free_send);

  server_node.available_to_send = 1;
  next_lookup();
}

void handle_wrong_ack(struct sockaddr_storage incoming, char *msg_delim) {
  char *msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL) { return; }
  node_t *curr = *send_head;
  while (curr != NULL) {
    if (cmp_addr_port(*((send_node_t *) curr->data)->nick_node->addr, incoming) == 1) break;
    curr = curr->next;
  }
  if (curr == NULL) {
    fprintf(stderr, "Could not find lookup node.\n");
    return;
  }
  fprintf(stderr, "Sent illegal %s to %s\n", msg_part, ((send_node_t *) curr->data)->nick_node->nick);
  send_node_t *node = ((send_node_t *)curr->data);
  node->nick_node->available_to_send = 1;
  node->timeout_timer->do_not_honour = 1;
  nick_node_t *curr_n = node->nick_node;
  delete_node(send_head, curr, free_send);
  next_msg(curr_n);
}

void handle_ok_ack(struct sockaddr_storage storage) {
  // Find the node to ack
  node_t *curr = *send_head;
  while (curr != NULL) {
    if (((send_node_t *) curr->data)->type == MSG && cmp_addr_port(*((send_node_t *) curr->data)->nick_node->addr, storage) == 1) {
      break;
    }
    curr = curr->next;
  }
  if (curr == NULL) {
    // Server registrations will fall through here
    return;
  }

  // Cancel timer
  set_time_usr1_timer(((send_node_t *) curr->data)->timeout_timer, 0);
  nick_node_t *curr_n = ((send_node_t *) curr->data)->nick_node;
  ((send_node_t *) curr->data)->timeout_timer->do_not_honour = 1;
  delete_node(send_head, curr, free_send);
  curr_n->available_to_send = 1;

  // Check if there are more messages queued
  next_msg(curr_n);
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
    print_err_from("message with wrong format", incoming);
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  size_t nick_len = strlen(msg_part);
  if (msg_part == NULL || nick_len < 1 || nick_len > 20) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    print_err_from("message with wrong format", incoming);
    return;
  }

  char nick[nick_len + 1];
  strcpy(nick, msg_part);
  if (!is_legal_nick(nick)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    print_err_from("message with wrong format", incoming);
    return;
  }

  if (find_node(blocked_head, nick)) return;

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, "TO") != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    print_err_from("message with wrong format", incoming);
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, my_nick) != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG NAME");
    print_err_from("message with wrong name", incoming);
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, "MSG") != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    print_err_from("message with wrong format", incoming);
    return;
  }

  msg_part = strtok(NULL, ""); // Get the rest
  if (msg_part == NULL || 1400 < strlen(msg_part)) {
    return;
  }

  node_t *recv = find_node(recv_head, nick);
  recv_node_t *recv_node;

  if (recv == NULL) {
    recv_node = malloc(sizeof(recv_node_t));
    QUIT_ON_NULL("malloc", recv_node)
    recv_node->expected_msg = "-1";
    recv_node->stamp = 0;
    insert_node(recv_head, nick, recv_node);
  } else {
    recv_node = (recv_node_t *) recv->data;
  }

  // If this is the first ever message or a new init msg, save stamp.
  if (strcmp(pkt_num, "0") != 0 && strcmp(pkt_num, "1") != 0) {
    recv_node->expected_msg = "1";
    char *endptr = alloca(sizeof(char));
    long new_stamp = strtol(pkt_num, &endptr, 10);
    if (*endptr != '\0' && *pkt_num != '\0') { fprintf(stderr, "Illegal pkt_num!\n"); return; }

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

void handle_heartbeat() {
  send_packet(socketfd, heartbeat_msg, strlen(heartbeat_msg) + 1, 0, (struct sockaddr *) &server, get_addr_len(server));
}

void add_msg(nick_node_t *node, char *msg) {
  message_node_t *new_message = malloc(sizeof(message_node_t));
  QUIT_ON_NULL("malloc", new_message)
  new_message->message = msg;
  new_message->next = NULL;
  if (node->msg_to_send == NULL) {
    node->msg_to_send = new_message;
    return;
  }
  message_node_t *curr = node->msg_to_send;
  while (curr->next != NULL) curr = curr->next;
  curr->next = new_message;
}

char *pop_msg(nick_node_t *node) {
  char *msg = node->msg_to_send->message;
  if (msg == NULL) return NULL;
  message_node_t *tmp = node->msg_to_send;
  node->msg_to_send = node->msg_to_send->next;
  free (tmp);
  return msg;
}

lookup_node_t *pop_lookup(nick_node_t *node) {
  lookup_node_t *popped = node->lookup_node;
  if (popped == NULL) return NULL;
  node->lookup_node = node->lookup_node->next;
  return popped;
}

void add_lookup(nick_node_t *node, char *nick, nick_node_t *waiting) {
  lookup_node_t *new_lookup = malloc(sizeof(lookup_node_t));
  QUIT_ON_NULL("malloc", new_lookup)
  new_lookup->nick = nick;
  new_lookup->waiting_node = waiting;
  new_lookup->next = NULL;
  if (node->lookup_node == NULL) {
    node->lookup_node = new_lookup;
    return;
  }
  lookup_node_t *curr = node->lookup_node;
  while (curr->next != NULL) curr = curr->next;
  curr->next = new_lookup;
}

void free_send(node_t *node) {
  send_node_t *send_node = (send_node_t *) node->data;
  if (send_node->should_free_pkt_num) {
    free(send_node->pkt_num);
  }
  if (send_node->type == MSG) {
    free(send_node->msg);
  }
  free(send_node);
}

void free_nick(node_t *node) {
  nick_node_t *nick_node = (nick_node_t *) node->data;
  if (nick_node->type == SERVER) {
    lookup_node_t *curr = nick_node->lookup_node;
    while (curr != NULL) {
      lookup_node_t *tmp = curr->next;
      free(curr->nick);
      free(curr);
      curr = tmp;
    }
  } else {
    message_node_t *curr = nick_node->msg_to_send;
    while (curr != NULL) {
      message_node_t *tmp = curr->next;
      free(curr->message);
      free(curr);
      curr = tmp;
    }
  }
  free(nick_node->nick);
  free(nick_node->addr);
  free(nick_node);
}

void free_timer_node(node_t *node) {
  usr1_sigval_t *timer_node = (usr1_sigval_t *) node->data;
  free(timer_node->id);
  unregister_usr_1_custom_sig(timer_node);
}

void handle_sig_alarm(__attribute__((unused)) int sig) {
  fprintf(stderr,"Timeout. Did not get ACK from server on registration.\n");
  free(heartbeat_msg);
  exit(EXIT_SUCCESS); // This is not an error in the program.
}

void handle_sig_terminate(__attribute__((unused)) int sig) {
  handle_exit(EXIT_SUCCESS);
}

void handle_sig_terminate_pre_reg(__attribute__((unused)) int sig) {
  free(heartbeat_msg);
  close(socketfd);
  exit(EXIT_SUCCESS);
}

void handle_exit(int status) {
  close(timeoutfd);
  close(exitsigfd);
  close(socketfd);
  close(heartbeatfd);
  delete_all_nodes(recv_head, FREE_DATA);
  delete_all_nodes(blocked_head, NULL);
  delete_all_nodes(send_head, free_send);
  delete_all_nodes(nick_head, free_nick);
  delete_all_nodes(timer_head, free_timer_node);
  free(heartbeat_msg);
  free(blocked_head);
  free(recv_head);
  free(send_head);
  free(nick_head);
  free(timer_head);
  exit(status);
}
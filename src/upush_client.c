#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <sys/timerfd.h>

#include "upush_client.h"
#include "send_packet.h"
#include "network_utils.h"
#include "nick_node_client.h"
#include "send_node.h"

#define MAX_MSG 1460

typedef struct recv_node {
    char *nick;
    char *expected_msg;
    struct recv_node *next;
} recv_node_t;

void handle_sig_alarm(int sig);
recv_node_t *find_or_insert_recv_node(char *nick);
recv_node_t *find_recv_node(char *nick);
void handle_pkt(char *msg_delim, struct sockaddr_storage incoming);
void handle_heartbeat();

void handle_ack(char *msg_delim, struct sockaddr_storage incoming);
void free_recv_nodes(void);

void send_msg(send_node_t *node);

static int socketfd;
static char *heartbeat_msg;
static struct sockaddr_storage server;
static recv_node_t *first_recv_node = NULL;
static char *my_nick;
static nick_node_t server_node;

int main(int argc, char **argv) {
  char *server_addr, *server_port, loss_probability;
  long timeout;

  if (argc == 6) {
    my_nick = argv[1];

    // Check that the nick is legal. That is: only ascii characters and only alpha characters. Max len 20 char
    size_t nick_len = strlen(my_nick);
    char legal_nick = 1;
    if (20 < nick_len) legal_nick = 0;
    else {
      for (size_t i = 0; i < nick_len; i++) {
        if (!isascii(my_nick[i]) || !isalpha(my_nick[i]) || isdigit(my_nick[i])) {
          legal_nick = 0;
        }
      }
    }
    if (!legal_nick) {
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
      set_loss_probability((float) loss_probability / 100.0f);
    } else {
      printf("Illegal loss probability. Enter a number between 0 and 100.\n");
      return EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong num
    }

  } else {
    printf("USAGE: %s <nick> <address> <port> <timeout> <loss_probability>\n", argv[0]);
    return EXIT_SUCCESS; // Return success as this is not an error, but expected without args.
  }

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

  // Make reg message
  size_t msg_len = 11 + strlen(my_nick);
  char msg[msg_len];
  strcpy(msg, "PKT 0 REG ");
  strcat(msg, my_nick);
  // Save it to be used in heartbeat msg
  heartbeat_msg = malloc(msg_len * sizeof(char));
  if (heartbeat_msg == NULL) {
    fprintf(stderr, "malloc() failed in main()\n");
    exit(EXIT_FAILURE);
  }
  strcpy(heartbeat_msg, msg);

  struct addrinfo server_hints, *server_res;
  memset(&server_hints, 0, sizeof (server_hints));
  server_hints.ai_family = AF_UNSPEC;
  server_hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_NUMERICSERV;

  int rc;
  if ((rc = getaddrinfo(server_addr, server_port, &server_hints, &server_res)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    return -1;
  }

  // Convert result to sockaddr_storage in server
  memset(&server, 0, sizeof(server));
  memcpy(&server, server_res->ai_addr, server_res->ai_addrlen);
  freeaddrinfo(server_res);

  send_packet(socketfd, msg, msg_len, 0, (struct sockaddr *) &server, get_addr_len(server));
  signal(SIGALRM, handle_sig_alarm);
  alarm(timeout);

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
      // Got too few incoming bytes. Likely transmission issue. Ignore.
      continue;
    }
    // Got correct ACK cancel alarm.
    alarm(0);
    signal(SIGALRM, SIG_DFL);
    break;
  }

  int heartbeatfd = timerfd_create(CLOCK_REALTIME, 0);
  struct itimerspec timespec;
  memset(&timespec, 0, sizeof (timespec));
  timespec.it_value.tv_sec = 10;
  timespec.it_interval.tv_sec = 10;
  timerfd_settime(heartbeatfd, 0, &timespec, NULL);

  int timeoutfd = timerfd_create(CLOCK_REALTIME, 0);
  memset(&timespec, 0, sizeof (timespec));
  timespec.it_value.tv_sec = timeout;
  timespec.it_interval.tv_sec = timeout;
  timerfd_settime(timeoutfd, 0, &timespec, NULL);

  server_node.type = SERVER;
  server_node.available_to_send = 1;
  server_node.next_pkt_num = "0";
  server_node.addr = &server;
  server_node.lookup_node = NULL;

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
      uint64_t ign;
      read(heartbeatfd, &ign, sizeof(uint64_t));
      handle_heartbeat();
    }

    if (FD_ISSET(timeoutfd, &fds)) {
      uint64_t ign;
      read(heartbeatfd, &ign, sizeof(uint64_t));
      // TODO: Handle timeout
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
        // Illegal datagrams is expected so this is not an error.
        continue;
      } else if (strcmp(msg_part, "PKT") == 0) {
        handle_pkt(msg_delim, incoming);
      } else if (strcmp(msg_part, "ACK") == 0) {
        handle_ack(msg_delim, incoming);
      } else {
        // Illegal datagrams is expected so this is not an error.
        continue;
      }
    }

    if (FD_ISSET(STDIN_FILENO, &fds)) {
      int c;
      int count = 0;
      while((c = getchar()) != EOF && c != '\n' && count < 1421) {
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
        return EXIT_SUCCESS;
      }

      if (buf[0] != '@') {
        continue; //TODO: maybe something else?
      }

      int startmsg = 0;
      char nick[21];
      while (startmsg < 21) {
        if (buf[startmsg] == ' ') break;
        nick[startmsg] = buf[startmsg];
        startmsg++;
      }
      nick[startmsg] = '\0';

      if (buf[startmsg] != ' ' || startmsg == 0) {
        continue; // Illegal nick if it does not end with ' ' or is zero-length
      }
      nick_node_t *search_result = find_nick_node(nick);
      if (search_result == NULL) {
        // Lookup
        // Add message to back of msg queue
        char *nick_persistant = malloc(sizeof(startmsg));
        strcpy(nick_persistant, nick);
        add_lookup(&server_node, nick_persistant, NULL);

        // If client is available to send, add client to send_node and send msg
        if (server_node.available_to_send == 1) {
          server_node.available_to_send = 0;
          send_node_t *new_node = malloc(sizeof(send_node_t));
          new_node->nick_node = &server_node;
          new_node->pkt_num = server_node.next_pkt_num;
          new_node->msg = nick_persistant;
          new_node->type = LOOKUP;
          new_node->num_tries = 0;
          insert_send_node(new_node);
          send_msg(new_node);
        }
        continue;
      }

      // Add message to back of msg queue
      char *new_msg = malloc(strlen(&buf[startmsg+1])+1);
      strcpy(new_msg, &buf[startmsg+1]);
      add_msg(search_result, new_msg);

      // If client is available to send, add client to send_node and send msg
      if (search_result->available_to_send == 1) {
        search_result->available_to_send = 0;
        send_node_t *new_node = malloc(sizeof(send_node_t));
        new_node->nick_node = search_result;
        new_node->pkt_num = search_result->next_pkt_num;
        new_node->msg = new_msg;
        new_node->num_tries = 0;
        new_node->type = MSG;
        insert_send_node(new_node);
        send_msg(new_node);
      }
    }
  }

  free(heartbeat_msg);
  close(socketfd);
  close(heartbeatfd);
  free_recv_nodes();
  return EXIT_FAILURE;
}

void send_msg(send_node_t *node) {
  if (node->type == LOOKUP) {
    // LOOKUP TYPE
    if (node->num_tries < 2) {
      node->num_tries++;
      unsigned long pkt_len = 12 + strlen(node->msg);
      char pkt[pkt_len];
      strcpy(pkt, "PKT ");
      strcat(pkt, node->pkt_num);
      strcat(pkt, " LOOKUP ");
      strcat(pkt, node->nick_node->nick);
      send_packet(socketfd, pkt, pkt_len, 0, (struct sockaddr *) node->nick_node->addr, get_addr_len(*node->nick_node->addr));
    } else {
      printf("NICK %s NOT REACHABLE\n", node->msg);
      //TODO: get next lookup
    }
    return;
  }
  // MSG TYPE
  if (node->num_tries < 2 || (node->num_tries >= RE_0 && node->num_tries < RE_2)) {
    node->num_tries++;
    unsigned long pkt_len = 14 + strlen(my_nick) + strlen(node->nick_node->nick) + strlen(node->msg);
    char pkt[pkt_len];
    strcpy(pkt, "PKT ");
    strcat(pkt, node->pkt_num);
    strcat(pkt, " FROM ");
    strcat(pkt, my_nick);
    strcat(pkt, " TO ");
    strcat(pkt, node->nick_node->nick);
    strcat(pkt, " MSG ");
    strcat(pkt, node->msg);
    send_packet(socketfd, pkt, pkt_len, 0, (struct sockaddr *) node->nick_node->addr, get_addr_len(*node->nick_node->addr));
  } else if (node->num_tries == 3) {
    // Do nothing. We wait for possible new lookup.
  } else {
    // Discard and see if more queued. Otherwise set to ready for transmission.
  }
}

void handle_ack(char *msg_delim, struct sockaddr_storage incoming) {
  char server_ack = 0;
  if (cmp_addr_port(incoming, server) == 1) server_ack = 1;

  char *msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || ((strcmp(msg_part, "0") != 0) && strcmp(msg_part, "1") != 0)) {
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  char pkt_num[2];
  strcpy(pkt_num, msg_part);

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL) {return;}
  if (strcmp(msg_part, "OK") == 0) {
    ack_node(incoming, "OK");
  } else if (strcmp(msg_part, "WRONG") == 0) {
    msg_part = strtok(NULL, msg_delim);
    if (msg_part == NULL) {return;}
    if (strcmp(msg_part, "NAME") == 0) {
      // TODO: What happens with these
      return;
    }
    if (strcmp(msg_part, "FORMAT") == 0) {
      // TODO: What happens with these
      return;
    }
  } else if (strcmp(msg_part, "NICK") == 0) {
    // Get the nick, the IP and the port and insert to cache. Check if there are any dependency nodes that we should "wake"
  } else if (strcmp(msg_part, "NOT") == 0) {
    // Find the lookup node that failed, print error and continue with next if there are any
  } else {
    return;
  }
}

void handle_pkt(char *msg_delim, struct sockaddr_storage incoming) {
  char *msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || ((strcmp(msg_part, "0") != 0) && strcmp(msg_part, "1") != 0)) {
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  char pkt_num[2];
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

  char nick[nick_len+1];
  strcpy(nick, msg_part);
  char legal_nick = 1;
  // Check that the nick is legal. That is: only ascii characters and only alpha characters.
  for (size_t i = 0; i < nick_len; i++) {
    if (!isascii(nick[i]) || !isalpha(nick[i]) || isdigit(nick[i])) {
      legal_nick = 0;
    }
  }
  if (!legal_nick) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, "TO") != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, my_nick) != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG NAME");
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || (strcmp(msg_part, "MSG") != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  msg_part = strtok(NULL, ""); // Get the rest
  if (msg_part == NULL || 1400 < strlen(msg_part)) {
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  recv_node_t *recv_node = find_or_insert_recv_node(nick);

  // If this is the first ever message, check that pkt num is 0.
  if (strcmp(recv_node->expected_msg, "-1") == 0) {
    if (strcmp(pkt_num, "0") == 0) {
      printf("%s: %s\n", nick, msg_part);
      send_ack(socketfd, incoming, pkt_num, 1, "OK");
      recv_node->expected_msg = "1";
      return;
    } else {
      send_ack(socketfd, incoming, pkt_num, 1, "WRONG FORMAT");
      return;
    }
  }

  // Send ack if this is previous message but only print if this is new msg.
  send_ack(socketfd, incoming, pkt_num, 1, "OK");
  if (strcmp(pkt_num,recv_node->expected_msg) == 0) {
    printf("%s: %s\n", nick, msg_part);
    recv_node->expected_msg = strcmp(pkt_num, "0") == 0 ? "1" : "0";
  }
}

recv_node_t *find_recv_node(char *nick) {
  recv_node_t *curr = first_recv_node;
  while(curr != NULL) {
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
    first_recv_node->next = NULL;
    return first_recv_node;
  }
  curr = malloc(sizeof(recv_node_t));
  curr->nick = nick;
  curr->expected_msg = "-1";
  curr->next = first_recv_node;
  first_recv_node = curr;
  return curr;
}

void free_recv_nodes(void) {
  recv_node_t *curr = first_recv_node;
  recv_node_t *tmp;
  while (curr != NULL) {
    tmp = curr;
    free(tmp);
    curr = curr->next;
  }
}

void handle_heartbeat() {
  send_packet(socketfd, heartbeat_msg, strlen(heartbeat_msg)+1, 0, (struct sockaddr *) &server, get_addr_len(server));
}

void handle_sig_alarm(int sig) {
  printf("Timeout. Did not get ACK from server on registration.\n");
  exit(EXIT_SUCCESS); // This is not an error in the program.
}
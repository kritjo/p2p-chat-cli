#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>

#include "upush_client.h"
#include "send_packet.h"
#include "network_utils.h"

#define MAX_MSG 1460

enum USR1_TYPE {
    REG,
    HEARTBEAT,
};

typedef struct {
    enum USR1_TYPE type;
    timer_t *timer;
} usr1_sigval_t;

typedef struct recv_node {
    char *nick;
    char *expected_msg;
    struct recv_node *next;
} recv_node_t;


void handle_sig_usr1(int _1, siginfo_t * siginfo, void * _2);
usr1_sigval_t *register_usr1_custom_sig(enum USR1_TYPE type, usr1_sigval_t *info, time_t timeout, time_t interval);
void unregister_usr_1_custom_sig(usr1_sigval_t *info);
recv_node_t *find_or_insert_recv_node(char *nick);
recv_node_t *find_recv_node(char *nick);
void handle_pkt(char *msg_delim, struct sockaddr_storage incoming);

void handle_ack(char *msg_delim, struct sockaddr_storage incoming);

static int socketfd;
static char *heartbeat_msg;
static struct sockaddr_storage server;
static recv_node_t *first_recv_node = NULL;
static char *my_nick;

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

  socketfd = get_bound_socket(hints, NULL, "0"); // Service 0 to get a random available port
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
  usr1_sigval_t *reg_timer = malloc(sizeof(usr1_sigval_t));
  if (reg_timer == 0) {
    fprintf(stderr, "malloc() failed in main()\n");
    return(EXIT_FAILURE);
  }
  if (register_usr1_custom_sig(REG, reg_timer, timeout, 0) == 0) {
    fprintf(stderr, "register_usr1_custom_sig() failed in main()\n");
    return EXIT_FAILURE;
  }

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
      unregister_usr_1_custom_sig(reg_timer);
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
    unregister_usr_1_custom_sig(reg_timer);
    break;
  }

  // We are now registered with the server and should start sending heartbeats every 10 seconds
  usr1_sigval_t *heartbeat_timer = malloc(sizeof(usr1_sigval_t));
  if (heartbeat_timer == 0) {
    fprintf(stderr, "malloc() failed in main()\n");
    return(EXIT_FAILURE);
  }
  if (register_usr1_custom_sig(HEARTBEAT, heartbeat_timer, 10, 10) == 0) {
    fprintf(stderr, "register_usr1_custom_sig() failed in main()\n");
    return EXIT_FAILURE;
  }

  char buf[MAX_MSG + 1];
  ssize_t bytes_received;
  socklen_t addrlen = sizeof(struct sockaddr_storage);
  struct sockaddr_storage incoming;
  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(socketfd, &fds);

    if ((select(FD_SETSIZE, &fds, NULL, NULL, NULL)) == -1) {
      perror("select");
      break;
    }

    if (FD_ISSET(STDIN_FILENO, &fds)) {
      // Got message from STDIN
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
  }

  unregister_usr_1_custom_sig(heartbeat_timer);
  return 1;
}

void handle_ack(char *msg_delim, struct sockaddr_storage incoming) {
  if (cmp_addr_port(incoming, server) == 1) return; // Ignore server ACKs

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
  if (msg_part == NULL || (strcmp(msg_part, my_nick) != 0)) {
    send_ack(socketfd, incoming, pkt_num, 1, "WRONG NAME");
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  msg_part = strtok(NULL, msg_delim);
  if (msg_part == NULL || 1400 < strlen(msg_part)) {
    // Illegal datagrams is expected so this is not an error.
    return;
  }

  recv_node_t *recv_node = find_or_insert_recv_node(nick);
  send_ack(socketfd, incoming, pkt_num, 1, "OK");
  if (pkt_num == recv_node->expected_msg) {
    printf("%s: %s\n", nick, msg_part);
  }
}

void unregister_usr_1_custom_sig(usr1_sigval_t *info) {
  timer_delete(info->timer);
  free(info->timer);
  free(info);
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
    first_recv_node->expected_msg = "0";
    first_recv_node->next = NULL;
    return first_recv_node;
  }
  curr = malloc(sizeof(recv_node_t));
  curr->nick = nick;
  curr->expected_msg = "0";
  curr->next = first_recv_node;
  first_recv_node = curr;
  return curr;
}

usr1_sigval_t *register_usr1_custom_sig(enum USR1_TYPE type, usr1_sigval_t *info, time_t timeout, time_t interval) {
  timer_t *timer = malloc(sizeof(*timer));
  if (timer == NULL) {
    fprintf(stderr, "malloc() failed in register_usr1_custom_sig()\n");
    return 0;
  }

  sigevent_t event;
  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;

  info->timer = timer;
  info->type = type;
  event.sigev_value.sival_ptr = info;

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = &handle_sig_usr1;
  action.sa_flags = SA_SIGINFO;
  if (sigaction(SIGUSR1, &action, NULL) != 0) {
    perror("sigaction");
    free(timer);
    return 0;
  }

  if (timer_create(CLOCK_REALTIME, &event, timer) == -1) {
    perror("timer_create");
    free(timer);
    return 0;
  }

  struct itimerspec timespec;
  memset(&timespec, 0, sizeof(timespec));
  timespec.it_value.tv_sec = timeout;
  timespec.it_interval.tv_sec = interval;
  if (timer_settime(*timer, 0, &timespec, NULL) == -1) {
    perror("timer_settime()");
    free(timer);
    return 0;
  }
  return info;
}

void handle_sig_usr1(int _1, siginfo_t * siginfo, void * _2) {
  usr1_sigval_t *info = (usr1_sigval_t *) siginfo->si_value.sival_ptr;
  switch (info->type) {
    case REG:
      printf("Timeout. Did not get ACK from server on registration.\n");
      unregister_usr_1_custom_sig(info);
      exit(EXIT_SUCCESS); // This is not an error in the program.
    case HEARTBEAT:
      send_packet(socketfd, heartbeat_msg, strlen(heartbeat_msg)+1, 0, (struct sockaddr *) &server, get_addr_len(server));
      break;
    default:
      fprintf(stderr, "Illegal USR1 signal caught.\n");
      exit(EXIT_FAILURE);
  }
}
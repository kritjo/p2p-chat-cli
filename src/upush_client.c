#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>

#include "upush_client.h"
#include "send_packet.h"
#include "network_utils.h"

#define MAX_MSG 1460

enum USR1_TYPE {
    REG,
    HEARTBEAT,
};

void handle_sig_usr1(int sig, siginfo_t * siginfo, void * _);
timer_t *register_usr1_custom_sig(enum USR1_TYPE type, time_t timeout, time_t interval);

static int socketfd;
static char *heartbeat_msg;
static struct sockaddr_storage server;

int main(int argc, char **argv) {
  char *nick, *server_addr, *server_port, loss_probability;
  long timeout;

  if (argc == 6) {
    nick = argv[1];

    // Check that the nick is legal. That is: only ascii characters and only alpha characters. Max len 20 char
    size_t nick_len = strlen(nick);
    char legal_nick = 1;
    if (20 < nick_len) legal_nick = 0;
    else {
      for (size_t i = 0; i < nick_len; i++) {
        if (!isascii(nick[i]) || !isalpha(nick[i]) || isdigit(nick[i])) {
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
  size_t msg_len = 11 + strlen(nick);
  char msg[msg_len];
  strcpy(msg, "PKT 0 REG ");
  strcat(msg, nick);
  // Save it to be used in heartbeat msg
  heartbeat_msg = malloc(msg_len * sizeof(char));
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

  server = *(struct sockaddr_storage *) server_res->ai_addr;
  freeaddrinfo(server_res);
  send_packet(socketfd, msg, msg_len, 0, (struct sockaddr *) &server, get_addr_len(server));
  timer_t *reg_timer = register_usr1_custom_sig(REG, timeout, 0);

  while (1) {
    ssize_t bytes_received = 0;
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
    timer_delete(reg_timer);
    printf("Got ACK from server: %s\n", buf);
    break;
  }
  // We are now registered with the server and should start sending heartbeats every 10 seconds
  register_usr1_custom_sig(HEARTBEAT, 10, 10);
  while(1);
  return 1;
}

timer_t *register_usr1_custom_sig(enum USR1_TYPE type, time_t timeout, time_t interval) {
  timer_t *timer = malloc(sizeof(*timer));

  sigevent_t event;
  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;
  event.sigev_value.sival_int = type;

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = &handle_sig_usr1;
  action.sa_flags = SA_SIGINFO;
  if (sigaction(SIGUSR1, &action, NULL) != 0) {
    perror("sigaction");
    exit(EXIT_FAILURE);
  }

  if (timer_create(CLOCK_REALTIME, &event, timer) == -1) {
    perror("timer_create");
    exit(EXIT_FAILURE);
  }

  struct itimerspec timespec;
  memset(&timespec, 0, sizeof(timespec));
  timespec.it_value.tv_sec = timeout;
  timespec.it_interval.tv_sec = interval;
  if (timer_settime(*timer, 0, &timespec, NULL) == -1) {
    perror("timer_settime()");
    exit(EXIT_FAILURE);
  }
}

void handle_sig_usr1(int _1, siginfo_t * siginfo, void * _2) {
  switch (siginfo->si_value.sival_int) {
    case REG:
      printf("Timeout. Did not get ACK from server on registration.\n");
      exit(EXIT_SUCCESS); // This is not an error in the program.
    case HEARTBEAT:
      send_packet(socketfd, heartbeat_msg, strlen(heartbeat_msg)+1, 0, (struct sockaddr *) &server, get_addr_len(server));
      printf("SENDING HEARTBEAT!\n");
      break;
    default:
      fprintf(stderr, "Illegal USR1 signal caught.\n");
      exit(EXIT_FAILURE);
  }
}
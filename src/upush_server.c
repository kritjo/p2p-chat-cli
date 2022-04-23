#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <search.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <time.h>

#include "send_packet.h"

#define MAX_CLIENTS 100
#define MAX_MSG 1500 // Longest msg can be 20 char + 2*nicklen + message
                     // Max message length is 1400.
                     // Assume that pkt num is 0 or 1.
                     // This implies a max len nick of 50 char.

typedef struct {
    struct sockaddr_storage *addr;
    time_t *registered_time;
} nick_t;

int send_ack(struct sockaddr_storage *addr, char *pkt_num, char num_args, ...);
char *get_addr(struct sockaddr_storage *addr);
char *get_port(struct sockaddr_storage *addr);

int main(int argc, char **argv) {
    int socketfd;
    int rc; // Return code
    char *port, loss_probability;
    struct addrinfo hints, *res, *curr_res;
    struct sockaddr_storage inncomming;
    const char msg_delim = ' ';

    // Check that we have enough and correct CLI arguments
    if (argc == 3) {
        port = argv[1];

        // Using strtol to avoid undefined behaviour
        long tmp;
        // TODO: DO NOT ASSUME THAT WE GET CORRECT PORT NUM
        tmp = strtol(argv[2], NULL, 10);
        // TODO: change <= to < on release
        if (0 <= tmp && tmp <= 100) {
            loss_probability = tmp;
        } else {
            printf("Illegal loss probability. Enter a number between 0 and 100.\n");
            EXIT_SUCCESS; // Return success as this is not an error case, but expected with wrong
                          // port number
        } 
    } else {
        printf("USAGE: %s <port> <loss_probability>\n", argv[0]);
        return EXIT_SUCCESS; // Return success as this is not an error, but expected without args.
    }

    set_loss_probability(loss_probability / 100.0f);

    // Create a hash table to store client registrations
    if(!hcreate((size_t) MAX_CLIENTS)) {
        perror("hcreate");
    }

    // Make the addrinfo struct ready
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    if ((rc = getaddrinfo(NULL, port, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return EXIT_FAILURE;
    }


    // The resulting addrinfo struct res could have multiple Internet adresses
    // bind to the first valid
    for (curr_res = res; curr_res != NULL; curr_res = curr_res->ai_next) {
        if ((socketfd = socket(
                        curr_res->ai_family,
                        curr_res->ai_socktype,
                        curr_res->ai_protocol)
                    ) == -1) {
            perror("socket");
            fprintf(stderr, "Could not aquire socket, trying next result.\n");
            continue; // If socket return error, try next.
        }

        if (bind(socketfd, curr_res->ai_addr, curr_res->ai_addrlen) != -1)
            break; // Bound socket, exit loop.

        perror("bind");
        fprintf(stderr, "Could not bind socket, trying next result.\n");
        close(socketfd);
    }

    freeaddrinfo(res);

    if (curr_res == NULL) {
        fprintf(stderr, "No socket bound. No further results.\n");
        exit(EXIT_FAILURE);
    }

    char buf[MAX_MSG+1] /* Add place for null char */, *pkt_num, *msg_part;
    ssize_t bytes_recived = 0;
    socklen_t addrlen = sizeof(inncomming);
    while (1) {
        bytes_recived = recvfrom(
                socketfd,
                (void *) buf,
                MAX_MSG,
                0,
                (struct sockaddr *) &inncomming, // Store the origin adr of inncomming dgram
                &addrlen
                );
        
        switch (bytes_recived) {
            case 0:
                continue; // Zero length dgrams are allowed. This is not an error.
                break;
            case -1:
                perror("recvfrom"); // On error we should quit the server.
                exit(EXIT_FAILURE);
        }
        buf[bytes_recived] = '\0';
        
        // Get first part of msg, should be "PKT"
        msg_part = strtok(buf, &msg_delim);
        // Short circuit in case of NULL
        if (msg_part == NULL || strcmp(msg_part, "PKT") != 0) {
            // Illegal datagrams is expected so this is not an error.
            printf("Recived illegal datagram.\n"); //TODO: add recived from: ip.
            continue;
        }

        char illegal = 0;
        // Get second part of msg, should be "0" or "1"
        msg_part = strtok(NULL, &msg_delim);
        if (msg_part == NULL) illegal = 1;
        if (strcmp(msg_part, "0") != 0 && strcmp(msg_part, "1") != 0) illegal = 1;
        if (illegal) {
            // Illegal datagrams is expected so this is not an error.
            printf("Recived illegal datagram.\n"); //TODO: add recived from: ip.
            continue;
        }
        pkt_num = msg_part;

        enum state{ REG, LOOKUP };
        enum state curr_state;
        // Get third part of msg, should be "REG" or "LOOKUP"
        msg_part = strtok(NULL, &msg_delim);
        if (msg_part == NULL) illegal = 1;
        if (strcmp(msg_part, "REG") == 0) {
            curr_state = REG;
        }
        else if (strcmp(msg_part, "LOOKUP") == 0) {
            curr_state = LOOKUP;
        }
        else {
            // Illegal datagrams is expected so this is not an error.
            printf("Recived illegal datagram.\n"); //TODO: add recived from: ip.
            continue;
        }

        // Get final part of msg, should be a nickname.
        msg_part = strtok(NULL, &msg_delim);
        int nick_len;
        if (msg_part == NULL || (nick_len = strlen(msg_part)) < 1 || nick_len > 50) {
            // Illegal datagrams is expected so this is not an error.
            printf("Recived illegal datagram.\n"); //TODO: add recived from: ip.
            continue;
        }
        
        // Remove newline
        if (msg_part[strlen(msg_part) - 1] == '\n') {
            char nickbuf[strlen(msg_part)];
            strncpy(nickbuf, msg_part, strlen(msg_part) - 1);
            nickbuf[strlen(msg_part) - 1] = '\0';
            strcpy(msg_part, nickbuf);
            printf("%s : %s\n", msg_part, nickbuf);
        }

        if (curr_state == REG) {
            // Check if nick exists, in that case, replace current address in table.
            ENTRY e, *e_res;
            e.key = msg_part;
            e_res = hsearch(e, FIND);
            if (e_res == NULL) {
                printf("NICK NOT FOUND CREATING.\n");
                // Now we know that the nick does not exist.
                nick_t *curr_nick = malloc(sizeof(nick_t));
                curr_nick->registered_time = malloc(sizeof(time_t));

                curr_nick->addr = &inncomming;
                time(curr_nick->registered_time);

                if (*curr_nick->registered_time == (time_t) -1) {
                    perror("Could not get system time");
                    exit(EXIT_FAILURE);
                }

                e.data = curr_nick;
                e_res = hsearch(e, ENTER);

                if (e_res == NULL) {
                    fprintf(stderr, "Nick table is full\n");
                    continue; // TODO: Should probably check if we are missing heartbeats.
                }

                // Registration completed send ACK
                send_ack(&inncomming, pkt_num, 1, "OK");
            } else {
                printf("NICK FOUND UPDATING.\n");
                // Update the nick with the inncomming adr and current time.
                // This implies that the code flow will end up here for both updates
                // to an entry from a new addr, and heartbeats.
                nick_t *curr_nick = (nick_t *) e_res->data;
                curr_nick->addr = &inncomming;
                time(curr_nick->registered_time);
                if (curr_nick->registered_time == (time_t) -1) {
                    perror("Could not get system time");
                    exit(EXIT_FAILURE);
                }
                send_ack(&inncomming, pkt_num, 1, "OK");
            }
        } else {
            printf("LOOKUP.\n");
            // We can be certain that we are now in lookup phase.
            ENTRY e, *e_res;
            e.key = msg_part;
            e_res = hsearch(e, FIND);
            time_t current_time;
            time(&current_time);
            nick_t *curr_nick = (nick_t *) e_res->data;

            if (e_res == NULL || current_time - curr_nick->registered_time > 30) {
                send_ack(&inncomming, pkt_num, 1, "NOT FOUND");
                continue;
            }
            printf("%s\n", msg_part); 
            send_ack(&inncomming, pkt_num, 5, "NICK", msg_part, get_addr(&inncomming), "PORT", get_port(&inncomming));
        }
    }

    close(socketfd);

    return EXIT_SUCCESS;
}

int send_ack(struct sockaddr_storage *addr, char *pkt_num, char num_args, ...) {
    va_list args;
    va_start(args, num_args);

    char **args_each = alloca(num_args * sizeof(char *));

    // Get total message length
    size_t msg_len = 6; // "ACK 0"
    char n = num_args;
    while(n) {
        printf("adding arg\n");
        msg_len += 1; // Leading space
        args_each[num_args - n] = va_arg(args, char *);
        msg_len += strlen(args_each[num_args - n]);
        n--;
    }
    
    char msg[msg_len];
    strcpy(msg, "ACK ");
    strcat(msg, pkt_num);
    while(n < num_args) {
        strcat(msg, " ");
        strcat(msg, args_each[n]);
        n++;
    }
    printf("%s\n", msg);
    return 0;
}

char *get_addr(struct sockaddr_storage *addr) {
    return "1.1.1.1";
}

char *get_port(struct sockaddr_storage *addr) {
    return "123";
}


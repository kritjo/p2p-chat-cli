#include <stdlib.h>
#include <stdio.h>
#include <search.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "send_packet.h"

#define MAX_CLIENTS 100
#define MAX_MSG 1441 // Longest msg is 40 char + 1400 max text length + 1 null byte 

int main(int argc, char **argv) {
    int socketfd;
    int rc; // Return code
    char *port, loss_probability;
    struct addrinfo hints, *res, *curr_res;

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

    char buf[MAX_MSG];
    int numb = 0;
    numb = recvfrom(socketfd, (void *) buf, MAX_MSG-1, 0, NULL, NULL);
    buf[numb] = '\0';

    printf("%s\n", buf);


    close(socketfd);

    return EXIT_SUCCESS;
}

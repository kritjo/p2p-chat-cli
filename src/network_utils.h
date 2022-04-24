#ifndef SRC_NETWORK_UTILS_H
#define SRC_NETWORK_UTILS_H


#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

int cmp_addr_port(struct sockaddr_storage first, struct sockaddr_storage second);

char *get_addr(struct sockaddr_storage addr, char *buf, size_t buflen);

char *get_port(struct sockaddr_storage addr, char *buf);

socklen_t get_addr_len(struct sockaddr_storage addr);

int get_bound_socket(struct addrinfo hints, char *name, char *service);

#endif //SRC_NETWORK_UTILS_H

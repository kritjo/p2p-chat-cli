#ifndef SRC_NETWORK_UTILS_H
#define SRC_NETWORK_UTILS_H


#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <arpa/inet.h>

char *get_addr(struct sockaddr_storage addr, char *buf, size_t buflen);

char *get_port(struct sockaddr_storage addr, char *buf);

socklen_t get_addr_len(struct sockaddr_storage addr);

#endif //SRC_NETWORK_UTILS_H

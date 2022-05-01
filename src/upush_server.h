#ifndef SRC_UPUSH_SERVER_H
#define SRC_UPUSH_SERVER_H

#include "linked_list.h"

#define MAX_MSG 1460 // Longest msg can be 20 char + 2*nicklen + message
// Max message length is 1400.
// Assume that pkt num is 0 or 1.
// Nicklen is max 20 char

typedef struct nick_node {
    struct sockaddr_storage *addr;
    time_t *registered_time;
} nick_node_t;

void handle_exit(void);

void handle_sig_terminate(__attribute__((unused)) int sig);

void print_illegal_dram(struct sockaddr_storage addr);

void free_nick_node(node_t *node);

void handle_sig_alarm(__attribute__((unused)) int sig);

void handle_sig_ignore(__attribute__((unused)) int sig) {}

#endif //SRC_UPUSH_SERVER_H

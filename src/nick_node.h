#ifndef SRC_NICK_NODE_H
#define SRC_NICK_NODE_H

#define MAX_NODES 10000 // Must be an integer value
#define TIMEOUT 30 // in seconds

#include <time.h>
#include <string.h>
#include <stdlib.h>

typedef struct nick_node {
    char *nick;
    struct sockaddr_storage *addr;
    time_t *registered_time;
    struct nick_node *next;
    struct nick_node *prev;
} nick_node_t;

int insert_nick_node(nick_node_t *node);

nick_node_t *find_nick_node(char *key);

void delete_nick_node(nick_node_t *node);

void free_nick_node(nick_node_t *node);

void delete_all_nick_nodes(void);

void delete_old_nick_nodes(void);

#endif //SRC_NICK_NODE_H

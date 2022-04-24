#ifndef SRC_NICK_NODE_SERVER_H
#define SRC_NICK_NODE_SERVER_H

#define MAX_NODES 10000 // Must be an integer value
#define TIMEOUT 30 // in seconds

#include <time.h>
#include <string.h>
#include <stdlib.h>

typedef struct nick_node_server {
    char *nick;
    struct sockaddr_storage *addr;
    time_t *registered_time;
    struct nick_node_server *next;
    struct nick_node_server *prev;
} nick_node_t;

int insert_nick_node(nick_node_t *node);

nick_node_t *find_nick_node(char *key);

void delete_nick_node(nick_node_t *node);

void free_nick_node(nick_node_t *node);

void delete_all_nick_nodes(void);

void delete_old_nick_nodes(void);

#endif //SRC_NICK_NODE_SERVER_H

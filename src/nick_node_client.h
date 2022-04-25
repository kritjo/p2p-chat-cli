#ifndef SRC_NICK_NODE_CLIENT_H
#define SRC_NICK_NODE_CLIENT_H

#define MAX_NODES 10000 // Must be an integer value
#define TIMEOUT 30 // in seconds

#include <time.h>
#include <string.h>
#include <stdlib.h>

enum nick_node_type { CLIENT, SERVER };

struct nick_node_client;

typedef struct message_node {
    char *message;
    struct message_node *next;
} message_node_t;

typedef struct lookup_node {
    char *nick;
    struct nick_node_client *waiting_node;
    struct lookup_node *next;
} lookup_node_t;

typedef struct nick_node_client {
    enum nick_node_type type;
    char *nick;
    union {
        message_node_t *msg_to_send; // FIFO queue of messages that have not been sent yet.
        lookup_node_t *lookup_node;
    };
    char available_to_send; // 1 if last msg was ACKed or not transmitted successfully.
    char *next_pkt_num;
    struct sockaddr_storage *addr;
    struct nick_node_client *next;
    struct nick_node_client *prev;
} nick_node_t;

int insert_nick_node(nick_node_t *node);

nick_node_t *find_nick_node(char *key);

void delete_nick_node(nick_node_t *node);

void free_nick_node(nick_node_t *node);

void delete_all_nick_nodes(void);

void add_msg(nick_node_t *node, char *msg);

void add_lookup(nick_node_t *node, char *nick, nick_node_t *waiting_node);

char *pop_msg(nick_node_t *node);

lookup_node_t *pop_lookup(nick_node_t *node);

#endif //SRC_NICK_NODE_CLIENT_H

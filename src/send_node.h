#ifndef SRC_SEND_NODE_H
#define SRC_SEND_NODE_H

#include "nick_node_client.h"
#include "real_time.h"

enum sendtype {MSG, LOOKUP};

typedef struct send_node {
    enum sendtype type; // IF LOOKUP TYPE msg is nick to lookup, nick_node can be null if initial lookup or a node waiting to know if it should try sending again
    nick_node_t *nick_node;
    char should_free_pkt_num;
    char *pkt_num;
    char num_tries;
    char *msg;
    struct usr1_sigval *timeout_timer;
    struct send_node *next;
    struct send_node *prev;
} send_node_t;

int insert_send_node(send_node_t *node);

void delete_send_node(send_node_t *node);

send_node_t *find_send_node(char *key);

void free_send_node(send_node_t *node);

void delete_all_send_nodes(void);

#endif //SRC_SEND_NODE_H

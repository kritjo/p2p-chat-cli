#ifndef SRC_UPUSH_CLIENT_H
#define SRC_UPUSH_CLIENT_H

#include "send_node.h"

#define MAX_MSG 1460

typedef struct recv_node {
    char *nick;
    char *expected_msg;
    long stamp; // This is a timestamp (used on initial package). If updated version, reset expected_msg.
    struct recv_node *next;
} recv_node_t;

typedef struct block_node {
    char *nick;
    struct block_node *next;
    struct block_node *prev;
} block_node_t;

void handle_sig_alarm(int sig);
recv_node_t *find_or_insert_recv_node(char *nick);
recv_node_t *find_recv_node(char *nick);
void handle_pkt(char *msg_delim, struct sockaddr_storage incoming);
void handle_heartbeat();

void handle_ack(char *msg_delim, struct sockaddr_storage incoming);
void free_recv_nodes(void);

void send_msg(send_node_t *node);

void queue_lookup(nick_node_t *node, int callback);

void maybe_send_msg(nick_node_t *nick_node, char *msg);

void insert_block_node(char *nick);
char is_blocked(char *nick);
void delete_blocked(char *nick);
void next_msg(nick_node_t *node);

const char WAIT_INIT = -1;
const char DO_NEW_LOOKUP = 2;
const char WAIT_FOR_LOOKUP = 3;
const char RE_0 = 10;
const char RE_2 = 12;

#endif //SRC_UPUSH_CLIENT_H

#ifndef SRC_UPUSH_CLIENT_H
#define SRC_UPUSH_CLIENT_H

#define HANDLE_EXIT_ON_MINUS_ONE(msg, param) if ((param) == -1) handle_exit(EXIT_FAILURE);
#define MAX_MSG 1460
#define MY_SOCK_TYPE AF_INET6 // Register AF_INET6 socket as default, as we want IPv4 mapped on IPv6 plus IPv6 native easy
                              // Switch to AF_INET if that is not available on your system or if you want to only use
                              // IPv4

#include "real_time.h"
#include "linked_list.h"


typedef struct recv_node {
    char *expected_msg;
    long stamp; // This is a timestamp (used on initial package). If updated version, reset expected_msg.
} recv_node_t;

enum nick_node_type {
    CLIENT, SERVER
};

struct nick_node;

typedef struct message_node {
    char *message;
    struct message_node *next;
} message_node_t;

typedef struct lookup_node {
    char *nick;
    struct nick_node *waiting_node;
    struct lookup_node *next;
} lookup_node_t;

typedef struct nick_node {
    enum nick_node_type type;
    char *nick;
    union {
        message_node_t *msg_to_send; // FIFO queue of messages that have not been sent yet.
        lookup_node_t *lookup_node;
    };
    char available_to_send; // 1 if last msg was ACKed or not transmitted successfully.
    char *next_pkt_num;
    struct sockaddr_storage *addr;
} nick_node_t;

enum sendtype {
    MSG, LOOKUP
};

typedef struct send_node {
    enum sendtype type; // IF LOOKUP TYPE msg is nick to lookup, nick_node can be null if initial lookup or a node waiting to know if it should try sending again
    nick_node_t *nick_node;
    char should_free_pkt_num;
    char *pkt_num;
    char num_tries;
    char *msg;
    struct usr1_sigval *timeout_timer;
} send_node_t;

void handle_ok_ack(struct sockaddr_storage storage);

void handle_wrong_ack(struct sockaddr_storage incoming, char *msg_delim);

void handle_nick_ack(char *msg_delim, char pkt_num[256]);

void handle_not_ack();

void send_lookup(send_node_t *node);

void send_msg(send_node_t *node);

void new_lookup(char nick[21], int startmsg, char *new_msg);

void next_lookup();

void handle_sig_alarm(__attribute__((unused)) __attribute__((unused)) int sig);

void handle_pkt(char *msg_delim, struct sockaddr_storage incoming);

void handle_heartbeat();

void handle_ack(char *msg_delim, struct sockaddr_storage incoming);

void send_node(send_node_t *node);

void queue_lookup(nick_node_t *node, int callback, int free_node);

void next_msg(nick_node_t *node, int set_timestamp);

void add_msg(nick_node_t *node, char *msg);

void add_lookup(nick_node_t *node, char *nick, nick_node_t *waiting_node);

char *pop_msg(nick_node_t *node);

lookup_node_t *pop_lookup(nick_node_t *node);

void register_with_server();

void free_nick(node_t *node);

void free_send(node_t *node);

void free_timer_node(node_t *node);

void handle_sig_terminate(__attribute__((unused)) int sig);

void handle_sig_ignore(__attribute__((unused)) int sig) {}

void handle_exit(int status);

void handle_sig_terminate_pre_reg(int sig);


const char WAIT_INIT = -1;
const char DO_NEW_LOOKUP = 2;
const char WAIT_FOR_LOOKUP = 3;
const char RE_0 = 10;
const char RE_2 = 12;

#endif //SRC_UPUSH_CLIENT_H

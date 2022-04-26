#include "nick_node_client.h"

static nick_node_t *firstNick = 0;
static int num_nick_nodes = 0;

int insert_nick_node(nick_node_t *node) {
  if (num_nick_nodes >= MAX_NODES) return -1;

  if (firstNick == NULL) {
    firstNick = node;
    node->prev = NULL;
    node->next = NULL;
    num_nick_nodes++;
    return 1;
  }

  firstNick->prev = node;
  node->next = firstNick;
  firstNick = node;
  node->prev = NULL;
  num_nick_nodes++;
  return 1;
}

void delete_nick_node(nick_node_t *node) {
  if (node->prev == 0) {
    firstNick = node->next;
    if (firstNick != NULL) firstNick->prev = 0;
    free_nick_node(node);
  } else if (node->next == 0) {
    node->prev->next = 0;
    free_nick_node(node);
  } else {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    free_nick_node(node);
  }
  num_nick_nodes--;
}

nick_node_t *find_nick_node(char *key) {
  nick_node_t *curr = firstNick;
  while(curr != NULL) {
    if (strcmp(curr->nick, key) == 0) return curr;
    curr = curr->next;
  }
  return NULL;
}

void free_nick_node(nick_node_t *node) {
  message_node_t *curr = node->msg_to_send;
  while (curr != NULL) {
    free(curr->message);
    message_node_t *tmp = curr;
    curr = curr->next;
    free(tmp);
  }
  free(node->addr);
  free(node->nick);
  free(node);
}

void delete_all_nick_nodes(void) {
  nick_node_t *current = firstNick;
  while (current != NULL) {
    nick_node_t *nxt = current->next;
    delete_nick_node(current);
    current = nxt;
  }
}

void add_msg(nick_node_t *node, char *msg) {
  message_node_t *new_message = malloc(sizeof(message_node_t));
  new_message->message = msg;
  new_message->next = NULL;
  if (node->msg_to_send == NULL) {
    node->msg_to_send = new_message;
    return;
  }
  message_node_t *curr = node->msg_to_send;
  while (curr->next != NULL) curr = curr->next;
  curr->next = new_message;
}

char *pop_msg(nick_node_t *node) {
  char *msg = node->msg_to_send->message;
  message_node_t *tmp = node->msg_to_send;
  node->msg_to_send = node->msg_to_send->next;
  free (tmp);
  return msg;
}

lookup_node_t *pop_lookup(nick_node_t *node) {
  lookup_node_t *popped = node->lookup_node;
  node->lookup_node = node->lookup_node->next;
  return popped;
}

void add_lookup(nick_node_t *node, char *nick, nick_node_t *waiting) {
  lookup_node_t *new_lookup = malloc(sizeof(lookup_node_t));
  new_lookup->nick = nick;
  new_lookup->waiting_node = waiting;
  new_lookup->next = NULL;
  if (node->lookup_node == NULL) {
    node->lookup_node = new_lookup;
    return;
  }
  lookup_node_t *curr = node->lookup_node;
  while (curr->next != NULL) curr = curr->next;
  curr->next = new_lookup;
}
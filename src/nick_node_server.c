#include "nick_node_server.h"

static nick_node_t *firstNick = 0;
static int num_nick_nodes = 0;

int insert_nick_node(nick_node_t *node) {
  if (num_nick_nodes >= MAX_NODES) return -1;

  if (firstNick == NULL) {
    firstNick = node;
    num_nick_nodes++;
    return 1;
  }

  firstNick->prev = node;
  node->next = firstNick;
  firstNick = node;
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
  free(node->registered_time);
  free(node->addr);
  free(node->nick);
  free(node);
}

void delete_all_nick_nodes(void) {
  nick_node_t *current = firstNick;
  while (current != 0) {
    nick_node_t *nxt = current->next;
    delete_nick_node(current);
    current = nxt;
  }
}

void delete_old_nick_nodes(void) {
  nick_node_t *current = firstNick;
  while (current != 0) {
    time_t current_time;
    time(&current_time);

    nick_node_t *nxt = current->next;

    if (current_time - *current->registered_time > TIMEOUT) {
      delete_nick_node(current);
    }

    current = nxt;
  }
}
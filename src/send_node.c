#include "send_node.h"

static send_node_t *first_node = 0;

int insert_send_node(send_node_t *node) {
  if (first_node == NULL) {
    first_node = node;
    return 1;
  }

  first_node->prev = node;
  node->next = first_node;
  first_node = node;
  return 1;
}

void delete_send_node(send_node_t *node) {
  if (node->prev == 0) {
    first_node = node->next;
    if (first_node != NULL) first_node->prev = 0;
    free_send_node(node);
  } else if (node->next == 0) {
    node->prev->next = 0;
    free_send_node(node);
  } else {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    free_send_node(node);
  }
}

send_node_t *find_send_node(char *key) {
  send_node_t *curr = first_node;
  while(curr != NULL) {
    if (strcmp(curr->nick_node->nick, key) == 0) return curr;
    curr = curr->next;
  }
  return NULL;
}

void free_send_node(send_node_t *node) {
  free(node);
}

void delete_all_send_nodes(void) {
  send_node_t *current = first_node;
  while (current != 0) {
    send_node_t *nxt = current->next;
    delete_send_node(current);
    current = nxt;
  }
}
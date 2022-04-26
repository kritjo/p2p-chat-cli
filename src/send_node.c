#include "send_node.h"

static send_node_t *first_node = NULL;

int insert_send_node(send_node_t *node) {
  if (first_node == NULL) {
    first_node = node;
    first_node->next = NULL;
    first_node->prev = NULL;
    return 1;
  }

  first_node->prev = node;
  node->next = first_node;
  first_node = node;
  node->prev = NULL;
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

// key == NULL returns first
send_node_t *find_send_node(char *key) {
  send_node_t *curr = first_node;
  if (key == NULL) return curr;
  while(curr != NULL) {
    if (curr->type == LOOKUP) {
      if (strcmp(curr->msg, key) == 0) return curr;
    } else if (strcmp(curr->nick_node->nick, key) == 0) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
}

void free_send_node(send_node_t *node) {
  if (node->should_free_pkt_num) free(node->pkt_num);
  unregister_usr_1_custom_sig(node->timeout_timer);
  free(node);
}

void delete_all_send_nodes(void) {
  send_node_t *current = first_node;
  while (current != 0) {
    send_node_t *nxt = current->next;
    free(current->msg);
    delete_send_node(current);
    current = nxt;
  }
}
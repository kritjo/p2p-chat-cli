//
// Created by kritjo on 28.04.22.
//

#include <malloc.h>
#include <string.h>
#include "linked_list.h"

void insert_node(node_t **head, char *key, void *data) {
  node_t *node = malloc(sizeof(node_t));
  node->key = malloc(strlen(key) + 1);
  strcpy(node->key, key);

  if (*head == NULL) {
    *head = node;
    (*head)->next = NULL;
    (*head)->prev = NULL;
    return;
  }

  (*head)->prev = node;
  node->next = (*head);
  (*head) = node;
}

node_t *find_node(node_t **head, char *key) {
  node_t *node = (*head);
  while (node != NULL) {
    if (strcmp(node->key, key) == 0) break;
    node = node->next;
  }
  return node;
}

void delete_node(node_t **head, char *key) {
  node_t *node = (*head);
  while (node != NULL) {
    if (strcmp(node->key, key) == 0) break;
    node = node->next;
  }
  if (node == NULL) return;

  if (node->prev == 0) {
    (*head) = node->next;
    if ((*head) != NULL) (*head)->prev = 0;
    free(node->key);
    free(node);
  } else if (node->next == 0) {
    node->prev->next = 0;
    free(node->key);
    free(node);
  } else {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    free(node->key);
    free(node);
  }
}
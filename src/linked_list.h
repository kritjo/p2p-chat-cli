//
// Created by kritjo on 28.04.22.
//

#ifndef SRC_LINKED_LIST_H
#define SRC_LINKED_LIST_H

typedef struct node {
    char *key;
    void *data;
    struct node *next;
    struct node *prev;
} node_t;

#endif //SRC_LINKED_LIST_H

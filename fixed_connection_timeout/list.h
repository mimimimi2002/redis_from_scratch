#pragma once

#include <stddef.h>


struct DListNode {
    DListNode *prev = NULL;
    DListNode *next = NULL;
};

inline void dlist_init(DListNode *node) {
    node->prev = node->next = node;
}

inline bool dlist_empty(DListNode *node) {
    return node->next == node;
}

inline void dlist_detach(DListNode *node) {
    DListNode *prev = node->prev;
    DListNode *next = node->next;
    prev->next = next;
    next->prev = prev;
}

inline void dlist_insert_before(DListNode *target, DListNode *rookie) {
    DListNode *prev = target->prev;
    prev->next = rookie;
    rookie->prev = prev;
    rookie->next = target;
    target->prev = rookie;
}

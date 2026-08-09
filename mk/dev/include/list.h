#ifndef ARC_LIST_H
#define ARC_LIST_H

struct list_node {
    struct list_node *next;
    struct list_node *prev;
};

static inline void list_init(struct list_node *head) {
    head->next = head;
    head->prev = head;
}

static inline void list_insert(struct list_node *head, struct list_node *node) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

static inline void list_append(struct list_node *head, struct list_node *node) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}

static inline void list_remove(struct list_node *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

static inline int list_empty(struct list_node *head) {
    return head->next == head;
}

#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

#define list_for_each(pos, head) \
    for (struct list_node *pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, tmp, head) \
    for (struct list_node *pos = (head)->next, *tmp = pos->next; \
         pos != (head); \
         pos = tmp, tmp = pos->next)

#endif

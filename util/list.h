#ifndef LIST_H
#define LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

struct list {
    struct list *next, *prev;
};

#ifndef __KERNEL__

// Initializes a list node by setting its next and prev pointers to point to itself, effectively creating
// a new circular list with one element.
static inline void list_init(struct list *list) {
    list->next = list;
    list->prev = list;
}

// This is a macro that can be used to statically initialize a list node. It sets both the next and prev pointers of the
// list node to point to the node itself, similar to list_init.
#define LIST_INITIALIZER(x) {.prev = &x, .next = &x}

// Checks if both the next and prev pointers of a list node are NULL. This is typically used to check if the list node
// has not been added to any list and is stand-alone.
static inline bool list_null(struct list *list) {
    return list->next == NULL && list->prev == NULL;
}

// A list is considered empty if its next pointer points to itself or if both next and prev pointers are NULL. This 
// function is useful to check before performing operations that require the list to have elements.
static inline bool list_empty(struct list *list) {
    return list->next == list || list_null(list);
}

static inline void _list_add_between(struct list *prev, struct list *next, struct list *item) {
    assert(prev != NULL && next != NULL);
    
    prev->next = item;
    item->prev = prev;
    item->next = next;
    next->prev = item;
}

static inline void list_add_tail(struct list *list, struct list *item) {
    _list_add_between(list->prev, list, item);
}

static inline void list_add(struct list *list, struct list *item) {
    assert(list != NULL && !list_null(list));
    
    _list_add_between(list, list->next, item);
}

static inline void list_add_before(struct list *before, struct list *item) {
    list_add_tail(before, item);
}
static inline void list_add_after(struct list *after, struct list *item) {
    list_add(after, item);
}

static inline void list_init_add(struct list *list, struct list *item) {
    if (list_null(list))
        list_init(list);
    list_add(list, item);
}

static inline void list_remove(struct list *item) { // MKEMKE Check
    if (!list_null(item)) { // MKE, hack to work around thread coherency issues.  Global lock?  FIXME-MKE
        item->prev->next = item->next; // Crashed here 19 Nov 2022
        item->next->prev = item->prev;
       item->next = item->prev = NULL;
    }
}

static inline void list_remove_safe(struct list *item) {
    if (!list_null(item))
        list_remove(item);
}

// Given a pointer to a list node (ptr), the containing structure's type, and the name of the list member within 
// the containing structure, this macro calculates the starting address of the containing structure.
#define list_entry(item, type, member) \
    container_of(item, type, member)

// Retrieves the first entry from the list, which is the entry following the head of the list.
#define list_first_entry(list, type, member) \
    list_entry((list)->next, type, member)

#define list_next_entry(item, member) \
    list_entry((item)->member.next, typeof(*(item)), member)

// Iterates over each element of the list. The pos variable is the loop cursor, and head is the pointer to the list head.
#define list_for_each(list, item) \
    for (item = (list)->next; item != (list); item = item->next)

// Similar to list_for_each but safe against removal of the list entry during iteration. This is achieved by storing the next
// node in n before processing the current node.
#define list_for_each_safe(list, item, tmp) \
    for (item = (list)->next, tmp = item->next; item != (list); \
            item = tmp, tmp = item->next)
// Iterates over the list of given type, retrieving each entry. used for iterating over a list of structures.
#define list_for_each_entry(list, item, member) \
    for (item = list_entry((list)->next, typeof(*item), member); \
            &item->member != (list); \
            item = list_entry(item->member.next, typeof(*item), member))

// Do the same as above, in such a way that the current entry can be removed during iteration
#define list_for_each_entry_safe(list, item, tmp, member) \
    for (item = list_first_entry(list, typeof(*(item)), member), \
            tmp = list_next_entry(item, member); \
            &item->member != (list); \
            item = tmp, tmp = list_next_entry(item, member))

// Iterate and count
static inline unsigned long list_size(struct list *list) {
    unsigned long count = 0;
    struct list *item;
    list_for_each(list, item) {
        count++;
    }
    return count;
}

#endif

#endif

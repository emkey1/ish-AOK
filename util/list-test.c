// Unit test for util/list.h. Runs on the host: `meson test -C build list`.
//
// Two contracts the kernel leans on, and which look like they could be
// "simplified" away:
//
//   - A head that was never initialised -- a zeroed global, a calloc'd struct
//     walked before its list_init -- is an empty list. list_empty() always
//     said so; the walks faulted on it instead (the alive_pids_list crash, a
//     head walked before the guest booted). Every walk form must run no
//     iterations over one.
//   - A removed node is NULL, not self-pointing: list_null() is how a node's
//     owner learns it was removed (a futex waiter, that it was woken), and
//     re-adding is gated on it. The test pins that down so a change to
//     list_remove has to come through here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "util/list.h"

static int failures;

static void check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

struct item {
    int value;
    struct list link;
};

struct owner {
    int pad;
    struct list items;
};

static struct list zeroed_global;

// Every walk form over `head`: returns the number of iterations, and the
// values visited in order through `seen` (up to 8).
static int walk_all_forms(struct list *head, int seen[4][8]) {
    int counts[4] = {0, 0, 0, 0};
    struct list *node, *tmp;
    struct item *it, *it_tmp;

    list_for_each(head, node) {
        if (counts[0] < 8)
            seen[0][counts[0]] = list_entry(node, struct item, link)->value;
        counts[0]++;
    }
    list_for_each_safe(head, node, tmp) {
        if (counts[1] < 8)
            seen[1][counts[1]] = list_entry(node, struct item, link)->value;
        counts[1]++;
    }
    list_for_each_entry(head, it, link) {
        if (counts[2] < 8)
            seen[2][counts[2]] = it->value;
        counts[2]++;
    }
    list_for_each_entry_safe(head, it, it_tmp, link) {
        if (counts[3] < 8)
            seen[3][counts[3]] = it->value;
        counts[3]++;
    }
    for (int f = 1; f < 4; f++)
        check(counts[f] == counts[0], "every walk form visits the same number of nodes");
    return counts[0];
}

static void test_zeroed_heads(void) {
    int seen[4][8];

    check(list_null(&zeroed_global), "a static head starts NULL (the case under test)");
    check(list_empty(&zeroed_global), "a zeroed head is empty");
    check(walk_all_forms(&zeroed_global, seen) == 0, "walks over a zeroed global head run no iterations");
    check(list_size(&zeroed_global) == 0, "list_size of a zeroed head is 0");

    struct owner *o = calloc(1, sizeof(*o));
    check(walk_all_forms(&o->items, seen) == 0, "walks over a calloc'd head run no iterations");

    // ...and it is still usable once initialised, as the lazy-init sites do.
    list_init(&o->items);
    struct item a = {.value = 1};
    list_add_tail(&o->items, &a.link);
    check(walk_all_forms(&o->items, seen) == 1, "the same head after list_init walks its one node");
    free(o);
}

static void test_populated_list(void) {
    struct list head;
    list_init(&head);
    int seen[4][8];
    check(walk_all_forms(&head, seen) == 0, "an initialised empty head walks no nodes");

    struct item items[3] = {{.value = 10}, {.value = 20}, {.value = 30}};
    for (int i = 0; i < 3; i++)
        list_add_tail(&head, &items[i].link);
    memset(seen, 0, sizeof(seen));
    check(walk_all_forms(&head, seen) == 3, "a three-node list walks three nodes");
    for (int f = 0; f < 4; f++)
        check(seen[f][0] == 10 && seen[f][1] == 20 && seen[f][2] == 30, "in insertion order");

    // The _safe forms survive removing the current node.
    struct item *it, *tmp;
    list_for_each_entry_safe(&head, it, tmp, link) {
        if (it->value == 20)
            list_remove(&it->link);
    }
    memset(seen, 0, sizeof(seen));
    check(walk_all_forms(&head, seen) == 2, "removing the middle node in a _safe walk leaves two");
    check(seen[2][0] == 10 && seen[2][1] == 30, "the two that are left");
}

static void test_removed_node(void) {
    struct list head;
    list_init(&head);
    struct item a = {.value = 1};
    list_add(&head, &a.link);
    check(!list_null(&a.link), "a linked node is not null");

    list_remove(&a.link);
    check(list_null(&a.link), "a removed node is NULL, not self-pointing");
    check(list_empty(&head), "its list is empty again");

    list_remove(&a.link);
    check(list_null(&a.link) && list_empty(&head), "removing it again changes nothing");
    list_remove_safe(&a.link);
    check(list_null(&a.link) && list_empty(&head), "list_remove_safe on it changes nothing");

    // Re-adding a removed node, gated on list_null as sockrestart does, links
    // it once and not to itself.
    if (list_null(&a.link))
        list_add(&head, &a.link);
    check(head.next == &a.link && a.link.next == &head, "a removed node can be added again");
}

int main(void) {
    test_zeroed_heads();
    test_populated_list();
    test_removed_node();
    if (failures) {
        printf("list: %d failure(s)\n", failures);
        return 1;
    }
    printf("list: PASS\n");
    return 0;
}

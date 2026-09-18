/* kernel/include/compat/list.h — kernel's own copy of libc/list.h.
 *
 * Phase 2 #5 R2: aarch64 kernel no longer pulls <list.h> from
 * libc/include/. This is the kernel's own copy of the OS01
 * project's doubly-linked list implementation. FROZEN — when
 * modifying, update both this file AND libc/include/list.h
 * together; the kernel cannot use any list operation beyond
 * what libc/include/list.h exposes.
 *
 * The aarch64 build transitively pulls <list.h> from
 * kernel/include/time/timer.h:5 and the 9 other kernel
 * <list.h> consumers (per R1 finding 3). All resolve to this
 * file via the -I include/compat Makefile flag.
 */

#ifndef _KERNEL_COMPAT_LIST_H
#define _KERNEL_COMPAT_LIST_H

#include <sys/cdefs.h>
#include <stddef.h>

typedef struct List
{
    struct List * prev;
    struct List * next;
} list_t;

static inline void list_init(struct List * lst)
{
    lst->prev = lst;
    lst->next = lst;
}

static inline void list_add_to_behind(struct List * entry, struct List * new_entry)
{
    new_entry->next = entry->next;
    new_entry->prev = entry;
    new_entry->next->prev = new_entry;
    entry->next = new_entry;
}

static inline void list_add_to_before(struct List * entry, struct List * new_entry)
{
    new_entry->next = entry;
    new_entry->prev = entry->prev;
    entry->prev->next = new_entry;
    entry->prev = new_entry;
}

static inline void list_del(struct List * entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
}

// Delete entry from its list and re-initialize it (self-pointing).
// Safe to call on an already-dangling entry — list_del_init is
// idempotent (the second call is a no-op on a self-pointing node).
static inline void list_del_init(struct List * entry)
{
    list_del(entry);
    list_init(entry);
}

static inline long list_is_empty(struct List * entry)
{
    if (entry == entry->next && entry == entry->prev)
    {
        return 1;
    }
    return 0;
}

static inline struct List * list_prev(struct List * entry)
{
    if (entry->prev != NULL)
    {
        return entry->prev;
    }
    return NULL;
}

static inline struct List * list_next(struct List * entry)
{
    if (entry->next != NULL)
    {
        return entry->next;
    }
    return NULL;
}

#endif
#ifndef _LIST_H
#define _LIST_H

#include <sys/cdefs.h>
#include <stddef.h>

typedef struct List
{
    struct List * prev;
    struct List * next;
} list_t;

inline void list_init(struct List * lst)
{
    lst->prev = lst;
    lst->next = lst;
}

inline void list_add_to_behind(struct List * entry, struct List * new_entry)
{
    new_entry->next = entry->next;
    new_entry->prev = entry;
    new_entry->next->prev = new_entry;
    entry->next = new_entry;
}

inline void list_add_to_before(struct List * entry, struct List * new_entry)
{
    new_entry->next = entry;
    entry->prev->next = new_entry;
    new_entry->next->prev = entry;
    entry->prev = new_entry;
}

inline void list_del(struct List * entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
}

inline long list_is_empty(struct List * entry)
{
    if (entry == entry->next && entry == entry->prev)
    {
        return 1;
    }
    return 0;
}

inline struct List * list_prev(struct List * entry)
{
    if (entry->prev != NULL)
    {
        return entry->prev;
    }
    return NULL;
}

inline struct List * list_next(struct List * entry)
{
    if (entry->next != NULL)
    {
        return entry->next;
    }
    return NULL;
}

#endif
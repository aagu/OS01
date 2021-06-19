#include <list.h>

extern void list_init(struct List * lst);
extern void list_add_to_behind(struct List * entry, struct List * new_entry);
extern void list_add_to_before(struct List * entry, struct List * new_entry);
extern void list_del(struct List * entry);
extern long list_is_empty(struct List * entry);
extern struct List * list_prev(struct List * entry);
extern struct List * list_next(struct List * entry);
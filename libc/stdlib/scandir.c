/*
 * scandir.c — scandir(3) and alphasort(3) (T15).
 *
 * scandir() reads a whole directory and hands back an array of POINTERS
 * to INDEPENDENT COPIES of the entries.  The caller frees each entry and
 * then the array itself.
 *
 * Contract, matching glibc:
 *   - the return value is the number of entries KEPT (>= 0), or -1 with
 *     errno set;
 *   - *namelist is written ONLY on success.  On any failure the array is
 *     freed again and the caller's pointer is left untouched, so a
 *     half-finished scan is never handed out;
 *   - a NULL filter keeps every entry;
 *   - a NULL comparator keeps the kernel's (readdir) order;
 *   - an EMPTY directory is a success with 0 entries: the array holds
 *     just the terminating NULL.
 *
 * Every allocation failure releases everything that was allocated so far
 * — the copies, the array and the directory stream — and answers -1 with
 * errno == ENOMEM.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Release the copies and the array.  Used on every failure path. */
static void scandir_free_all(struct dirent **list, int count)
{
    int i;

    if (list) {
        for (i = 0; i < count; i++)
            free(list[i]);
        free(list);
    }
}

int scandir(const char *dirp,
            struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
    DIR *dir;
    struct dirent **list = NULL;
    int count = 0;
    int capacity = 0;
    int saved;

    if (!dirp || !namelist) {
        errno = EINVAL;
        return -1;
    }

    dir = opendir(dirp);
    if (!dir)
        return -1;          /* errno comes from opendir() */

    for (;;) {
        struct dirent *entry;
        struct dirent *copy;

        /* readdir() answers NULL both at the end of the directory and on
         * an error.  The end of the directory leaves errno UNCHANGED, so
         * errno is cleared before the call: a non-zero errno afterwards
         * means the kernel reported a failure (errno is never 0). */
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                saved = errno;
                scandir_free_all(list, count);
                closedir(dir);
                errno = saved;
                return -1;
            }
            break;
        }

        if (filter && !filter(entry))
            continue;

        if (count + 1 > capacity) {
            int newcap = capacity ? capacity * 2 : 16;
            struct dirent **grown;

            /* The +1 keeps room for the terminating NULL. */
            grown = (struct dirent **)realloc(list,
                                              (size_t)(newcap + 1) * sizeof(*list));
            if (!grown) {
                scandir_free_all(list, count);
                closedir(dir);
                errno = ENOMEM;
                return -1;
            }
            list = grown;
            capacity = newcap;
        }

        copy = (struct dirent *)malloc(sizeof(*copy));
        if (!copy) {
            scandir_free_all(list, count);
            closedir(dir);
            errno = ENOMEM;
            return -1;
        }
        memcpy(copy, entry, sizeof(*copy));
        list[count++] = copy;
    }

    /* An empty directory (or a filter that rejected everything) still
     * answers with an array that holds just the terminating NULL. */
    if (list == NULL) {
        list = (struct dirent **)malloc(sizeof(*list));
        if (!list) {
            closedir(dir);
            errno = ENOMEM;
            return -1;
        }
    }

    list[count] = NULL;

    if (compar)
        qsort(list, (size_t)count, sizeof(*list),
              (int (*)(const void *, const void *))compar);

    *namelist = list;
    return count;
}

int alphasort(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

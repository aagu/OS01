/* libc/stdlib/environ.c — the OS01 environment (T3).
 *
 * One unified table, one owner for every string.
 *
 *   - `environ` is the public array of "NAME=VALUE" strings.  It starts
 *     out as the block the kernel handed to crt0 and is replaced by an
 *     array the libc owns at the first mutation.
 *
 *   - A private array runs parallel to it and records, for every slot,
 *     the string the libc allocated for that slot (NULL when the slot
 *     holds a borrowed string).  That is the whole ownership rule: the
 *     inherited block and the strings a caller lends to putenv() are
 *     never freed, the copies setenv() makes are freed exactly once.
 *
 * The rule "the pointer table is copied at the first modification" also
 * makes the module work when an application replaces `environ` with an
 * array of its own (BusyBox's shell saves and restores `environ` around a
 * nofork applet): the libc notices that the public array is no longer
 * the one it allocated, drops its state and adopts the new array at the
 * next mutation.
 *
 * Every mutation is transactional: the string is allocated first, the
 * table is grown second, and only then is the new entry published.  A
 * failed allocation therefore leaves the environment exactly as it was.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef __is_libk
#include <errno.h>
#define ENV_ERRNO(code) (errno = (code))
#else
/* libk has no user errno global (libc/errno/errno.c is user-only), so the
 * kernel-side copy of this module reports a failure through its return
 * value alone. */
#define ENV_ERRNO(code) ((void)0)
#endif

/* The public environment array. */
char **environ = NULL;

/* The array the libc allocated, and the parallel array that records which
 * slot holds a string the libc allocated.  `environ == env_slots` means
 * the public array is the one we own and may modify in place. */
static char **env_slots;
static char **env_owned;
static size_t env_count;
static size_t env_cap;

/* An empty environment.  Used when there is no array to modify and no
 * allocation may fail (clearenv on an untouched environment). */
static char *env_empty[1] = { NULL };

/* The kernel bounds the inherited environment, but a malformed array must
 * not make the libc walk off the end of it. */
#define ENV_MAX_ENTRIES 4096

/* ── helpers ───────────────────────────────────────────────────────── */

/* A name is valid when it is not empty and carries no '='. */
static int env_name_valid(const char *name)
{
    size_t i;

    if (!name || name[0] == '\0')
        return 0;
    for (i = 0; name[i] != '\0'; i++)
        if (name[i] == '=')
            return 0;
    return 1;
}

/* Does the "NAME=VALUE" entry belong to `name`? */
static int env_entry_matches(const char *entry, const char *name, size_t name_len)
{
    return strncmp(entry, name, name_len) == 0 && entry[name_len] == '=';
}

/* The index of the first entry with this name, or (size_t)-1. */
static size_t env_find(const char *name, size_t name_len)
{
    size_t i;

    if (!environ)
        return (size_t)-1;
    for (i = 0; i < ENV_MAX_ENTRIES && environ[i] != NULL; i++)
        if (env_entry_matches(environ[i], name, name_len))
            return i;
    return (size_t)-1;
}

/* The number of entries in the current environment. */
static size_t env_current_count(void)
{
    size_t n = 0;

    if (!environ)
        return 0;
    while (n < ENV_MAX_ENTRIES && environ[n] != NULL)
        n++;
    return n;
}

/* Does slot `i` hold a string the libc allocated (and may therefore
 * free)?  The pointer comparison matters: an application is allowed to
 * write into the public array, and a slot that no longer holds the
 * string we allocated must not be freed. */
static int env_owns(size_t i)
{
    return env_owned != NULL && env_owned[i] != NULL &&
           env_owned[i] == env_slots[i];
}

/* ── taking over and growing the table ────────────────────────────── */

/* Copy the current pointer table into an array the libc owns.  The
 * strings themselves are NOT copied: they belong to the process image
 * (the crt0 stack block) or to the caller, and the libc must never free
 * them.
 *
 * `extra` is the number of entries the caller still wants to add. */
static int env_take_over(size_t extra)
{
    size_t n = env_current_count();
    size_t cap = n + extra + 1;          /* + 1 for the NULL terminator */
    char **slots;
    char **owned;
    size_t i;

    slots = malloc(cap * sizeof(char *));
    if (!slots)
        return -1;
    owned = malloc(cap * sizeof(char *));
    if (!owned) {
        free(slots);
        return -1;
    }
    for (i = 0; i < cap; i++) {
        slots[i] = NULL;
        owned[i] = NULL;
    }
    for (i = 0; i < n; i++)
        slots[i] = environ[i];

    /* The table this module allocated is released: POSIX lets a
     * setenv()/unsetenv() change the public array, so an application
     * that kept the old `environ` pointer has to re-read it (glibc's
     * realloc of the same array behaves the same way).
     *
     * The STRINGS the old table owned are NOT released: the array the
     * application installed may still refer to them, and getenv() hands
     * out a pointer INTO such a string.  Releasing them would hand the
     * application a dangling pointer, so they are deliberately leaked
     * here.  That is the one leak this module accepts, and it only
     * happens when the application replaces the public array itself. */
    free(env_owned);
    free(env_slots);
    env_slots = slots;
    env_owned = owned;
    env_count = n;
    env_cap = cap;
    return 0;
}

/* Grow the array the libc owns.  Both arrays are allocated before either
 * one is published, so a failed growth leaves the environment untouched. */
static int env_grow(size_t extra)
{
    size_t new_cap = env_cap ? env_cap * 2 : 8;
    char **slots;
    char **owned;
    char **old_slots = env_slots;
    char **old_owned = env_owned;
    size_t i;

    while (new_cap < env_count + extra + 1)
        new_cap *= 2;

    slots = malloc(new_cap * sizeof(char *));
    if (!slots)
        return -1;
    owned = malloc(new_cap * sizeof(char *));
    if (!owned) {
        free(slots);
        return -1;
    }
    for (i = 0; i < env_count; i++) {
        slots[i] = env_slots[i];
        owned[i] = env_owned[i];
    }
    for (i = env_count; i < new_cap; i++) {
        slots[i] = NULL;
        owned[i] = NULL;
    }
    /* Publish the new arrays first, then release the old ones: the
     * public pointer must never refer to freed memory, not even for the
     * few instructions between the two steps. */
    env_slots = slots;
    env_owned = owned;
    env_cap = new_cap;
    environ = slots;
    free(old_slots);
    free(old_owned);
    return 0;
}

/* Make sure the public array is one the libc may modify.  Returns 0 on
 * success and -1 when the allocation failed; in the failure case the
 * environment is unchanged. */
static int env_ensure_mutable(size_t extra)
{
    if (environ == env_slots) {
        if (env_count + extra + 1 > env_cap)
            return env_grow(extra);
        return 0;
    }
    return env_take_over(extra);
}

/* Remove every entry with this name, starting at slot `from`.  Only the
 * strings the libc allocated are freed: an inherited string and a string
 * the caller lent to putenv() belong to someone else.
 *
 * The table is edited, not the public array: the public array is
 * published once the mutation succeeded, so a failed mutation leaves the
 * environment exactly as it was. */
static void env_remove_from(size_t from, const char *name, size_t name_len)
{
    size_t i = from;

    while (i < env_count) {
        /* The application may write into the public array, so a slot can
         * be empty even though the libc still counts it.  The empty slot
         * is skipped: it holds no entry, and env_entry_matches() would
         * dereference it. */
        if (env_slots[i] != NULL &&
            env_entry_matches(env_slots[i], name, name_len)) {
            if (env_owns(i))
                free(env_owned[i]);
            for (size_t j = i; j + 1 < env_count; j++) {
                env_slots[j] = env_slots[j + 1];
                env_owned[j] = env_owned[j + 1];
            }
            env_count--;
            env_slots[env_count] = NULL;
            env_owned[env_count] = NULL;
            /* The entry that moved into slot i still has to be checked. */
        } else {
            i++;
        }
    }
}

/* ── getenv ───────────────────────────────────────────────────────── */

char *getenv(const char *name)
{
    size_t name_len;
    size_t i;

    if (!env_name_valid(name))
        return NULL;
    name_len = strlen(name);
    if (!environ)
        return NULL;
    for (i = 0; i < ENV_MAX_ENTRIES && environ[i] != NULL; i++)
        if (env_entry_matches(environ[i], name, name_len))
            return environ[i] + name_len + 1;
    return NULL;
}

/* ── setenv ───────────────────────────────────────────────────────── */

int setenv(const char *name, const char *value, int overwrite)
{
    size_t name_len;
    size_t value_len;
    size_t total;
    char *text;
    size_t slot;

    if (!env_name_valid(name) || !value) {
        ENV_ERRNO(EINVAL);
        return -1;
    }
    name_len = strlen(name);
    slot = env_find(name, name_len);
    /* overwrite == 0 and the variable is already there: nothing to do,
     * and nothing is allocated. */
    if (slot != (size_t)-1 && !overwrite)
        return 0;

    value_len = strlen(value);
    if (name_len > SIZE_MAX - value_len - 2) {
        ENV_ERRNO(ENOMEM);
        return -1;
    }
    total = name_len + value_len + 2;         /* "N" "=" "V" "\0" */
    text = malloc(total);
    if (!text) {
        ENV_ERRNO(ENOMEM);
        return -1;
    }
    memcpy(text, name, name_len);
    text[name_len] = '=';
    memcpy(text + name_len + 1, value, value_len + 1);

    if (env_ensure_mutable(slot == (size_t)-1 ? 1 : 0) != 0) {
        free(text);
        ENV_ERRNO(ENOMEM);
        return -1;
    }

    if (slot == (size_t)-1) {
        env_slots[env_count] = text;
        env_owned[env_count] = text;
        env_count++;
    } else {
        if (env_owns(slot))
            free(env_owned[slot]);
        env_slots[slot] = text;
        env_owned[slot] = text;
        /* One name, one entry: a duplicate inherited entry for the same
         * name goes away together with the overwrite. */
        env_remove_from(slot + 1, name, name_len);
    }
    env_slots[env_count] = NULL;
    env_owned[env_count] = NULL;
    environ = env_slots;          /* the mutation succeeded: publish it */
    return 0;
}

/* ── unsetenv ─────────────────────────────────────────────────────── */

int unsetenv(const char *name)
{
    size_t name_len;

    if (!env_name_valid(name)) {
        ENV_ERRNO(EINVAL);
        return -1;
    }
    name_len = strlen(name);
    if (env_find(name, name_len) == (size_t)-1)
        return 0;                    /* not present: nothing to do */

    if (env_ensure_mutable(0) != 0) {
        ENV_ERRNO(ENOMEM);
        return -1;
    }
    env_remove_from(0, name, name_len);
    environ = env_slots;          /* the mutation succeeded: publish it */
    return 0;
}

/* ── putenv ───────────────────────────────────────────────────────── */

int putenv(char *string)
{
    size_t eq = 0;
    size_t name_len;
    size_t slot;

    if (!string) {
        ENV_ERRNO(EINVAL);
        return -1;
    }
    while (string[eq] != '\0' && string[eq] != '=')
        eq++;
    if (string[eq] == '\0') {
        /* No '=': the request removes the variable. */
        return unsetenv(string);
    }
    if (eq == 0) {
        ENV_ERRNO(EINVAL);            /* an empty name */
        return -1;
    }
    name_len = eq;
    slot = env_find(string, name_len);

    if (env_ensure_mutable(slot == (size_t)-1 ? 1 : 0) != 0) {
        ENV_ERRNO(ENOMEM);
        return -1;
    }
    if (slot == (size_t)-1) {
        env_slots[env_count] = string;
        env_owned[env_count] = NULL;  /* borrowed from the caller */
        env_count++;
    } else {
        if (env_owns(slot))
            free(env_owned[slot]);
        env_slots[slot] = string;
        env_owned[slot] = NULL;        /* borrowed from the caller */
        env_remove_from(slot + 1, string, name_len);
    }
    env_slots[env_count] = NULL;
    env_owned[env_count] = NULL;
    environ = env_slots;          /* the mutation succeeded: publish it */
    return 0;
}

/* ── clearenv ─────────────────────────────────────────────────────── */

int clearenv(void)
{
    size_t i;

    if (environ == env_slots && env_slots != NULL) {
        /* The array is ours: free exactly the strings we allocated.  The
         * inherited strings and the strings the caller lent us stay. */
        for (i = 0; i < env_count; i++) {
            if (env_owns(i))
                free(env_owned[i]);
            env_owned[i] = NULL;
        }
        env_count = 0;
        environ[0] = NULL;
        return 0;
    }

    /* The current array is not ours.  Its strings are the inherited block
     * or belong to the application, and the strings we allocated may still
     * be referenced there, so nothing is freed: the state is dropped and
     * the environment starts empty. */
    free(env_owned);
    free(env_slots);
    env_owned = NULL;
    env_slots = NULL;
    env_count = 0;
    env_cap = 0;
    /* The empty array is the libc's own.  It is reset before it is
     * published again, so a write the application made into the
     * previous incarnation of it cannot survive into this one. */
    env_empty[0] = NULL;
    environ = env_empty;
    return 0;
}

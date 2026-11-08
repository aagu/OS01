/*
 * hosttests/cases/test_libc_environ.c — OS01 libc completion, env group (T3).
 *
 * The env group checks the OS01 environment's CONTRACTS:
 *
 *   setenv_visible             a variable set with setenv() is visible to
 *                              getenv() right away, and it is in the public
 *                              `environ` array
 *   overwrite_and_remove       overwrite=0 keeps the old value, overwrite=1
 *                              replaces it, unsetenv() removes it, and
 *                              removing a variable that is not there is not
 *                              an error
 *   preserve_inherited         the inherited entries survive a mutation
 *   putenv_borrowed            putenv() borrows the caller's string: the
 *                              libc sees the change and never frees it
 *   invalid_name               an empty name, a name with '=' and a NULL
 *                              value are EINVAL, and the environment is
 *                              untouched
 *   duplicate_inherited_unset  unsetenv() removes EVERY entry with that
 *                              name, not just the first one
 *   many_entries               40 variables and a 1024 byte value survive
 *   clearenv_then_set          clearenv() empties the environment without
 *                              freeing the inherited block, and the
 *                              environment can be built again afterwards
 *   allocation_failure_preserves
 *                              a failed allocation leaves the environment
 *                              and the old value exactly as they were
 *
 * The binary links the REAL production environment module
 * (libc/stdlib/environ.c) under the `os01_` prefix (see hosttests/mk/
 * libc.mk), so a call in this file can never resolve to the host libc's
 * same-named implementation.
 *
 * The allocator is the one boundary that is replaced: the module
 * allocates its strings and its table through malloc/free/realloc, and
 * the test supplies a ledger for those three entry points so a chosen
 * allocation can be made to fail and so the test can see which strings
 * the libc owns.
 */
#include <errno.h>
#include "test_framework.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* errno is a plain global in the OS01 libc; the host glibc does not
 * export one, so the test provides it (the production object
 * references it). */
int errno = 0;

/* ── the allocator boundary ─────────────────────────────────────────
 * Every allocation the environment module makes goes through this
 * ledger.  `env_alloc_fail_after(n)` lets the next n allocations
 * succeed and makes the one after them fail, which is how the test
 * observes a failed mutation. */
#define ENV_LEDGER_MAX 512

typedef struct {
    void  *ptr;
    size_t size;
} env_alloc_t;

static env_alloc_t env_ledger[ENV_LEDGER_MAX];
static size_t      env_ledger_count;
static int         env_ledger_overflow;
static int         env_alloc_countdown = -1;   /* -1: no failure pending */
static int         env_free_foreign;           /* frees of unknown pointers */

/* The three allocator entry points the production module calls.  They are
 * declared here because the production object is compiled with the
 * `os01_` prefix. */
void *os01_malloc(size_t size);
void  os01_free(void *ptr);
void *os01_realloc(void *ptr, size_t size);

/* Let `n` allocations succeed and make the next one fail. */
static void env_alloc_fail_after(int n)
{
    env_alloc_countdown = n;
}

/* Make the very next allocation fail. */
static void env_alloc_fail_next(void)
{
    env_alloc_fail_after(0);
}

void *os01_malloc(size_t size)
{
    void *p;

    if (env_alloc_countdown == 0) {
        env_alloc_countdown = -1;            /* fail once */
        return NULL;
    }
    if (env_alloc_countdown > 0)
        env_alloc_countdown--;

    p = malloc(size);
    if (!p)
        return NULL;
    if (env_ledger_count < ENV_LEDGER_MAX) {
        env_ledger[env_ledger_count].ptr = p;
        env_ledger[env_ledger_count].size = size;
        env_ledger_count++;
    } else {
        env_ledger_overflow++;
    }
    return p;
}

void os01_free(void *ptr)
{
    size_t i;

    if (!ptr)
        return;
    for (i = 0; i < env_ledger_count; i++) {
        if (env_ledger[i].ptr == ptr) {
            env_ledger[i] = env_ledger[--env_ledger_count];
            free(ptr);
            return;
        }
    }
    /* The libc released a pointer it never allocated: an inherited
     * string or a string the caller lent to putenv().  That is exactly
     * the bug this group exists to catch, so it is counted instead of
     * being passed to the host allocator. */
    env_free_foreign++;
}

void *os01_realloc(void *ptr, size_t size)
{
    void *next;
    size_t old = 0;
    size_t i;

    if (!ptr)
        return os01_malloc(size);
    for (i = 0; i < env_ledger_count; i++) {
        if (env_ledger[i].ptr == ptr) {
            old = env_ledger[i].size;
            break;
        }
    }
    next = os01_malloc(size);
    if (!next)
        return NULL;
    memcpy(next, ptr, old < size ? old : size);
    os01_free(ptr);
    return next;
}

/* ── the inherited environment ──────────────────────────────────────
 * The kernel hands crt0 a "NAME=VALUE" array on the stack.  The test
 * plays the part of crt0: it installs an array of its own and checks
 * that the libc treats those strings as borrowed memory. */
static char *env_inherited[8];

static void env_install(const char *const *entries)
{
    int n = 0;

    while (entries[n] != NULL && n < 7) {
        env_inherited[n] = (char *)entries[n];
        n++;
    }
    env_inherited[n] = NULL;
    environ = env_inherited;
}

/* The module allocates its table as TWO arrays of char* (the entries
 * and the ownership map).  Everything else it allocates is a string, and
 * every string must be released again — so the number of live
 * allocations at the end of a case is exactly these two arrays. */
#define ENV_TABLE_ARRAYS 2

/* Release everything the libc owns and start from a fresh inherited
 * environment.
 *
 * The foreign-free counter is reset here, so a free of a pointer the
 * libc never allocated is attributed to the case that caused it — and
 * not to whichever case happens to run next. */
static void env_reset(const char *const *entries)
{
    clearenv();
    env_install(entries);
    env_free_foreign = 0;
}

/* The end of a case.
 *
 * The cleanup first hands the public array back to the libc: through an
 * array the APPLICATION owns, the libc deliberately keeps the strings it
 * allocated, because that array may still refer to them.  Owning the
 * array again is what makes the cleanup release them, so a string the
 * libc allocated and forgot is still live when the count is checked. */
static void env_case_end(void)
{
    setenv("__env_probe__", "1", 1);
    clearenv();

    /* The libc must never free a pointer it did not allocate. */
    assert_eq(0, env_free_foreign);
    /* And it must give back every string it took. */
    assert_eq(ENV_TABLE_ARRAYS, env_ledger_count);
}

/* The value of a variable, or a marker when it is not set.  The marker
 * keeps a failed lookup from turning into a NULL dereference inside
 * the assertion. */
static const char *env_value(const char *name)
{
    const char *value = getenv(name);
    return value ? value : "<missing>";
}

/* Is this exact "NAME=VALUE" entry in the public array? */
static int env_has_entry(const char *entry)
{
    int i;

    if (!environ)
        return 0;
    for (i = 0; environ[i] != NULL; i++)
        if (strcmp(environ[i], entry) == 0)
            return 1;
    return 0;
}

static const char *const env_default[] = { "A=1", "B=2", NULL };
static const char *const env_dup[]    = { "A=1", "D=1", "D=2", NULL };

/* ── the cases ────────────────────────────────────────────────────── */

TEST_FUNC(setenv_visible)
{
    env_reset(env_default);

    assert_null(getenv("OS01_VAR"));
    assert_eq(0, setenv("OS01_VAR", "hello", 1));
    assert_str_eq("hello", env_value("OS01_VAR"));
    assert_true(env_has_entry("OS01_VAR=hello"));

    env_case_end();
}

TEST_FUNC(overwrite_and_remove)
{
    env_reset(env_default);

    assert_eq(0, setenv("K", "one", 1));
    assert_str_eq("one", env_value("K"));

    /* overwrite == 0 keeps the value that is already there */
    assert_eq(0, setenv("K", "two", 0));
    assert_str_eq("one", env_value("K"));

    /* overwrite != 0 replaces it */
    assert_eq(0, setenv("K", "two", 1));
    assert_str_eq("two", env_value("K"));
    assert_false(env_has_entry("K=one"));

    /* unsetenv removes it */
    assert_eq(0, unsetenv("K"));
    assert_null(getenv("K"));
    assert_false(env_has_entry("K=two"));

    /* removing a variable that is not there is not an error */
    assert_eq(0, unsetenv("K"));

    env_case_end();
}

TEST_FUNC(preserve_inherited)
{
    env_reset(env_default);

    assert_str_eq("1", env_value("A"));
    assert_str_eq("2", env_value("B"));

    /* a mutation must not drop the inherited entries */
    assert_eq(0, setenv("C", "3", 1));
    assert_str_eq("1", env_value("A"));
    assert_str_eq("2", env_value("B"));
    assert_str_eq("3", env_value("C"));

    assert_eq(0, putenv("D=4"));
    assert_str_eq("1", env_value("A"));
    assert_str_eq("2", env_value("B"));
    assert_str_eq("3", env_value("C"));
    assert_str_eq("4", env_value("D"));

    env_case_end();
}

TEST_FUNC(putenv_borrowed)
{
    static char borrowed[] = "BORROW=one";

    env_reset(env_default);

    assert_eq(0, putenv(borrowed));
    assert_str_eq("one", env_value("BORROW"));

    /* the caller owns the string: changing it changes what getenv sees */
    borrowed[7] = 'X';
    assert_str_eq("Xne", env_value("BORROW"));

    /* and the libc never frees a string it does not own */
    assert_eq(0, clearenv());
    assert_eq(0, env_free_foreign);
    env_case_end();
}

TEST_FUNC(invalid_name)
{
    env_reset(env_default);

    errno = 0;
    assert_eq(-1, setenv("", "v", 1));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(-1, setenv("A=B", "v", 1));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(-1, setenv("N", NULL, 1));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(-1, unsetenv(""));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(-1, unsetenv("A=B"));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(-1, putenv("=value"));
    assert_eq(EINVAL, errno);

    /* the environment is untouched */
    assert_str_eq("1", env_value("A"));
    assert_str_eq("2", env_value("B"));
    assert_false(env_has_entry("A=B"));

    env_case_end();
}

TEST_FUNC(duplicate_inherited_unset)
{
    env_reset(env_dup);

    /* the inherited array carries the same key twice */
    assert_str_eq("1", env_value("D"));
    assert_true(env_has_entry("D=1"));
    assert_true(env_has_entry("D=2"));

    /* unsetenv removes every entry with that name */
    assert_eq(0, unsetenv("D"));
    assert_null(getenv("D"));
    assert_false(env_has_entry("D=1"));
    assert_false(env_has_entry("D=2"));

    /* the other inherited entries survive */
    assert_str_eq("1", env_value("A"));

    env_case_end();
}

TEST_FUNC(many_entries)
{
    char name[8];
    char big[1025];
    int i;

    env_reset(env_default);

    for (i = 0; i < 40; i++) {
        name[0] = 'V';
        name[1] = 'A';
        name[2] = 'R';
        name[3] = (char)('0' + (i / 10) % 10);
        name[4] = (char)('0' + i % 10);
        name[5] = '\0';
        assert_eq(0, setenv(name, "x", 1));
    }
    for (i = 0; i < 40; i++) {
        name[3] = (char)('0' + (i / 10) % 10);
        name[4] = (char)('0' + i % 10);
        assert_str_eq("x", env_value(name));
    }

    /* a 1024 byte value */
    for (i = 0; i < 1024; i++)
        big[i] = 'v';
    big[1024] = '\0';
    assert_eq(0, setenv("BIG", big, 1));
    assert_str_eq(big, env_value("BIG"));

    env_case_end();
}

TEST_FUNC(clearenv_then_set)
{
    env_reset(env_default);

    assert_eq(0, setenv("X", "1", 1));
    assert_str_eq("1", env_value("X"));

    assert_eq(0, clearenv());
    assert_null(getenv("X"));
    assert_null(getenv("A"));
    assert_not_null(environ);
    if (environ)
        assert_null(environ[0]);         /* empty, not dangling */

    /* and the environment can be built again */
    assert_eq(0, setenv("Y", "2", 1));
    assert_str_eq("2", env_value("Y"));
    assert_null(getenv("A"));

    env_case_end();
}

TEST_FUNC(allocation_failure_preserves)
{
    char **env_before;
    size_t live_before;

    env_reset(env_default);
    assert_eq(0, setenv("KEEP", "old", 1));
    assert_str_eq("old", env_value("KEEP"));
    env_before = environ;
    live_before = env_ledger_count;

    /* the string allocation fails */
    env_alloc_fail_next();
    errno = 0;
    assert_eq(-1, setenv("KEEP", "new", 1));
    assert_eq(ENOMEM, errno);
    assert_str_eq("old", env_value("KEEP"));
    assert_true(environ == env_before);
    assert_eq(live_before, env_ledger_count);

    /* the table allocation fails: the new string is released again and
     * the environment is still the old one */
    env_alloc_fail_after(1);
    errno = 0;
    assert_eq(-1, setenv("OTHER", "new", 1));
    assert_eq(ENOMEM, errno);
    assert_str_eq("old", env_value("KEEP"));
    assert_true(environ == env_before);
    assert_eq(live_before, env_ledger_count);

    env_case_end();
}

TEST_LIST_BEGIN
    TEST_ENTRY(setenv_visible),
    TEST_ENTRY(overwrite_and_remove),
    TEST_ENTRY(preserve_inherited),
    TEST_ENTRY(putenv_borrowed),
    TEST_ENTRY(invalid_name),
    TEST_ENTRY(duplicate_inherited_unset),
    TEST_ENTRY(many_entries),
    TEST_ENTRY(clearenv_then_set),
    TEST_ENTRY(allocation_failure_preserves),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

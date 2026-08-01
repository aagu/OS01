#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ── environ ─────────────────────────────────────────────
// Global environment pointer.  Initialized to NULL (no env).
// Set by crt0.S or by setenv/getenv on first use.
char **environ = NULL;

// ── getenv ──────────────────────────────────────────────
char *getenv(const char *name)
{
    if (!environ || !name) return NULL;

    size_t name_len = strlen(name);
    for (char **ep = environ; *ep != NULL; ep++) {
        // Check if *ep starts with "name="
        if (strncmp(*ep, name, name_len) == 0 && (*ep)[name_len] == '=') {
            return (*ep) + name_len + 1;  // value after '='
        }
    }
    return NULL;
}

// ── setenv ──────────────────────────────────────────────
// Very simple: can only set the first few env vars (static storage).
// A real implementation would use malloc.
#define MAX_ENV 32
static char env_buf[MAX_ENV][256];
static int  env_count = 0;

int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !value) return -1;

    // Check if name already exists
    char *existing = getenv(name);
    if (existing && !overwrite) return 0;

    // Check capacity
    if (env_count >= MAX_ENV) return -1;

    // Build "NAME=VALUE"
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    if (nlen + 1 + vlen >= 255) return -1;

    // We don't implement overwrite of existing dynamically — just add new
    memcpy(env_buf[env_count], name, nlen);
    env_buf[env_count][nlen] = '=';
    memcpy(env_buf[env_count] + nlen + 1, value, vlen + 1);
    env_count++;

    return 0;
}

// ── clearenv ─────────────────────────────────────────────
int clearenv(void)
{
    environ = NULL;
    return 0;
}

/* ── putenv / unsetenv ── */

int unsetenv(const char *name) { (void)name; return 0; }

int putenv(char *string)
{
    static char **env_table = NULL;
    static int env_capacity = 0;
    static int env_count = 0;
    char *eq;

    if (!string || !(eq = strchr(string, '=')))
        return -1;

    size_t key_len = (size_t)(eq - string);

    /* Find existing key → replace */
    for (int i = 0; i < env_count; i++) {
        if (strncmp(env_table[i], string, key_len) == 0
            && env_table[i][key_len] == '=') {
            env_table[i] = string;
            return 0;
        }
    }

    /* New entry: need env_count + 2 slots (new entry + NULL terminator) */
    if (env_count + 2 > env_capacity) {
        int new_cap = env_capacity ? env_capacity * 2 : 16;
        char **new_table = realloc(env_table, new_cap * sizeof(char *));
        if (!new_table)
            return -1;
        env_table = new_table;
        env_capacity = new_cap;
    }

    env_table[env_count++] = string;
    env_table[env_count] = NULL;
    environ = env_table;

    return 0;
}

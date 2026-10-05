/*
 * test_slab_lock_path.c — hosttest that verifies slab.c no longer uses
 * inline pushfq/cli/sti/popfq for the lock path (v4 fix for v3 review
 * item 12: match inline asm strings only, not bare substrings).
 *
 * Source-level scan via grep; matches strings of the form
 *   __asm__ __volatile__("... token ...")
 * where `token` is one of: pushfq, cli, sti, popfq.
 */
#include "test_framework.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OS01_KERNEL_SRC
#error "Define OS01_KERNEL_SRC to the kernel source root (e.g. -DOS01_KERNEL_SRC=\"/path/to/repo\")"
#endif

#define SLAB_C_PATH OS01_KERNEL_SRC "/kernel/memory/slab.c"

static char *slab_text = NULL;

/* Slurp whole file into a malloc'd NUL-terminated buffer.
 * Uses the fread-grow pattern (matches test_gic_marker_lines.c) because
 * the OS01 libc/include/stdio.h shim doesn't expose fseek/ftell/SEEK_END/
 * SEEK_SET — only fopen, fread, fclose, fseeko. */
static int read_file(const char *path, char **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 4096;
    size_t n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fclose(f); return -1; }
    for (;;) {
        if (n + 1024 > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) { free(buf); fclose(f); return -1; }
            buf = nb;
            cap = new_cap;
        }
        size_t got = fread(buf + n, 1, 1024, f);
        n += got;
        if (got < 1024) break;  /* EOF or error */
    }
    fclose(f);
    buf[n] = '\0';
    *out = buf;
    return 0;
}

/* Returns 1 if any `__asm__ __volatile__("...")` block contains the token
 * as a bare word (i.e., whitespace or quote boundary either side).
 * v4 fix for v3 review item 12: only matches inside asm string literals. */
static int asm_string_contains_token(const char *src, const char *token) {
    const char *p = src;
    while ((p = strstr(p, "__asm__")) != NULL) {
        const char *volatile_kw = strstr(p, "__volatile__");
        if (!volatile_kw) break;
        const char *q = strchr(volatile_kw, '"');
        if (!q) { p = volatile_kw + 1; continue; }
        q++;  /* past opening quote */
        const char *end = strchr(q, '"');
        if (!end) { p = q; continue; }
        /* search token as a whole word inside [q, end) */
        size_t tlen = strlen(token);
        for (const char *s = q; s + tlen <= end; s++) {
            if (strncmp(s, token, tlen) != 0) continue;
            int left_ok  = (s == q) || (!isalnum((unsigned char)s[-1]) && s[-1] != '_');
            int right_ok = (s + tlen == end) || (!isalnum((unsigned char)s[tlen]) && s[tlen] != '_');
            if (left_ok && right_ok) return 1;
        }
        p = end + 1;
    }
    return 0;
}

static int contains_arch_irq_call(const char *src, const char *fn) {
    return strstr(src, fn) != NULL ? 1 : 0;
}

TEST_FUNC(test_slab_lock_path_no_inline_asm_tokens) {
    if (read_file(SLAB_C_PATH, &slab_text) != 0) {
        printf("  (cannot read %s; skipping)\n", SLAB_C_PATH);
        return;
    }
    assert_eq(asm_string_contains_token(slab_text, "pushfq"), 0);
    assert_eq(asm_string_contains_token(slab_text, "popfq"),  0);
    /* cli/sti are forbidden in asm strings but may appear in comments;
     * the bare-substring scan would false-positive on identifiers like
     * "client" or "testing"; the asm-string matcher scopes correctly. */
    assert_eq(asm_string_contains_token(slab_text, "cli"), 0);
    assert_eq(asm_string_contains_token(slab_text, "sti"), 0);
}

TEST_FUNC(test_slab_lock_path_uses_arch_irq_api) {
    if (!slab_text) {
        if (read_file(SLAB_C_PATH, &slab_text) != 0) {
            printf("  (cannot read %s; skipping)\n", SLAB_C_PATH);
            return;
        }
    }
    assert_eq(contains_arch_irq_call(slab_text, "arch_local_irq_save"),  1);
    assert_eq(contains_arch_irq_call(slab_text, "arch_local_irq_restore"), 1);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_slab_lock_path_no_inline_asm_tokens),
    TEST_ENTRY(test_slab_lock_path_uses_arch_irq_api),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

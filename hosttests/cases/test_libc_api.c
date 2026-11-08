/*
 * hosttests/cases/test_libc_api.c — OS01 libc completion, api group (T1).
 *
 * The api group checks the PUBLIC HEADER contract of the OS01 libc:
 *
 *   api_header_self_contained  every public header compiles on its own
 *                              (it declares everything it uses)
 *   api_two_translation_units  two translation units that include the
 *                              same header link together: the header has
 *                              exactly one definition, no duplicates
 *   api_cpp_linkage            a C++ consumer sees the declarations with C
 *                              linkage, and a C++ object links against the
 *                              production C symbol
 *   api_regex_stub_baseline    the current regex.h stub still reports a
 *                              match for a non-matching input.  This is the
 *                              T1 behaviour RED baseline; T16 replaces the
 *                              expectation when regex is migrated.
 *
 * The harness drives the host compiler: it writes probe sources into the
 * profile build directory and compiles/links them.  All compiler
 * invocations of one kind are batched into a single process so the whole
 * group stays fast; a batch failure is re-run per probe to attribute it.
 *
 * The probe sources include the OS01 headers only — the host's own
 * <stdio.h> and friends are shadowed by `-I libc/include`, so a probe can
 * never silently test the host libc's headers.
 *
 * The test binary itself links the REAL production libc string objects
 * under their `os01_` prefix (see hosttests/mk/common.mk), so a call in
 * this file can never resolve to the host libc's same-named
 * implementation.
 */
#include "test_framework.h"
#include "libc_completion_runtime.h"
#include "libc_api_manifest.h"

#include <stdarg.h>

#ifndef OS01_API_CC
#error "OS01_API_CC must be defined by the build (host C compiler)"
#endif
#ifndef OS01_API_CXX
#error "OS01_API_CXX must be defined by the build (host C++ compiler)"
#endif
#ifndef OS01_API_NM
#error "OS01_API_NM must be defined by the build (nm)"
#endif
#ifndef OS01_API_INCLUDE_DIR
#error "OS01_API_INCLUDE_DIR must be defined by the build"
#endif
#ifndef OS01_API_PROBE_DIR
#error "OS01_API_PROBE_DIR must be defined by the build"
#endif
#ifndef OS01_API_PREFIX_DEFS
#define OS01_API_PREFIX_DEFS ""
#endif
#ifndef OS01_API_PREFIXED_OBJS
#define OS01_API_PREFIXED_OBJS ""
#endif

#define API_CMD_MAX   8192
#define API_TEXT_MAX  4096
#define API_SRC_MAX   16384
#define API_SYM_MAX   4096

/* ── small helpers ─────────────────────────────────────────────────── */

/* Write a probe source file.  Returns 0 on success. */
static int api_write_file(const char *path, const char *text)
{
    void *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    if (fputs(text, f) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Slurp a file into buf.  Returns the byte count, or -1 on error. */
static int api_read_file(const char *path, char *buf, size_t buflen)
{
    size_t total = 0;
    void *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    while (total + 1 < buflen) {
        size_t got = fread(buf + total, 1, buflen - total - 1, f);
        if (got == 0) {
            break;
        }
        total += got;
    }
    buf[total] = '\0';
    fclose(f);
    return (int)total;
}

/* Run a shell command.  Returns the command's exit status, or -1 when the
 * command could not be started at all. */
static int api_run(const char *cmd)
{
    int raw = system(cmd);
    if (raw < 0) {
        return -1;
    }
    if ((raw & 0x7f) == 0) {
        return (raw >> 8) & 0xff;
    }
    return 128 + (raw & 0x7f);
}

/* snprintf into a bounded buffer. */
static void api_fmt(char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    dst[n - 1] = '\0';
}

/* True when `needle` occurs in `hay`. */
static int api_contains(const char *hay, const char *needle)
{
    size_t nlen = 0;
    size_t hlen = 0;
    size_t i;

    if (!hay || !needle) {
        return 0;
    }
    while (needle[nlen]) {
        nlen++;
    }
    while (hay[hlen]) {
        hlen++;
    }
    if (nlen == 0 || nlen > hlen) {
        return 0;
    }
    for (i = 0; i + nlen <= hlen; i++) {
        size_t k = 0;
        while (k < nlen && hay[i + k] == needle[k]) {
            k++;
        }
        if (k == nlen) {
            return 1;
        }
    }
    return 0;
}

/* ── case 1: every public header is self-contained ─────────────────── */

static void api_header_self_contained(void)
{
    char cmd[API_CMD_MAX];
    int batch;
    int i;

    TEST_SUITE("api_header_self_contained");

    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        char path[API_TEXT_MAX];
        char body[API_TEXT_MAX];
        api_fmt(path, sizeof(path), "%s/self_%d.c", OS01_API_PROBE_DIR, i);
        api_fmt(body, sizeof(body),
                "#include <%s>\n"
                "int os01_api_self_%d(void) { return 0; }\n",
                os01_api_headers[i].header, i);
        if (api_write_file(path, body) != 0) {
            printf("  [FAIL] cannot write probe %s\n", path);
            assert_true(0);
            return;
        }
    }

    api_fmt(cmd, sizeof(cmd), "cd %s && %s -std=c11 -fsyntax-only -I%s",
            OS01_API_PROBE_DIR, OS01_API_CC, OS01_API_INCLUDE_DIR);
    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " self_%d.c", i);
    }
    batch = api_run(cmd);

    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        int rc;
        if (batch == 0) {
            rc = 0;
        } else {
            char one[API_CMD_MAX];
            api_fmt(one, sizeof(one),
                    "cd %s && %s -std=c11 -fsyntax-only -I%s self_%d.c",
                    OS01_API_PROBE_DIR, OS01_API_CC, OS01_API_INCLUDE_DIR, i);
            rc = api_run(one);
        }
        if (rc != 0) {
            printf("  [FAIL] %s is not self-contained (compiler exit %d)\n",
                   os01_api_headers[i].header, rc);
        }
        assert_eq(0, rc);
    }

    /* Negative control: a probe that includes a header which does not
     * exist MUST fail to compile.  If it succeeds the harness cannot see
     * a failure and every positive result above would be worthless. */
    {
        char path[API_TEXT_MAX];
        char body[API_TEXT_MAX];
        int rc;
        api_fmt(path, sizeof(path), "%s/neg_missing.c", OS01_API_PROBE_DIR);
        api_fmt(body, sizeof(body),
                "#include <os01_no_such_public_header.h>\n"
                "int os01_api_neg_missing(void) { return 0; }\n");
        api_write_file(path, body);
        api_fmt(cmd, sizeof(cmd),
                "cd %s && %s -std=c11 -fsyntax-only -I%s neg_missing.c",
                OS01_API_PROBE_DIR, OS01_API_CC, OS01_API_INCLUDE_DIR);
        rc = api_run(cmd);
        if (rc == 0) {
            printf("  [FAIL] negative control compiled: the harness cannot "
                   "detect a compile failure\n");
        }
        assert_true(rc != 0);
    }
}

/* ── case 2: one definition across translation units ──────────────── */

static void api_two_translation_units(void)
{
    char cmd[API_CMD_MAX];
    int batch;
    int i;

    TEST_SUITE("api_two_translation_units");

    {
        char path[API_TEXT_MAX];
        api_fmt(path, sizeof(path), "%s/twu_main.c", OS01_API_PROBE_DIR);
        api_write_file(path, "int main(void) { return 0; }\n");
    }

    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        char path[API_TEXT_MAX];
        char body[API_TEXT_MAX];
        api_fmt(path, sizeof(path), "%s/twu_a_%d.c", OS01_API_PROBE_DIR, i);
        api_fmt(body, sizeof(body),
                "#include <%s>\n"
                "int os01_api_twu_a_%d(void) { return 1; }\n",
                os01_api_headers[i].header, i);
        api_write_file(path, body);
        api_fmt(path, sizeof(path), "%s/twu_b_%d.c", OS01_API_PROBE_DIR, i);
        api_fmt(body, sizeof(body),
                "#include <%s>\n"
                "int os01_api_twu_b_%d(void) { return 2; }\n",
                os01_api_headers[i].header, i);
        api_write_file(path, body);
    }

    api_fmt(cmd, sizeof(cmd), "cd %s && %s -std=c11 -c -I%s",
            OS01_API_PROBE_DIR, OS01_API_CC, OS01_API_INCLUDE_DIR);
    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                " twu_a_%d.c twu_b_%d.c", i, i);
    }
    api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " twu_main.c");
    batch = api_run(cmd);

    if (batch != 0) {
        /* Attribute the failure to a header by compiling one pair. */
        for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
            char one[API_CMD_MAX];
            api_fmt(one, sizeof(one),
                    "cd %s && %s -std=c11 -c -I%s twu_a_%d.c twu_b_%d.c",
                    OS01_API_PROBE_DIR, OS01_API_CC, OS01_API_INCLUDE_DIR, i, i);
            if (api_run(one) != 0) {
                printf("  [FAIL] %s does not compile in two translation units\n",
                       os01_api_headers[i].header);
                assert_true(0);
            }
        }
        assert_true(0);
        return;
    }

    /* Link every pair into one program: a header that defines a symbol
     * twice would show up as a duplicate symbol here. */
    api_fmt(cmd, sizeof(cmd), "cd %s && %s -o twu_all",
            OS01_API_PROBE_DIR, OS01_API_CC);
    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                " twu_a_%d.o twu_b_%d.o", i, i);
    }
    api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " twu_main.o");
    {
        int rc = api_run(cmd);
        if (rc != 0) {
            printf("  [FAIL] the two-translation-unit link failed (exit %d): "
                   "a header defines a symbol more than once\n", rc);
        }
        assert_eq(0, rc);
    }

    /* Negative control: two translation units that define the same global
     * symbol MUST fail to link. */
    {
        char path[API_TEXT_MAX];
        int rc;
        api_fmt(path, sizeof(path), "%s/neg_dup_a.c", OS01_API_PROBE_DIR);
        api_write_file(path, "int os01_api_neg_dup(void) { return 1; }\n");
        api_fmt(path, sizeof(path), "%s/neg_dup_b.c", OS01_API_PROBE_DIR);
        api_write_file(path, "int os01_api_neg_dup(void) { return 2; }\n");
        api_fmt(cmd, sizeof(cmd),
                "cd %s && %s -std=c11 -c neg_dup_a.c neg_dup_b.c && "
                "%s -o neg_dup neg_dup_a.o neg_dup_b.o",
                OS01_API_PROBE_DIR, OS01_API_CC, OS01_API_CC);
        rc = api_run(cmd);
        if (rc == 0) {
            printf("  [FAIL] negative control linked: the harness cannot "
                   "detect a duplicate definition\n");
        }
        assert_true(rc != 0);
    }
}

/* ── case 3: C++ linkage ──────────────────────────────────────────── */

/* Per-object symbol names collected from nm, newline separated. */
static char api_obj_syms[OS01_API_HEADER_COUNT][API_SYM_MAX];

static int api_parse_nm(const char *path)
{
    static char buf[1 << 18];
    int len = api_read_file(path, buf, sizeof(buf));
    int i;

    if (len < 0) {
        return -1;
    }
    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        api_obj_syms[i][0] = '\0';
    }

    i = 0;
    while (i < len) {
        int start = i;
        int end;
        char *obj;
        char *sep;
        int idx = -1;

        while (i < len && buf[i] != '\n') {
            i++;
        }
        end = i;
        if (i < len) {
            i++;
        }
        buf[end] = '\0';

        /* nm --print-file-name prefixes EVERY line with "<file>:" */
        obj = buf + start;
        sep = obj;
        while (*sep && !(sep[0] == '.' && sep[1] == 'o' && sep[2] == ':')) {
            sep++;
        }
        if (*sep == '\0') {
            continue;
        }
        *sep = '\0';
        sep += 3;

        if (obj[0] == 'c' && obj[1] == 'p' && obj[2] == 'p' && obj[3] == '_') {
            int n = 0;
            const char *q = obj + 4;
            int ok = 1;
            while (*q >= '0' && *q <= '9') {
                n = n * 10 + (*q - '0');
                q++;
            }
            if (*q != '\0') {
                ok = 0;
            }
            if (ok && n >= 0 && n < OS01_API_HEADER_COUNT) {
                idx = n;
            }
        }
        if (idx < 0) {
            continue;
        }

        /* the symbol name is the last whitespace separated field */
        {
            const char *name = sep;
            const char *p2 = sep;
            const char *last = sep;
            while (*p2) {
                if (*p2 == ' ' || *p2 == '\t') {
                    last = p2 + 1;
                }
                p2++;
            }
            name = last;
            if (*name) {
                size_t used = 0;
                while (used < API_SYM_MAX - 1 && api_obj_syms[idx][used]) {
                    used++;
                }
                if (used + strlen(name) + 2 < API_SYM_MAX) {
                    api_obj_syms[idx][used] = '\n';
                    memcpy(&api_obj_syms[idx][used + 1], name, strlen(name) + 1);
                }
            }
        }
    }
    return 0;
}

/* True when the newline separated symbol list contains `name` as a whole
 * field. */
static int api_sym_list_has(const char *list, const char *name)
{
    size_t nlen = 0;
    while (name[nlen]) {
        nlen++;
    }
    while (*list) {
        const char *eol = list;
        while (*eol && *eol != '\n') {
            eol++;
        }
        if ((size_t)(eol - list) == nlen) {
            size_t k;
            for (k = 0; k < nlen; k++) {
                if (list[k] != name[k]) {
                    break;
                }
            }
            if (k == nlen) {
                return 1;
            }
        }
        list = *eol ? eol + 1 : eol;
    }
    return 0;
}

/* True when `sym` is the Itanium local-name encoding of `name`
 * ("_ZL<len><name><params>").  Such a symbol means the header DEFINES the
 * function itself with internal linkage: the declaration's language
 * linkage is then unobservable, and the C view behaves the same way (a
 * static function is local there too).  It is NOT a missing extern "C". */
static int api_is_local_mangling_of(const char *sym, const char *name)
{
    size_t len = 0;
    size_t nlen = 0;
    size_t i = 3;

    if (!sym || sym[0] != '_' || sym[1] != 'Z' || sym[2] != 'L') {
        return 0;
    }
    if (sym[i] < '0' || sym[i] > '9') {
        return 0;
    }
    while (sym[i] >= '0' && sym[i] <= '9') {
        len = len * 10 + (size_t)(sym[i] - '0');
        i++;
    }
    while (name[nlen]) {
        nlen++;
    }
    if (len != nlen) {
        return 0;
    }
    for (size_t k = 0; k < nlen; k++) {
        if (sym[i + k] != name[k]) {
            return 0;
        }
    }
    return 1;
}

/* True when the newline separated symbol list holds a local definition
 * of `name`. */
static int api_sym_list_has_local(const char *list, const char *name)
{
    while (*list) {
        const char *eol = list;
        while (*eol && *eol != '\n') {
            eol++;
        }
        if (api_is_local_mangling_of(list, name)) {
            return 1;
        }
        list = *eol ? eol + 1 : eol;
    }
    return 0;
}

static void api_cpp_linkage(void)
{
    char cmd[API_CMD_MAX];
    int batch;
    int i;
    unsigned j;

    TEST_SUITE("api_cpp_linkage");

    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        char path[API_TEXT_MAX];
        char body[API_SRC_MAX];
        size_t used = 0;
        int rc;

        api_fmt(path, sizeof(path), "%s/cpp_%d.cpp", OS01_API_PROBE_DIR, i);
        api_fmt(body, sizeof(body), "#include <%s>\n",
                os01_api_headers[i].header);
        used = strlen(body);
        for (j = 0; j < os01_api_headers[i].nsymbols; j++) {
            const char *sym = os01_api_headers[i].symbols[j];
            /* Taking the address of every declared function forces the
             * compiler to emit a reference; the symbol name in the object
             * file then shows whether the declaration kept C linkage. */
            rc = snprintf(body + used, sizeof(body) - used,
                          "__attribute__((used)) static auto "
                          "os01_api_cpp_ref_%u = &%s;\n",
                          j, sym);
            if (rc < 0 || (size_t)rc >= sizeof(body) - used) {
                printf("  [FAIL] the C++ probe for %s does not fit in the "
                       "probe buffer\n", os01_api_headers[i].header);
                assert_true(0);
                break;
            }
            used += (size_t)rc;
        }
        api_write_file(path, body);
    }

    api_fmt(cmd, sizeof(cmd), "cd %s && %s -std=c++17 -c -x c++ -I%s",
            OS01_API_PROBE_DIR, OS01_API_CXX, OS01_API_INCLUDE_DIR);
    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " cpp_%d.cpp", i);
    }
    batch = api_run(cmd);
    if (batch != 0) {
        for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
            char one[API_CMD_MAX];
            api_fmt(one, sizeof(one),
                    "cd %s && %s -std=c++17 -fsyntax-only -x c++ -I%s cpp_%d.cpp",
                    OS01_API_PROBE_DIR, OS01_API_CXX, OS01_API_INCLUDE_DIR, i);
            if (api_run(one) != 0) {
                printf("  [FAIL] %s is not a valid C++ header\n",
                       os01_api_headers[i].header);
                assert_true(0);
            }
        }
        assert_true(0);
        return;
    }

    /* Collect the symbols each C++ object references. */
    api_fmt(cmd, sizeof(cmd), "cd %s && %s --print-file-name",
            OS01_API_PROBE_DIR, OS01_API_NM);
    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " cpp_%d.o", i);
    }
    api_fmt(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " > nm_api.txt");
    if (api_run(cmd) != 0) {
        printf("  [FAIL] nm could not read the C++ probe objects\n");
        assert_true(0);
        return;
    }
    {
        char path[API_TEXT_MAX];
        api_fmt(path, sizeof(path), "%s/nm_api.txt", OS01_API_PROBE_DIR);
        if (api_parse_nm(path) != 0) {
            printf("  [FAIL] cannot read the nm report\n");
            assert_true(0);
            return;
        }
    }

    for (i = 0; i < OS01_API_HEADER_COUNT; i++) {
        for (j = 0; j < os01_api_headers[i].nsymbols; j++) {
            const char *sym = os01_api_headers[i].symbols[j];
            if (api_sym_list_has(api_obj_syms[i], sym)) {
                continue;   /* referenced with its plain C name */
            }
            if (api_sym_list_has_local(api_obj_syms[i], sym)) {
                continue;   /* the header defines it itself (internal) */
            }
            if (api_contains(api_obj_syms[i], sym)) {
                printf("  [FAIL] %s: %s has C++ linkage (missing "
                       "extern \"C\")\n", os01_api_headers[i].header, sym);
            } else {
                printf("  [FAIL] %s: %s is not referenced by the C++ probe\n",
                       os01_api_headers[i].header, sym);
            }
            assert_true(0);
        }
    }

    /* Link a C++ object against a C object: the C++ side calls a function
     * declared by a public header, and the reference must resolve to the
     * production C symbol. */
    {
        char path[API_TEXT_MAX];
        int rc;
        api_fmt(path, sizeof(path), "%s/interop_c.c", OS01_API_PROBE_DIR);
        api_write_file(path,
                       "#include <stdio.h>\n"
                       "int os01_api_cpp_value(void) { return 41; }\n"
                       "int os01_api_cpp_entry(void);\n"
                       "int main(void) { return os01_api_cpp_entry() == 44 ? 0 : 1; }\n");
        api_fmt(path, sizeof(path), "%s/interop_cpp.cpp", OS01_API_PROBE_DIR);
        api_write_file(path,
                       "#include <stdio.h>\n"
                       "#include <string.h>\n"
                       "extern \"C\" int os01_api_cpp_value(void);\n"
                       "extern \"C\" int os01_api_cpp_entry(void)\n"
                       "{\n"
                       "    return os01_api_cpp_value() + (int)os01_strlen(\"abc\");\n"
                       "}\n");
        api_fmt(cmd, sizeof(cmd),
                "cd %s && %s -std=c11 -c -I%s interop_c.c && "
                "%s -std=c++17 -c -x c++ %s -I%s interop_cpp.cpp && "
                "%s -o interop interop_c.o interop_cpp.o %s",
                OS01_API_PROBE_DIR,
                OS01_API_CC, OS01_API_INCLUDE_DIR,
                OS01_API_CXX, OS01_API_PREFIX_DEFS, OS01_API_INCLUDE_DIR,
                OS01_API_CC, OS01_API_PREFIXED_OBJS);
        rc = api_run(cmd);
        if (rc != 0) {
            printf("  [FAIL] the C/C++ interop link failed (exit %d)\n", rc);
        }
        assert_eq(0, rc);

        api_fmt(cmd, sizeof(cmd), "cd %s && ./interop", OS01_API_PROBE_DIR);
        rc = api_run(cmd);
        if (rc != 0) {
            printf("  [FAIL] the C/C++ interop program did not agree "
                   "(exit %d)\n", rc);
        }
        assert_eq(0, rc);
    }

    /* Negative control: a C++ probe that uses a function the header does
     * not declare MUST fail to compile. */
    {
        char path[API_TEXT_MAX];
        int rc;
        api_fmt(path, sizeof(path), "%s/neg_cpp.cpp", OS01_API_PROBE_DIR);
        api_write_file(path,
                       "#include <stdio.h>\n"
                       "int main() { return (int)os01_no_such_function(); }\n");
        api_fmt(cmd, sizeof(cmd),
                "cd %s && %s -std=c++17 -fsyntax-only -x c++ -I%s neg_cpp.cpp",
                OS01_API_PROBE_DIR, OS01_API_CXX, OS01_API_INCLUDE_DIR);
        rc = api_run(cmd);
        if (rc == 0) {
            printf("  [FAIL] negative control compiled: the harness cannot "
                   "detect a C++ compile failure\n");
        }
        assert_true(rc != 0);
    }
}

/* ── case 4: the current regex.h stub (T1 behaviour RED baseline) ─── */

static void api_regex_stub_baseline(void)
{
    char cmd[API_CMD_MAX];
    char path[API_TEXT_MAX];
    int rc;

    TEST_SUITE("api_regex_stub_baseline");

    /* A single translation unit that uses the CURRENT regex.h.  The
     * current header does not include <stddef.h> itself, so the probe
     * supplies it; the header defines the functions inline, which is
     * exactly why regex.h cannot take part in the two-translation-unit
     * and C++ checks before T16 migrates it. */
    api_fmt(path, sizeof(path), "%s/regex_probe.c", OS01_API_PROBE_DIR);
    api_write_file(path,
                   "#include <stddef.h>\n"
                   "#include <regex.h>\n"
                   "#include <stdio.h>\n"
                   "int main(void)\n"
                   "{\n"
                   "    regex_t preg;\n"
                   "    int rc = regcomp(&preg, \"^abc$\", 0);\n"
                   "    int r = regexec(&preg, \"hello\", 0, (regmatch_t *)0, 0);\n"
                   "    printf(\"regex-stub: regcomp=%d regexec=%d\\n\", rc, r);\n"
                   "    /* T1 baseline: a NON-matching input still reports a\n"
                   "     * match.  T16 replaces this expectation. */\n"
                   "    return (rc == 0 && r == 0) ? 0 : 1;\n"
                   "}\n");

    api_fmt(cmd, sizeof(cmd),
            "cd %s && %s -std=c11 -I%s -o regex_probe regex_probe.c",
            OS01_API_PROBE_DIR, OS01_API_CC, OS01_API_INCLUDE_DIR);
    rc = api_run(cmd);
    if (rc != 0) {
        printf("  [FAIL] the single-TU regex probe did not build (exit %d)\n",
               rc);
        assert_eq(0, rc);
        return;
    }
    assert_eq(0, rc);

    api_fmt(cmd, sizeof(cmd), "cd %s && ./regex_probe", OS01_API_PROBE_DIR);
    rc = api_run(cmd);
    if (rc != 0) {
        printf("  [FAIL] the current regex.h no longer reports a match for a "
               "non-matching input (exit %d): update this baseline\n", rc);
    }
    assert_eq(0, rc);
}

/* ── the production objects really are the ones under test ────────── */

static void api_production_objects(void)
{
    TEST_SUITE("api_production_objects");

    /* These calls resolve to the REAL libc string implementations
     * compiled under the os01_ prefix, never to the host libc's
     * same-named function: the host build renames both sides. */
    assert_eq(3, (int)os01_strlen("abc"));
    assert_eq(0, os01_strcmp("abc", "abc"));
    assert_true(os01_strcmp("abc", "abd") < 0);
    assert_eq(0, os01_memcmp("ab", "ab", 2));
}

TEST_LIST_BEGIN
    TEST_ENTRY(api_header_self_contained),
    TEST_ENTRY(api_two_translation_units),
    TEST_ENTRY(api_cpp_linkage),
    TEST_ENTRY(api_regex_stub_baseline),
    TEST_ENTRY(api_production_objects),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

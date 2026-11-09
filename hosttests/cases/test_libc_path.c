/*
 * test_libc_path.c — libc-completion Task T14: realpath and the exec
 * family.
 *
 * What is under test: the PRODUCTION sources libc/stdlib/realpath.c,
 * libc/unistd/execl.c and libc/unistd/execve.c, plus the production
 * lstat/readlink/stat/getcwd wrappers.  What is replaced: the FILESYSTEM
 * and the EXEC boundary — hosttests/mock/libc_path_runtime.c answers the
 * syscalls (see that file for what it simulates and how the real kernel
 * behaves).
 *
 * The test pins the properties the brief names:
 *   - the relative/absolute forms, the root, repeated slashes, "." and "..";
 *   - relative and absolute symbolic links, and a dangling link;
 *   - a trailing '/' requires the final component to be a directory;
 *   - a missing component answers ENOENT;
 *   - the symbolic-link budget: 40 links resolve, the 41st answers ELOOP;
 *   - a path that does not fit answers ENAMETOOLONG;
 *   - resolved == NULL hands the caller a buffer IT owns;
 *   - ".." is applied to the RESOLVED path (the link is followed first),
 *     which is the answer glibc gives as well;
 *   - realpath() succeeding does NOT mean the kernel could open() the
 *     same path (the kernel follows 8 links in one resolution, the libc
 *     walks 40 one component at a time);
 *   - the exec wrappers build the argument vector correctly and terminate
 *     it with NULL;
 *   - execle() passes its EXPLICIT environment;
 *   - an EMPTY segment of PATH means the current directory;
 *   - EACCES takes priority over the ENOENT of the other candidates;
 *   - an ENOEXEC candidate is run through /bin/sh.
 *
 * NOTE: assert_eq() stringizes its expression into a printf format, so
 * every call whose format string contains a '%' is hoisted into a
 * variable first (a '%' inside the stringized expression would crash the
 * framework's failure-message formatter).
 */

#include "test_framework.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../mock/libc_path_runtime.h"

/* The RED build links the pre-T14 state: realpath was an empty stub in
 * libc/stdlib/malloc.c that answered NULL, and execl/execlp/execle had
 * NO definition at all.  The stub is provided here so that the RED is a
 * BEHAVIOURAL failure and not a link error.
 *
 * Two RED transcripts are produced from it:
 *   RED-1 (behavioural)  -DPATH_RED_STUB -DPATH_RED_NO_EXECL
 *       The stub realpath is linked and the exec cases are compiled out:
 *       every realpath case fails as an ASSERTION (never a crash).
 *   RED-2 (link)         -DPATH_RED_STUB
 *       The exec cases stay in and libc/unistd/execl.c is NOT linked:
 *       the LINK itself fails with "undefined reference to execl".
 */
#ifdef PATH_RED_STUB
char *realpath(const char *path, char *resolved)
{
    (void)path;
    (void)resolved;
    return NULL;
}
#endif

#ifdef PATH_RED_NO_EXECL
#define HAVE_EXECL 0
#else
#define HAVE_EXECL 1
#endif


/* A NULL-safe path assertion.  assert_str_eq() would hand strcmp() a NULL
 * pointer when the call under test fails, and the framework's own failure
 * formatter would then crash — a delivered-vs-stub failure must be an
 * ASSERTION FAILURE, never a crash. */
static void assert_path(const char *expected, const char *actual)
{
    __test_stats.total++;
    if (actual == NULL) {
        __test_stats.failed++;
        printf("  [FAIL] expected \"%s\", got NULL\n", expected);
        return;
    }
    if (strcmp(expected, actual) != 0) {
        __test_stats.failed++;
        printf("  [FAIL] expected \"%s\", got \"%s\"\n", expected, actual);
        return;
    }
    __test_stats.passed++;
}

/* ── the fake filesystem ───────────────────────────────────────────── */

#define DIR   S_IFDIR
#define REG   S_IFREG
#define LNK   S_IFLNK

static void fs_reset(void)
{
    path_rt_reset();
    /* The PATH must be ISOLATED: execvp() reads it through getenv(), and
     * without this the exec cases would walk the HOST's PATH — which
     * makes them machine-dependent and leaves the "an empty PATH segment
     * means the current directory" rule untested.  Every exec case sets
     * its own PATH; fs_reset() clears it so the cases cannot pollute
     * each other. */
    unsetenv("PATH");
    path_rt_set_cwd("/cwd");
    path_rt_add("/cwd", DIR, NULL);
    path_rt_add("/cwd/file", REG, NULL);
    path_rt_add("/cwd/dir", DIR, NULL);
    path_rt_add("/cwd/dir/inner", REG, NULL);
    path_rt_add("/other", DIR, NULL);
    path_rt_add("/other/file", REG, NULL);
}

/* ── the realpath cases ────────────────────────────────────────────── */

TEST_FUNC(test_realpath_relative_uses_cwd)
{
    char buf[PATH_MAX];

    fs_reset();
    assert_path("/cwd/file", realpath("file", buf));
    assert_path("/cwd/dir/inner", realpath("dir/inner", buf));
    assert_path("/cwd/file", realpath("./file", buf));
}

TEST_FUNC(test_realpath_absolute_and_root)
{
    char buf[PATH_MAX];

    fs_reset();
    assert_path("/cwd/file", realpath("/cwd/file", buf));
    assert_path("/other/file", realpath("/other/file", buf));
    /* the root itself */
    assert_path("/", realpath("/", buf));
}

TEST_FUNC(test_realpath_normalizes_slashes_and_dots)
{
    char buf[PATH_MAX];

    fs_reset();
    assert_path("/cwd/file", realpath("/cwd//file", buf));
    assert_path("/cwd/file", realpath("/cwd/./file", buf));
    assert_path("/cwd/dir", realpath("/cwd/dir/.", buf));
}

TEST_FUNC(test_realpath_dotdot_pops)
{
    char buf[PATH_MAX];

    fs_reset();
    assert_path("/cwd/file", realpath("/cwd/dir/../file", buf));
    assert_path("/cwd", realpath("/cwd/dir/..", buf));
    assert_path("/other", realpath("/cwd/../other", buf));
}

TEST_FUNC(test_realpath_relative_symlink)
{
    char buf[PATH_MAX];

    fs_reset();
    /* "/cwd/rel" -> "file"  (relative to the directory holding the link) */
    path_rt_add("/cwd/rel", LNK, "file");
    assert_path("/cwd/file", realpath("/cwd/rel", buf));
}

TEST_FUNC(test_realpath_absolute_symlink)
{
    char buf[PATH_MAX];

    fs_reset();
    /* "/cwd/abs" -> "/other/file"  (absolute target) */
    path_rt_add("/cwd/abs", LNK, "/other/file");
    assert_path("/other/file", realpath("/cwd/abs", buf));
    /* and one that restarts from the root in the middle of a path */
    path_rt_add("/cwd/mid", LNK, "/other");
    assert_path("/other/file", realpath("/cwd/mid/file", buf));
}

TEST_FUNC(test_realpath_dangling_symlink)
{
    char buf[PATH_MAX];

    fs_reset();
    path_rt_add("/cwd/dead", LNK, "nowhere");
    /* the target does not exist: realpath() answers ENOENT */
    assert_eq(NULL, realpath("/cwd/dead", buf));
    assert_eq(ENOENT, errno);
}

TEST_FUNC(test_realpath_trailing_slash_requires_directory)
{
    char buf[PATH_MAX];

    fs_reset();
    /* "/cwd/dir/" is a directory: fine */
    assert_path("/cwd/dir", realpath("/cwd/dir/", buf));
    /* "/cwd/file/" is not: ENOTDIR */
    assert_eq(NULL, realpath("/cwd/file/", buf));
    assert_eq(ENOTDIR, errno);
}

TEST_FUNC(test_realpath_missing_component)
{
    char buf[PATH_MAX];

    fs_reset();
    assert_eq(NULL, realpath("/cwd/nope", buf));
    assert_eq(ENOENT, errno);
    /* an empty path answers ENOENT */
    assert_eq(NULL, realpath("", buf));
    assert_eq(ENOENT, errno);
}

TEST_FUNC(test_realpath_symlink_budget_40_41)
{
    char buf[PATH_MAX];
    char link[64];

    char target[64];

    fs_reset();
    /* A chain of 40 links: l0 -> l1 -> ... -> l39 -> file.
     * realpath() walks them ONE COMPONENT AT A TIME (an lstat per
     * component), so the kernel's own MAXSYMLINKS never applies here. */
    for (int i = 0; i < 39; i++) {
        snprintf(link, sizeof(link), "/cwd/l%d", i);
        snprintf(target, sizeof(target), "l%d", i + 1);
        path_rt_add(link, LNK, target);
    }
    path_rt_add("/cwd/l39", LNK, "file");
    assert_path("/cwd/file", realpath("/cwd/l0", buf));

    /* The 41st link exceeds the libc's budget: ELOOP. */
    for (int i = 0; i < 40; i++) {
        snprintf(link, sizeof(link), "/cwd/m%d", i);
        snprintf(target, sizeof(target), "m%d", i + 1);
        path_rt_add(link, LNK, target);
    }
    path_rt_add("/cwd/m40", LNK, "file");
    {
        char *r = realpath("/cwd/m0", buf);
        assert_eq(0, r != NULL);
        assert_eq(ELOOP, errno);
    }
}

TEST_FUNC(test_realpath_path_too_long)
{
    char buf[PATH_MAX];
    char huge[PATH_MAX * 2];

    fs_reset();
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    /* one component longer than PATH_MAX */
    assert_eq(NULL, realpath(huge, buf));
    assert_eq(ENAMETOOLONG, errno);
}

TEST_FUNC(test_realpath_resolved_null_is_caller_owned)
{
    char *out;

    fs_reset();
    out = realpath("/cwd/file", NULL);
    assert_str_eq("/cwd/file", out);
    /* resolved == NULL means the buffer is MALLOCed: the caller owns it
     * and may free it.  A leak here would be invisible to the test, so
     * the free is what makes the contract observable. */
    free(out);
}

TEST_FUNC(test_realpath_dotdot_applies_to_the_resolved_path)
{
    char buf[PATH_MAX];

    fs_reset();
    /* "/cwd/par" is a symbolic link to the DIRECTORY "/other".  The link
     * is RESOLVED BEFORE the ".." is applied, so the ".." pops the
     * resolved target's last component and the answer is "/" — the
     * parent of "/other".  This is the same answer glibc gives (it asks
     * the kernel for the parent INODE of the resolved directory).
     * (An earlier claim that OS01 popped ".." LEXICALLY was WRONG: the
     * walk resolves each component's link as it goes, so the ".." sees
     * the resolved path.) */
    path_rt_add("/cwd/par", LNK, "/other");
    assert_path("/", realpath("/cwd/par/..", buf));
    /* and a plain ".." pops the resolved prefix's last component */
    assert_path("/", realpath("/cwd/..", buf));
}

TEST_FUNC(test_realpath_success_is_not_open_success)
{
    char buf[PATH_MAX];
    char link[64];
    struct stat st;

    char target[64];

    fs_reset();
    for (int i = 0; i < 11; i++) {
        snprintf(link, sizeof(link), "/cwd/k%d", i);
        snprintf(target, sizeof(target), "k%d", i + 1);
        path_rt_add(link, LNK, target);
    }
    path_rt_add("/cwd/k11", LNK, "file");
    /* realpath() resolves the chain: it lstat()s ONE COMPONENT at a time,
     * so the kernel never has to follow 12 links in one resolution. */
    assert_path("/cwd/file", realpath("/cwd/k0", buf));

    /* But a kernel ONE-SHOT resolution of the same path follows every
     * link in the chain, and the kernel's MAXSYMLINKS is 8 — so the
     * kernel answers ELOOP.  "realpath() succeeded" therefore does NOT
     * imply "the kernel can open() this path". */
    assert_eq(-1, stat("/cwd/k0", &st));
    assert_eq(ELOOP, errno);
}

/* ── the exec cases ────────────────────────────────────────────────── */

#if HAVE_EXECL
TEST_FUNC(test_execl_builds_argv)
{
    fs_reset();
    path_rt_set_exec_errno(ENOENT);

    /* The variadic list is turned into an argv that is NULL-terminated
     * and whose ORDER is the argument order. */
    (void)execl("/bin/x", "first", "second", "third", (char *)NULL);

    assert_eq(1, path_rt_exec_count());
    assert_str_eq("/bin/x", path_rt_exec_path(0));
    assert_eq(3, path_rt_exec_argc(0));
    assert_str_eq("first", path_rt_exec_argv(0, 0));
    assert_str_eq("second", path_rt_exec_argv(0, 1));
    assert_str_eq("third", path_rt_exec_argv(0, 2));
    assert_eq(NULL, path_rt_exec_argv(0, 3));
}

TEST_FUNC(test_execlp_searches_path)
{
    fs_reset();
    setenv("PATH", "/p1:/p2", 1);
    path_rt_set_exec_errno(ENOENT);

    (void)execlp("tool", "arg0", (char *)NULL);

    assert_eq(2, path_rt_exec_count());
    assert_str_eq("/p1/tool", path_rt_exec_path(0));
    assert_str_eq("/p2/tool", path_rt_exec_path(1));
    /* the argv survives the PATH search untouched */
    assert_eq(1, path_rt_exec_argc(0));
    assert_str_eq("arg0", path_rt_exec_argv(0, 0));
}

TEST_FUNC(test_execvp_empty_path_segment_is_cwd)
{
    fs_reset();
    setenv("PATH", ":/bin:", 1);
    path_rt_set_exec_errno(ENOENT);

    /* PATH=":/bin:" — the FIRST segment is empty, so it means the
     * CURRENT DIRECTORY.  An implementation that tokenises PATH with
     * strtok_r() would SKIP the empty segment and try "/bin/tool"
     * first; this one must try "./tool" first. */
    (void)execlp("tool", "arg0", (char *)NULL);

    assert_eq(3, path_rt_exec_count());
    assert_str_eq("./tool", path_rt_exec_path(0));
    assert_str_eq("/bin/tool", path_rt_exec_path(1));
    assert_str_eq("./tool", path_rt_exec_path(2));
}

TEST_FUNC(test_execvp_eacces_beats_enoent)
{
    fs_reset();
    setenv("PATH", "/p1:/p2", 1);
    path_rt_set_exec_errno(ENOENT);
    /* "/p2/tool" exists but is not executable */
    path_rt_set_exec_errno_for("/p2/tool", EACCES);

    (void)execlp("tool", "arg0", (char *)NULL);

    /* EACCES wins over the ENOENT of the other candidates */
    assert_eq(2, path_rt_exec_count());
    assert_str_eq("/p1/tool", path_rt_exec_path(0));
    assert_str_eq("/p2/tool", path_rt_exec_path(1));
}

TEST_FUNC(test_execvp_enoexec_uses_bin_sh)
{
    fs_reset();
    setenv("PATH", "/p1:/p2", 1);
    path_rt_set_exec_errno(ENOENT);
    path_rt_set_exec_errno_for("/p1/tool", ENOEXEC);

    (void)execlp("tool", "arg0", "arg1", (char *)NULL);

    /* the ENOEXEC candidate is re-run through /bin/sh IMMEDIATELY —
     * the PATH search is NOT continued (glibc's behaviour) */
    assert_eq(2, path_rt_exec_count());
    assert_str_eq("/p1/tool", path_rt_exec_path(0));
    assert_str_eq("/bin/sh", path_rt_exec_path(1));
    assert_str_eq("/bin/sh", path_rt_exec_argv0(1));
    /* the shell's argv is {"/bin/sh", <the file>, argv[1], …} — the
     * ORIGINAL argv[0] is dropped, which is what glibc does */
    assert_str_eq("tool", path_rt_exec_argv(1, 1));
    assert_str_eq("arg1", path_rt_exec_argv(1, 2));
    assert_eq(NULL, path_rt_exec_argv(1, 3));
}

TEST_FUNC(test_execle_passes_explicit_envp)
{
    char *envp[3];

    fs_reset();
    path_rt_set_exec_errno(ENOENT);

    envp[0] = (char *)"ONLY=this";
    envp[1] = (char *)"SECOND=that";
    envp[2] = NULL;

    (void)execle("/bin/x", "arg0", (char *)NULL, envp);

    assert_eq(1, path_rt_exec_count());
    assert_str_eq("/bin/x", path_rt_exec_path(0));
    assert_eq(1, path_rt_exec_argc(0));
    assert_str_eq("arg0", path_rt_exec_argv(0, 0));
    /* the EXPLICIT environment, not the inherited one */
    assert_eq(2, path_rt_exec_envc(0));
    assert_str_eq("ONLY=this", path_rt_exec_env(0, 0));
    assert_str_eq("SECOND=that", path_rt_exec_env(0, 1));
}

TEST_FUNC(test_execvp_name_with_slash_skips_path)
{
    fs_reset();
    setenv("PATH", "/p1:/p2", 1);
    path_rt_set_exec_errno(ENOENT);

    /* a name that contains a '/' is NOT searched for in PATH */
    (void)execlp("/abs/tool", "arg0", (char *)NULL);

    assert_eq(1, path_rt_exec_count());
    assert_str_eq("/abs/tool", path_rt_exec_path(0));
}
#endif /* HAVE_EXECL */

TEST_LIST_BEGIN
TEST_ENTRY(test_realpath_relative_uses_cwd),
TEST_ENTRY(test_realpath_absolute_and_root),
TEST_ENTRY(test_realpath_normalizes_slashes_and_dots),
TEST_ENTRY(test_realpath_dotdot_pops),
TEST_ENTRY(test_realpath_relative_symlink),
TEST_ENTRY(test_realpath_absolute_symlink),
TEST_ENTRY(test_realpath_dangling_symlink),
TEST_ENTRY(test_realpath_trailing_slash_requires_directory),
TEST_ENTRY(test_realpath_missing_component),
TEST_ENTRY(test_realpath_symlink_budget_40_41),
TEST_ENTRY(test_realpath_path_too_long),
TEST_ENTRY(test_realpath_resolved_null_is_caller_owned),
TEST_ENTRY(test_realpath_dotdot_applies_to_the_resolved_path),
TEST_ENTRY(test_realpath_success_is_not_open_success),
#if HAVE_EXECL
TEST_ENTRY(test_execl_builds_argv),
TEST_ENTRY(test_execlp_searches_path),
TEST_ENTRY(test_execvp_empty_path_segment_is_cwd),
TEST_ENTRY(test_execvp_eacces_beats_enoent),
TEST_ENTRY(test_execvp_enoexec_uses_bin_sh),
TEST_ENTRY(test_execle_passes_explicit_envp),
TEST_ENTRY(test_execvp_name_with_slash_skips_path),
#endif
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

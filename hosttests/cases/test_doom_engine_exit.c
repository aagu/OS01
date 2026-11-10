/*
 * D03 T2 -- the Doom engine's exit behaviour on OS01.
 *
 * The fixture host-compiles the REAL upstream i_system.c from the
 * PROFILE-PRIVATE patched copy that the user build produces -- the very
 * same source the shipped archive is built from -- and drives it through
 * the public exit interface (I_AtExit / I_Quit / I_Error).
 *
 * The exit boundary is intercepted at LINK time (--wrap=exit): the
 * production source is NOT modified, no test entry point is installed in
 * the engine, and the wrapper records what the engine asked the OS to do
 * instead of terminating the process.  That is what makes the two
 * behaviours below observable at all:
 *
 *   quit_calls_handlers_then_exit  a normal quit runs every registered
 *                                  exit handler exactly once, in the
 *                                  upstream order, and then asks the OS
 *                                  to terminate the process with status
 *                                  0.  Without the OS01 quit patch the
 *                                  upstream I_Quit only runs the
 *                                  handlers and RETURNS (its exit(0) sits
 *                                  inside the #if ORIGCODE branch, and
 *                                  ORIGCODE is deliberately not defined),
 *                                  so the engine would never shut down.
 *
 *   error_without_zenity           an error path writes the message to
 *                                  stderr and asks the OS to terminate
 *                                  with a NON-ZERO status, and it never
 *                                  shells out to a GUI helper.
 *
 * The production object check reads the symbol table of the shipped
 * i_system.o: it must not reference system() at all.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_framework.h"

/* The REAL upstream headers of the engine under test. */
#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"

#ifndef OS01_DOOM_NM
#error "OS01_DOOM_NM must be defined by the build (nm)"
#endif
#ifndef OS01_DOOM_PROD_OBJECT
#error "OS01_DOOM_PROD_OBJECT must be defined by the build"
#endif

/* ── the engine helpers the exit paths need ──────────────────────────
 *
 * I_Quit and I_Error call M_ParmExists and M_vsnprintf; the remaining
 * helpers and the argv globals are referenced from the initialisation
 * code this fixture never enters.  They are stubbed HERE so the fixture
 * can link the REAL i_system.c without dragging in the whole engine.
 * None of them takes part in the behaviour under test. */
int myargc = 0;
char **myargv = 0;

int M_CheckParmWithArgs(char *check, int num_args)
{
    (void)check;
    (void)num_args;
    return 0;
}

boolean M_ParmExists(char *check)
{
    (void)check;
    return false;
}

boolean M_StrToInt(const char *str, int *result)
{
    (void)str;
    (void)result;
    return false;
}

int M_vsnprintf(char *buf, size_t buf_len, const char *s, va_list args)
{
    return vsnprintf(buf, buf_len, s, args);
}

/* ── the intercepted exit boundary ───────────────────────────────────
 *
 * The engine calls the libc exit(), which is declared _Noreturn: the
 * compiler therefore drops the return path of I_Quit and I_Error.  A
 * wrapper that simply RETURNED would fall off the end of those
 * functions, so the wrapper must not return either -- it jumps straight
 * back into the fixture, which is exactly what "intercepting the exit
 * boundary" means here.
 *
 * The linker rewrites the engine's call to exit() to this wrapper (see
 * the -Wl,--wrap=exit below); the production source is NOT modified and
 * no test entry point is installed in the engine. */
static int exit_calls;
static int exit_status;

static jmp_buf exit_jump;
static int exit_jump_ready;

void __wrap_exit(int status)
{
    exit_calls++;
    exit_status = status;

    if (exit_jump_ready) {
        exit_jump_ready = 0;
        longjmp(exit_jump, 1);
    }

    /* No interception point is armed: honour the contract of exit() and
     * terminate for real. */
    _Exit(status);
}

/* ── the exit handlers the fixture registers ────────────────────────── */

static int first_calls;
static int second_calls;
static int error_handler_calls;
static int quiet_handler_calls;

static char handler_order[64];
static int handler_order_len;

static void record(char tag)
{
    if (handler_order_len < (int)sizeof(handler_order) - 2) {
        handler_order[handler_order_len++] = tag;
        handler_order[handler_order_len++] = ',';
    }
    handler_order[handler_order_len] = '\0';
}

static void handler_first(void)
{
    first_calls++;
    record('1');
}

static void handler_second(void)
{
    second_calls++;
    record('2');
}

static void handler_on_error(void)
{
    error_handler_calls++;
    record('E');
}

static void handler_not_on_error(void)
{
    quiet_handler_calls++;
    record('Q');
}

/* ── case 1: a normal quit ─────────────────────────────────────────── */

static void quit_calls_handlers_then_exit(void)
{
    exit_calls = 0;
    exit_status = -12345;
    first_calls = 0;
    second_calls = 0;
    handler_order_len = 0;
    handler_order[0] = '\0';

    /* Upstream I_AtExit PREPENDS, so the handlers run in the reverse
     * registration order. */
    I_AtExit(handler_first, false);
    I_AtExit(handler_second, false);

    /* The engine must not fall through: I_Quit has to hand the process
     * back to the OS. */
    volatile int quit_returned = 0;
    exit_jump_ready = 1;
    if (setjmp(exit_jump) == 0) {
        I_Quit();
        quit_returned = 1;
    }
    exit_jump_ready = 0;

    assert_false(quit_returned);
    assert_eq(1, exit_calls);
    assert_eq(0, exit_status);

    /* Every handler ran, exactly once. */
    assert_eq(1, first_calls);
    assert_eq(1, second_calls);
    assert_str_eq("2,1,", handler_order);
}

/* ── case 2: the error path ────────────────────────────────────────── */

static void error_without_zenity(void)
{
    FILE *capture = tmpfile();
    assert_true(capture != NULL);
    if (capture == NULL) {
        return;
    }

    error_handler_calls = 0;
    quiet_handler_calls = 0;
    int exit_calls_before = exit_calls;
    int exit_status_before = exit_status;

    /* One handler asks to run on an error, the other one does not. */
    I_AtExit(handler_on_error, true);
    I_AtExit(handler_not_on_error, false);

    /* Capture stderr across the error call. */
    int saved_stderr = dup(STDERR_FILENO);
    assert_true(saved_stderr >= 0);
    dup2(fileno(capture), STDERR_FILENO);

    char message[] = "doom-host-error: %d";
    volatile int error_returned = 0;
    exit_jump_ready = 1;
    if (setjmp(exit_jump) == 0) {
        I_Error(message, 42);
        error_returned = 1;
    }
    exit_jump_ready = 0;

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    char captured[512];
    /* The engine wrote to the raw descriptor, so the shared file offset
     * sits at the end of the message: rewind before reading it back. */
    fseek(capture, 0, SEEK_SET);
    size_t len = fread(captured, 1, sizeof(captured) - 1, capture);
    captured[len] = '\0';
    fclose(capture);

    /* The message reaches stderr. */
    assert_true(strstr(captured, "doom-host-error: 42") != NULL);

    /* The error path terminates the process with a NON-ZERO status. */
    assert_false(error_returned);
    assert_eq(exit_calls_before + 1, exit_calls);
    assert_true(exit_status != exit_status_before);
    assert_true(exit_status != 0);

    /* Only the handler that asked to run on error ran. */
    assert_eq(1, error_handler_calls);
    assert_eq(0, quiet_handler_calls);
}

/* ── case 3: the shipped object carries no system() dependency ──────── */

static int object_references_symbol(const char *tool, const char *object,
                                    const char *symbol)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "'%s' --undefined-only '%s' 2>/dev/null",
             tool, object);

    FILE *pipe = popen(cmd, "r");
    if (pipe == NULL) {
        return -1;
    }

    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), pipe) != NULL) {
        char name[256];
        if (sscanf(line, "%*s %255s", name) == 1 &&
            strcmp(name, symbol) == 0) {
            found = 1;
            break;
        }
    }
    pclose(pipe);
    return found;
}

static void patched_i_system_object_has_no_system_dependency(void)
{
    int refs = object_references_symbol(OS01_DOOM_NM, OS01_DOOM_PROD_OBJECT,
                                        "system");
    assert_true(refs >= 0);
    assert_eq(0, refs);
}

TEST_LIST_BEGIN
    TEST_ENTRY(quit_calls_handlers_then_exit),
    TEST_ENTRY(error_without_zenity),
    TEST_ENTRY(patched_i_system_object_has_no_system_dependency),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

/*
 * D03 T3 -- the Doom engine's configuration round-trip on OS01.
 *
 * The fixture host-compiles the REAL, PATCHED upstream m_config.c (and the
 * upstream m_misc.c it needs for M_StringJoin/M_MakeDirectory) from the
 * PROFILE-PRIVATE copy the user build produces -- the very same source the
 * shipped archive is built from -- and drives it through the PUBLIC
 * configuration interface only:
 *
 *   M_SetConfigDir()  M_SetConfigFilenames()  M_BindVariable()
 *   M_LoadDefaults()  M_SaveDefaults()
 *
 * The upstream has no typed binding API, so the fixture provides int,
 * float and char* storage for the entries of the default configuration
 * table and binds each of them through the SAME M_BindVariable().
 *
 * The fixture observes six things:
 *
 *   fire_use_roundtrip        the OS01 controller keys survive a
 *                             save/reload cycle: the file keeps the
 *                             vanilla scancodes 29 and 57 and they read
 *                             back as KEY_FIRE and KEY_USE.
 *   letter_rebind_roundtrip   a rebound letter key survives the same
 *                             cycle, and the Shift special case (54)
 *                             still round-trips.
 *   config_numeric_string_roundtrip
 *                             the numeric and string semantics survive:
 *                             decimal, octal and hexadecimal integers,
 *                             floats and quoted strings.
 *   bad_line_keeps_default    a broken entry keeps the DEFAULT and is
 *                             reported; an invalid binding is never
 *                             generated.
 *   overlong_line_progress    a line that does not fit is dropped WHOLE
 *                             and the loader still makes progress.
 *   io_error_terminates       a config file that cannot be read back, or
 *                             written, is reported and terminates the
 *                             process with a non-zero status.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_framework.h"

/* The REAL upstream headers of the engine under test. */
#include "doomtype.h"
#include "doomkeys.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_config.h"

/* ── the engine helpers the configuration code needs ───────────────────
 *
 * M_LoadDefaults() asks the command line for an alternative config file;
 * the fixture never passes one, so the stub always reports "not found".
 * myargc/myargv are the upstream globals that hold it. */
int myargc = 0;
char **myargv = 0;

int M_CheckParmWithArgs(char *check, int num_args)
{
    (void)check;
    (void)num_args;
    return 0;
}

/* The upstream zone allocator is referenced by m_misc.c; the fixture does
 * not use the zone, so a plain malloc is enough to close the link. */
void *Z_Malloc(int size, int tag, void *ptr)
{
    (void)tag;
    (void)ptr;
    return malloc(size);
}

/* ── the fatal-error boundary ──────────────────────────────────────────
 *
 * The patched configuration code reports a fatal I/O problem through
 * I_Error().  The stub records the message and jumps back into the
 * fixture: the production code must not continue after an I/O error, and
 * the fixture has to be able to observe the termination. */
static jmp_buf fatal_jump;
static int fatal_jump_ready;
static int fatal_count;
static char fatal_message[512];

void I_Error(char *error, ...)
{
    va_list args;

    fatal_count++;
    va_start(args, error);
    vsnprintf(fatal_message, sizeof(fatal_message), error, args);
    va_end(args);
    fprintf(stderr, "I_Error: %s\n", fatal_message);
    fflush(stderr);

    if (fatal_jump_ready)
    {
        fatal_jump_ready = 0;
        longjmp(fatal_jump, 1);
    }

    exit(1);
}

/* ── the working directory ─────────────────────────────────────────────
 *
 * The configuration files are relative to the current directory
 * ("./default.cfg"), so every case works in its own temporary directory. */
static char work_dir[256];
static char saved_cwd[256];

static void enter_work_dir(void)
{
    char template[] = "/tmp/os01-doom-config-XXXXXX";
    char *dir;

    if (saved_cwd[0] == '\0')
    {
        assert_true(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
    }

    dir = mkdtemp(template);
    assert_true(dir != NULL);
    if (dir == NULL)
    {
        return;
    }

    snprintf(work_dir, sizeof(work_dir), "%s", dir);
    assert_true(chdir(work_dir) == 0);

    M_SetConfigFilenames("default.cfg", "doomgenericdoom.cfg");
    M_SetConfigDir(NULL);
}

static void leave_work_dir(void)
{
    unlink("default.cfg");
    unlink("doomgenericdoom.cfg");
    rmdir("default.cfg");
    rmdir(work_dir);
    if (saved_cwd[0] != '\0')
    {
        assert_true(chdir(saved_cwd) == 0);
    }
}

/* ── small helpers ────────────────────────────────────────────────────── */

static int read_file(const char *path, char *buffer, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t len;

    if (f == NULL)
    {
        return -1;
    }

    len = fread(buffer, 1, size - 1, f);
    buffer[len] = '\0';
    fclose(f);
    return (int)len;
}

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    size_t len;

    if (f == NULL)
    {
        return -1;
    }

    len = strlen(content);
    if (fwrite(content, 1, len, f) != len)
    {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* Extract the value the saver wrote for one configuration entry.  The
 * upstream format is the name, padded to 30 columns, then the value. */
static int config_value(const char *content, const char *name,
                         char *out, size_t size)
{
    size_t namelen = strlen(name);
    const char *p = content;

    while (p != NULL && *p != '\0')
    {
        if (strncmp(p, name, namelen) == 0 &&
            (p[namelen] == ' ' || p[namelen] == '\t'))
        {
            size_t i = 0;

            p += namelen;
            while (*p == ' ' || *p == '\t')
            {
                ++p;
            }
            while (*p != '\0' && *p != '\n' && i + 1 < size)
            {
                out[i++] = *p++;
            }
            out[i] = '\0';
            while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t'))
            {
                out[--i] = '\0';
            }
            return (int)i;
        }

        p = strchr(p, '\n');
        if (p != NULL)
        {
            ++p;
        }
    }

    out[0] = '\0';
    return -1;
}

/* The stored configuration variables.  Their types come from the default
 * configuration table of the pinned revision:
 *   key_fire / key_use / key_right   DEFAULT_KEY     -> int
 *   screenblocks / mouse_sensitivity DEFAULT_INT     -> int
 *   mouse_acceleration               DEFAULT_FLOAT   -> float
 *   savedir                          DEFAULT_STRING  -> char *
 */
static int key_fire_value;
static int key_use_value;
static int key_right_value;
static int screenblocks_value;
static int mouse_sensitivity_value;
static float mouse_acceleration_value;
static char *savedir_value;

static void bind_variables(void)
{
    M_BindVariable("key_fire", &key_fire_value);
    M_BindVariable("key_use", &key_use_value);
    M_BindVariable("key_right", &key_right_value);
    M_BindVariable("screenblocks", &screenblocks_value);
    M_BindVariable("mouse_sensitivity", &mouse_sensitivity_value);
    M_BindVariable("mouse_acceleration", &mouse_acceleration_value);
    M_BindVariable("savedir", &savedir_value);
}

/* The engine computes the config file names during the startup load, so
 * M_SaveDefaults() can only run after M_LoadDefaults() has run once.
 * That is the upstream order: load at startup, save at shutdown. */
static void prepare_config(void)
{
    M_LoadDefaults();
}

/* ── case 1: the OS01 controller keys survive a save/reload cycle ────── */

static void fire_use_roundtrip(void)
{
    char content[4096];
    char value[128];

    enter_work_dir();
    bind_variables();
    prepare_config();

    key_fire_value = KEY_FIRE;
    key_use_value = KEY_USE;
    key_right_value = KEY_RIGHTARROW;
    screenblocks_value = 12;
    mouse_sensitivity_value = 8;
    mouse_acceleration_value = 1.0f;
    savedir_value = (char *)"saves";

    M_SaveDefaults();

    assert_true(read_file("default.cfg", content, sizeof(content)) > 0);

    /* The file keeps the VANILLA scancodes, not the internal key codes:
     * the OS01 table maps 29 to KEY_FIRE and 57 to KEY_USE. */
    assert_true(config_value(content, "key_fire", value, sizeof(value)) > 0);
    assert_str_eq("29", value);
    assert_true(config_value(content, "key_use", value, sizeof(value)) > 0);
    assert_str_eq("57", value);

    /* An unmapped entry keeps the upstream mapping. */
    assert_true(config_value(content, "key_right", value, sizeof(value)) > 0);
    assert_true(strcmp(value, "163") != 0);
    assert_true(strcmp(value, "162") != 0);

    /* A fresh process reads the file back. */
    key_fire_value = 0;
    key_use_value = 0;
    M_LoadDefaults();

    assert_eq(KEY_FIRE, key_fire_value);
    assert_eq(KEY_USE, key_use_value);

    leave_work_dir();
}

/* ── case 2: a rebound letter key survives the same cycle ────────────── */

static void letter_rebind_roundtrip(void)
{
    char content[4096];
    char value[128];

    enter_work_dir();
    bind_variables();
    prepare_config();

    /* Rebind the "right" key to the letter 'f' (the vanilla scancode for
     * 'f' is 33). */
    key_right_value = 'f';

    M_SaveDefaults();

    assert_true(read_file("default.cfg", content, sizeof(content)) > 0);
    assert_true(config_value(content, "key_right", value, sizeof(value)) > 0);
    assert_str_eq("33", value);

    key_right_value = 0;
    M_LoadDefaults();

    assert_eq('f', key_right_value);

    /* The Shift special case is untouched: the saver still writes the
     * vanilla scancode 54 and the loader reads KEY_RSHIFT back. */
    key_right_value = KEY_RSHIFT;
    M_SaveDefaults();

    assert_true(read_file("default.cfg", content, sizeof(content)) > 0);
    assert_true(config_value(content, "key_right", value, sizeof(value)) > 0);
    assert_str_eq("54", value);

    key_right_value = 0;
    M_LoadDefaults();

    assert_eq(KEY_RSHIFT, key_right_value);

    leave_work_dir();
}

/* ── case 3: the numeric and string semantics survive ────────────────── */

static void config_numeric_string_roundtrip(void)
{
    char content[4096];
    char extra[4096];
    char value[128];

    enter_work_dir();
    bind_variables();
    prepare_config();

    screenblocks_value = 12;
    mouse_sensitivity_value = 8;
    mouse_acceleration_value = 3.5f;
    savedir_value = (char *)"os01-saves";

    M_SaveDefaults();

    assert_true(read_file("default.cfg", content, sizeof(content)) > 0);
    /* The string stays quoted. */
    assert_true(config_value(content, "savedir", value, sizeof(value)) > 0);
    assert_str_eq("\"os01-saves\"", value);

    /* mouse_acceleration belongs to the EXTRA collection of the default
     * configuration table, so it lives in the extra config file. */
    assert_true(read_file("doomgenericdoom.cfg", extra,
                          sizeof(extra)) > 0);
    /* The float keeps its fractional part. */
    assert_true(config_value(extra, "mouse_acceleration", value,
                              sizeof(value)) > 0);
    assert_str_eq("3.500000", value);

    screenblocks_value = 0;
    mouse_sensitivity_value = 0;
    mouse_acceleration_value = 0.0f;
    savedir_value = NULL;

    M_LoadDefaults();

    assert_eq(12, screenblocks_value);
    assert_eq(8, mouse_sensitivity_value);
    assert_true(mouse_acceleration_value > 3.49f &&
                mouse_acceleration_value < 3.51f);
    assert_true(savedir_value != NULL);
    assert_str_eq("os01-saves", savedir_value);

    /* The integer syntax keeps the upstream "%i" semantics: a 0-prefixed
     * literal is octal, a 0x-prefixed one is hexadecimal. */
    assert_true(write_file("default.cfg",
                           "screenblocks 017\n"
                           "mouse_sensitivity 0x10\n"
                           "savedir \"quoted string\"\n") == 0);
    assert_true(write_file("doomgenericdoom.cfg",
                           "mouse_acceleration 2.5\n") == 0);

    screenblocks_value = 0;
    mouse_sensitivity_value = 0;
    mouse_acceleration_value = 0.0f;
    savedir_value = NULL;

    M_LoadDefaults();

    assert_eq(15, screenblocks_value);          /* 017 is octal */
    assert_eq(16, mouse_sensitivity_value);     /* 0x10 is hexadecimal */
    assert_true(mouse_acceleration_value > 2.49f &&
                mouse_acceleration_value < 2.51f);
    assert_true(savedir_value != NULL);
    assert_str_eq("quoted string", savedir_value);

    leave_work_dir();
}

/* ── case 4: a broken entry keeps the default ────────────────────────── */

static void bad_line_keeps_default(void)
{
    enter_work_dir();
    bind_variables();
    prepare_config();

    key_fire_value = KEY_FIRE;
    screenblocks_value = 12;

    assert_true(write_file("default.cfg",
                           "screenblocks notanumber\n"
                           "key_fire 999\n"
                           "key_fire\n") == 0);

    M_LoadDefaults();

    /* The broken integer keeps the default. */
    assert_eq(12, screenblocks_value);
    /* The unmappable key keeps the default too: an invalid binding is
     * never generated. */
    assert_eq(KEY_FIRE, key_fire_value);

    leave_work_dir();
}

/* ── case 5: an overlong line is dropped whole ───────────────────────── */

static void overlong_line_progress(void)
{
    char content[8192];
    size_t i;

    enter_work_dir();
    bind_variables();
    prepare_config();

    screenblocks_value = 12;
    mouse_sensitivity_value = 8;

    /* A line that does not fit into the loader's buffer, between two good
     * ones. */
    i = (size_t)snprintf(content, sizeof(content), "screenblocks 21\n");
    while (i < sizeof(content) - 64)
    {
        content[i++] = 'x';
    }
    snprintf(content + i, sizeof(content) - i, "\nmouse_sensitivity 9\n");

    assert_true(write_file("default.cfg", content) == 0);

    screenblocks_value = 0;
    mouse_sensitivity_value = 0;

    M_LoadDefaults();

    /* The loader made progress: the line before AND the line after the
     * overlong one were applied. */
    assert_eq(21, screenblocks_value);
    assert_eq(9, mouse_sensitivity_value);

    leave_work_dir();
}

/* ── case 6: an I/O error terminates ──────────────────────────────────── */

static void io_error_terminates(void)
{
    enter_work_dir();
    bind_variables();

    /* Make the config path a DIRECTORY: it can be opened, but it cannot be
     * read, and it cannot be opened for writing.  That is deterministic and
     * needs no privileges. */
    assert_true(mkdir("default.cfg", 0755) == 0);

    /* The read error terminates instead of spinning. */
    fatal_count = 0;
    fatal_message[0] = '\0';
    fatal_jump_ready = 1;
    if (setjmp(fatal_jump) == 0)
    {
        M_LoadDefaults();
    }
    fatal_jump_ready = 0;

    assert_true(fatal_count >= 1);
    assert_true(strstr(fatal_message, "default.cfg") != NULL);

    /* Saving into the directory fails loudly as well. */
    fatal_count = 0;
    fatal_message[0] = '\0';
    fatal_jump_ready = 1;
    if (setjmp(fatal_jump) == 0)
    {
        M_SaveDefaults();
    }
    fatal_jump_ready = 0;

    assert_true(fatal_count >= 1);
    assert_true(strstr(fatal_message, "default.cfg") != NULL);

    leave_work_dir();
}

TEST_LIST_BEGIN
    TEST_ENTRY(fire_use_roundtrip),
    TEST_ENTRY(letter_rebind_roundtrip),
    TEST_ENTRY(config_numeric_string_roundtrip),
    TEST_ENTRY(bad_line_keeps_default),
    TEST_ENTRY(overlong_line_progress),
    TEST_ENTRY(io_error_terminates),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

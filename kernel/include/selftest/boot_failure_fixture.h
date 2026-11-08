/* kernel/include/selftest/boot_failure_fixture.h — BOOT-3 startup failure fixture.
 *
 * Linked exclusively into boot-failure-<case> kernel variants.
 * Non-variant builds see empty static-inlines and zero overhead.
 */

#ifndef _SELFTEST_BOOT_FAILURE_FIXTURE_H
#define _SELFTEST_BOOT_FAILURE_FIXTURE_H

#include <stdbool.h>

#ifdef BOOT_FAILURE_TEST

/* Early print of variant identity and registration of fixture subsys */
void boot3_test_early_init(void);

/* Decision query: returns true if 'site' matches active failure case and hasn't fired yet */
bool boot3_test_fail_site(const char *site);

/* Init path query: returns bad path for CASE_INIT, "/bin/init" otherwise */
const char *boot3_test_init_path(void);

/* stdio helper allocation window */
void boot3_test_stdio_enter(void);
void boot3_test_stdio_leave(void);
bool boot3_test_in_stdio_window(void);

/* spawn child-fds allocation window */
void boot3_test_spawn_enter(void);
void boot3_test_spawn_leave(void);
bool boot3_test_in_spawn_window(void);

/* Sentinel emission: prints [boot-3-test] <sentinel> */
void boot3_test_sentinel(const char *sentinel);

#else /* !BOOT_FAILURE_TEST */

static inline void boot3_test_early_init(void) {}
static inline bool boot3_test_fail_site(const char *site) { (void)site; return false; }
static inline const char *boot3_test_init_path(void) { return "/bin/init"; }
static inline void boot3_test_stdio_enter(void) {}
static inline void boot3_test_stdio_leave(void) {}
static inline bool boot3_test_in_stdio_window(void) { return false; }
static inline void boot3_test_spawn_enter(void) {}
static inline void boot3_test_spawn_leave(void) {}
static inline bool boot3_test_in_spawn_window(void) { return false; }
static inline void boot3_test_sentinel(const char *sentinel) { (void)sentinel; }

#endif /* BOOT_FAILURE_TEST */

#endif /* _SELFTEST_BOOT_FAILURE_FIXTURE_H */

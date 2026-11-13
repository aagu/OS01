#ifndef OS01_USER_SYSTEST_H
#define OS01_USER_SYSTEST_H

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <rbtree.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ssp.h>
#include <time.h>

typedef void (*test_fn)(void);

typedef struct {
    const char *id;      /* stable machine ID, [A-Za-z0-9_.-]+ */
    const char *name;    /* human-readable display name */
    test_fn fn;
    int required;        /* 1 = mandatory case (SELECT required=1) */
    int quick;           /* 1 = included by --quick */
} test_case_t;

extern int fail_count;
extern int pass_count;
extern const char *case_skip_reason;
extern int g_argc;
extern char **g_argv;

#define PASS(msg, ...) do { printf("[PASS] " msg "\n", ##__VA_ARGS__); pass_count++; } while(0)
#define FAIL(msg, ...) do { printf("[FAIL] " msg "\n", ##__VA_ARGS__); fail_count++; } while(0)
#define skip_case(reason) do { case_skip_reason = (reason); } while(0)
#define CHECK3(cond, name, detail) do { \
    if (cond) PASS("%s (%s)", name, detail); \
    else FAIL("%s: %s", name, detail); \
} while(0)
#define CHECKF(cond, name, pfmt, ffmt, ...) do { \
    if (cond) PASS("%s (" pfmt ")", name, ##__VA_ARGS__); \
    else FAIL("%s: " ffmt, name, ##__VA_ARGS__); \
} while(0)

/* Startup probe runner: defined in systest.c, called by test_libc_env in test_libc.c */
int run_startup_probe_case(char **argv, char **envp, int expect_code);

/* ── Test case function declarations (122 test cases) ── */
void test_41_exec_via_symlink(void);
void test_42_getdents_dt_lnk(void);
void test_access(void);
void test_atexit_lifecycle(void);
void test_boot_fat_mount(void);
void test_brk(void);
void test_brk_read_fresh_page(void);
void test_busybox_lines(void);
void test_chdir_getcwd(void);
void test_chmod(void);
void test_clock_gettime(void);
void test_cow_kernel_outputs(void);
void test_devfs_open_default_fg_pgrp(void);
void test_devfs_open_inherited_fg_pgrp(void);
void test_dir_scan_vfs(void);
void test_dup_dup2(void);
void test_environ_mutation(void);
void test_exec_hostile_argv(void);
void test_exec_hostile_envp(void);
void test_ext2_write(void);
void test_fcntl(void);
void test_float_precision(void);
void test_fork_brk_isolation(void);
void test_fork_exec_waitpid(void);
void test_fork_mmap_cow_isolation(void);
void test_fixed_file_replace(void);
void test_fb_fixed_replace(void);
void test_getdents64(void);
void test_getpid_getppid(void);
void test_getrandom(void);
void test_gettimeofday(void);
void test_hostile(void);
void test_integer_conversions(void);
void test_kill_neg_pid_pgrp(void);
void test_kill_signal_deliver(void);
void test_libc_alloc(void);
void test_libc_append(void);
void test_libc_dir(void);
void test_libc_env(void);
void test_libc_float(void);
void test_libc_integer(void);
void test_libc_path_exec(void);
void test_libc_printf_getopt(void);
void test_libc_scan(void);
void test_libc_stdio(void);
void test_libc_time(void);
void test_linux_abi_compat(void);
void test_lseek(void);
void test_mkdir_rmdir(void);
void test_mmap_execute_protection(void);
void test_mprotect_subrange_cow(void);
void test_nanosleep(void);
void test_nanosleep_eintr(void);
void test_open_close(void);
void test_orphan_reparent(void);
void test_pipe(void);
void test_poll(void);
void test_proc_fd(void);
void test_proc_maps(void);
void test_protected_ranges(void);
void test_pselect_bad_ss_len(void);
void test_pselect_null_sigmask(void);
void test_pselect_sleep(void);
void test_putchar(void);
void test_read(void);
void test_readlink(void);
void test_realpath_vfs(void);
void test_reboot_skip(void);
void test_rename(void);
void test_select_basic(void);
void test_select_invalid_fd(void);
void test_select_multifd(void);
void test_select_null_timeout(void);
void test_select_sleep(void);
void test_select_timeout(void);
void test_select_write(void);
void test_select_zero_timeout(void);
void test_sendto_user_boundaries(void);
void test_setpgid_auto_fg_pgrp(void);
void test_setpgid_getpgid(void);
void test_setsid(void);
void test_signal_handler_sync(void);
void test_fatal_signal_exit(void);
void test_signal_pgrp_basic(void);
void test_signal_register(void);
void test_sigprocmask(void);
void test_sigreturn_saved_frame(void);
void test_ssp_guard_distinct_across_exec(void);
void test_ssp_guard_seeded(void);
void test_ssp_no_false_trip(void);
void test_ssp_trip_sigabrt_exec(void);
void test_ssp_trip_sigabrt_fork(void);
void test_startup_execvp(void);
void test_startup_layout(void);
void test_startup_matrix(void);
void test_stat_fstat(void);
void test_stdio_modes(void);
void test_symlink_create_readlink(void);
void test_symlink_errors(void);
void test_symlink_follow_midpath(void);
void test_symlink_loop_depth(void);
void test_symlink_stat(void);
void test_sync(void);
void test_termios(void);
void test_time_conversion(void);
void test_times(void);
void test_tiocspgrp_roundtrip(void);
void test_truncate(void);
void test_umask(void);
void test_uname(void);
void test_unlink(void);
void test_user_fault_divzero(void);
void test_user_fault_gp(void);
void test_user_fault_ud2(void);
void test_user_fault_write_ro(void);
void test_vfs_append(void);
void test_wrap_eevdf_fork_child_scheduled(void);
void test_wrap_rbtree_erase_middle(void);
void test_wrap_rbtree_insert_order(void);
void test_wrap_rbtree_last(void);
void test_wrap_rbtree_prev_null(void);
void test_wrap_rbtree_prev_traversal(void);
void test_wrap_rbtree_stress_100(void);
void test_write(void);

#endif /* OS01_USER_SYSTEST_H */

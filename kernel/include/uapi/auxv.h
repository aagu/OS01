#ifndef _UAPI_AUXV_H
#define _UAPI_AUXV_H

/* ── Auxiliary vector types (spec 2026-09-17 §6.3) ────────────────────────
 * Single source for both kernel-side setup and libc user-space getauxval().
 * The kernel builds kernel.bin from this header and `setup_user_stack()` in
 * kernel/sched/task.c emits entries by AT_* numeric value; the libc build
 * installs this same header into its sysroot so <sys/auxv.h> includes it
 * verbatim (no per-side mirror). Values match Linux <sys/auxv.h> for the
 * subset OS01 actually emits (AT_NULL/AT_PLATFORM/AT_RANDOM + a few common
 * ones user-space programs may probe). */

#define AT_NULL           0    /* end of vector */
#define AT_IGNORE         1    /* ignored entry */
#define AT_EXECFD         2    /* fd of program (deprecated) */
#define AT_PHDR           3    /* &phdr[0] */
#define AT_PHENT          4    /* sizeof(phdr) */
#define AT_PHNUM          5    /* # phdr entries */
#define AT_PAGESZ         6    /* page size */
#define AT_BASE           7    /* interpreter base */
#define AT_FLAGS          8    /* flags */
#define AT_ENTRY          9    /* program entry point */
#define AT_NOTELF         10   /* program is not ELF */
#define AT_UID            11   /* real uid */
#define AT_EUID           12   /* effective uid */
#define AT_GID            13   /* real gid */
#define AT_EGID           14   /* effective gid */
#define AT_PLATFORM       15   /* string identifying platform ("x86_64") */
#define AT_HWCAP          16   /* CPU feature bit mask */
#define AT_CLKTCK         17   /* frequency of times() */
#define AT_SECURE         23   /* boolean, set for security-sensitive */
#define AT_RANDOM         25   /* 16 random bytes (CSPRNG) */
#define AT_HWCAP2         26   /* extension of AT_HWCAP */
#define AT_EXECFN         31   /* program filename */
#define AT_SYSINFO_EHDR   33   /* vDSO entry — OS01 has none (always miss) */

#endif /* _UAPI_AUXV_H */

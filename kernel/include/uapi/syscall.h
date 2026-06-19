#ifndef _UAPI_SYSCALL_H
#define _UAPI_SYSCALL_H

// ── Original (preserved for compatibility) ────────────────
#define SYS_putchar  0
#define SYS_write    1    // write(fd, buf, len)  — fd-based since Phase 1
#define SYS_exit     2
#define SYS_brk      3
#define SYS_getpid   4
#define SYS_exec     5
#define SYS_read     6    // read(fd, buf, len)   — fd-based since Phase 1

// ── Phase 1: file descriptor infrastructure ───────────────
#define SYS_open     7
#define SYS_close    8
#define SYS_dup      9
#define SYS_dup2    10

// ── Phase 2: process management ───────────────────────────
#define SYS_fork    11
#define SYS_waitpid 12

// ── Phase 3: pipe + directory ─────────────────────────────
#define SYS_pipe    13
#define SYS_chdir   14
#define SYS_getcwd  15

#endif

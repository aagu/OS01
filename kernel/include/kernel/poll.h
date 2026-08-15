#ifndef _KERNEL_POLL_H
#define _KERNEL_POLL_H

#include <stdint.h>
#include <stdbool.h>
#include <list.h>
#include <kernel/wait.h>
#include <kernel/arch/spinlock.h>

// ── Max fds per poll call ──────────────────────────────

#define POLL_MAX_FDS  16

// ── Poll event flags (Linux ABI) ──────────────────────

#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020
#define POLLRDNORM  0x040
#define POLLRDBAND  0x080
#define POLLWRNORM  0x100
#define POLLWRBAND  0x200

static inline bool poll_requested_read(uint32_t requested)
{
    return (requested & (POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND)) != 0;
}

static inline bool poll_requested_write(uint32_t requested)
{
    return (requested & (POLLOUT | POLLWRNORM | POLLWRBAND)) != 0;
}

// ── Poll fd (Linux ABI — must match libc/include/poll.h) ──
// Defined here so kernel code (poll.c, trap.c) can use it
// without including a userspace header.

struct pollfd {
    int   fd;
    short events;
    short revents;
};

// ── Forward declaration ────────────────────────────────

struct poll_table;

// ── Poll wait entry ────────────────────────────────────
// Hung on an fd's poll list (e.g. pipe_t.read_poll).
// When the fd becomes ready, its wake path cascades:
//   wait_queue_wake_all(entry->poll_wq)
// The fd_lock pointer protects the list this entry lives on;
// poll_table_cleanup uses it for safe removal.

typedef struct poll_wait_entry {
    list_t       node;        // link in fd's poll list
    wait_queue_t *poll_wq;    // cascade target — pt.wq
    spinlock_T   *fd_lock;    // lock protecting the poll list
} poll_wait_entry_t;

// ── Poll table ─────────────────────────────────────────
// Per-syscall object, one per concurrent poll/select call.
// The polling task sleeps on pt.wq; when any fd becomes ready
// it cascade-wakes pt.wq.  Entries are heap-allocated via
// poll_table_setup and freed via poll_table_destroy.

typedef struct poll_table {
    wait_queue_t        wq;                      // main wait queue
    poll_wait_entry_t   *entries;                // dynamically allocated array
    int                 max_entries;             // capacity of entries array
    int                 nent;                    // active entry count
    bool                triggered;               // short-circuit: fd ready
} poll_table_t;

// ── API ────────────────────────────────────────────────

// Reset poll table for a new scan round (nent=0, triggered=false).
// Does NOT re-init wq — call poll_table_setup once, then init per round.
void poll_table_init(poll_table_t *pt);

// One-time setup: allocate entries + init wq + all entry nodes.
// Returns 0 on success, -ENOMEM on kmalloc failure.
int poll_table_setup(poll_table_t *pt, int max_entries);

// Free entries allocated by poll_table_setup.
void poll_table_destroy(poll_table_t *pt);

// Register current fd as not-ready.  The entry is hung on poll_list
// (protected by fd_lock).  When the fd becomes ready, its wake path
// walks poll_list and calls wait_queue_wake_all(e->poll_wq).
// Entries are indexed into pt->entries; capacity is pt->max_entries.
void poll_wait(poll_table_t *pt, list_t *poll_list, spinlock_T *fd_lock);

// Remove all entries from their fd poll lists.  Uses entry.fd_lock
// for mutual exclusion with fd wake paths.
void poll_table_cleanup(poll_table_t *pt);

// Forward-declared in kernel/fs/file.h; implementation in kernel/fs/poll.c.
struct file;
uint32_t fd_poll(struct file *f, uint32_t requested,
                 struct poll_table *pt);

// do_poll — poll(2) syscall implementation.
int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout);

// do_poll_core — core polling loop (no user memory access).
// Caller provides kfds and pt (already setup via poll_table_setup).
// Caller is responsible for poll_table_destroy (not called inside).
// Returns: ready count (>=0), -EINTR, -ENOMEM.
int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int64_t timeout_val, poll_table_t *pt);

#endif // _KERNEL_POLL_H

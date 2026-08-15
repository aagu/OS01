// kernel/net/arch/sys_arch.h — lwIP sys_arch types + prototypes for OS01
// Matches lwIP 2.2.0 API: all functions take pointer-to-handle.
#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "lwip/opt.h"
#include "lwip/err.h"

// ── Opaque types ───────────────────────────────────────────────
// Must be assignable types (not void*) because lwIP does
// *mbox = NULL and similar in API_VAR_ALLOC teardown paths.
typedef uintptr_t sys_sem_t;
typedef uintptr_t sys_mbox_t;
typedef uintptr_t sys_thread_t;
typedef uintptr_t sys_mutex_t;   // needed by lwIP 2.2.0 tcpip.h:54

// ── Protection ─────────────────────────────────────────────────
typedef uint64_t sys_prot_t;

// ── Critical region macros ─────────────────────────────────────
#define SYS_ARCH_DECL_PROTECT(lev)   sys_prot_t lev
#define SYS_ARCH_PROTECT(lev)        lev = sys_arch_protect()
#define SYS_ARCH_UNPROTECT(lev)      sys_arch_unprotect(lev)

// ── Semaphore (matches lwIP 2.2.0 declarations in sys.h) ──────
// lwIP calls these with &sem_field (pointer to sys_sem_t value).
err_t  sys_sem_new(sys_sem_t *sem, u8_t count);
void   sys_sem_signal(sys_sem_t *sem);
u32_t  sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout);
void   sys_sem_free(sys_sem_t *sem);
#define sys_sem_valid(sem)       ((*(sem)) != 0)
#define sys_sem_set_invalid(sem) do { *(sem) = 0; } while (0)

// ── Mailbox (matches lwIP 2.2.0 declarations in sys.h) ────────
// lwIP calls these with &mbox_field (pointer to sys_mbox_t value).
err_t  sys_mbox_new(sys_mbox_t *mbox, int size);
void   sys_mbox_post(sys_mbox_t *mbox, void *msg);
u32_t  sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout);
u32_t  sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg);
void   sys_mbox_free(sys_mbox_t *mbox);
#define sys_mbox_valid(mbox)       ((*(mbox)) != 0)
#define sys_mbox_set_invalid(mbox) do { *(mbox) = 0; } while (0)

// ── Thread ─────────────────────────────────────────────────────
typedef void (*lwip_thread_fn)(void *arg);
sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *arg, int stacksize, int prio);

// ── Protection ─────────────────────────────────────────────────
sys_prot_t sys_arch_protect(void);
void       sys_arch_unprotect(sys_prot_t pval);

// ── Time ──────────────────────────────────────────────────────
u32_t sys_now(void);

// ── Init ──────────────────────────────────────────────────────
void sys_init(void);

// ── Sleep ─────────────────────────────────────────────────────
void sys_msleep(u32_t ms);

#endif // LWIP_ARCH_SYS_ARCH_H

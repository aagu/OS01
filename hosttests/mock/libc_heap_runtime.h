/* hosttests/mock/libc_heap_runtime.h
 *
 * Host-test heap runtime for the OS01 libc allocator group (T2).
 *
 * The OS01 user libc grows its heap with the SYS_brk syscall
 * (libc/stdlib/malloc.c).  Under OS01_HOST_TEST the libc's
 * <sys/syscall.h> routes every syscall through the weak hook
 * `host_test_syscall`, so this mock provides the heap the allocator
 * grows and a fault-injection switch that makes the next heap growth
 * fail with ENOMEM.
 *
 * The mock deliberately emulates ONLY SYS_brk — the single boundary the
 * allocator crosses.  Every other syscall is answered with -ENOSYS.
 *
 * The hook itself and the mock state live in the test translation unit
 * that includes this header (hosttests/cases/test_libc_alloc.c); the
 * header is the declared contract between the mock and that test.
 */
#ifndef OS01_LIBC_HEAP_RUNTIME_H
#define OS01_LIBC_HEAP_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* Size of the mock heap the allocator may grow into.  It is big enough
 * for the 4096-byte alignment case (an aligned block may waste up to
 * one alignment unit of front space) plus the ordinary allocations. */
#define LIBC_HEAP_RUNTIME_SIZE (512u * 1024u)

/* The syscall boundary hook.  libc/include/sys/syscall.h declares it
 * weak under OS01_HOST_TEST; the test translation unit defines it. */
int64_t host_test_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t a3);

/* Reset the mock heap: the program break returns to the heap base and
 * the fault injection is cleared.  Call it ONCE, before the first
 * allocation of the process: the production allocator keeps its free list
 * in statics, so its state cannot be rewound afterwards — rewinding only
 * the program break would make the allocator hand out blocks that
 * overlap the ones it already gave away. */
void libc_heap_runtime_reset(void);

/* Make the next heap growth fail: the following SYS_brk growth returns
 * -ENOMEM, so the allocator must report an out-of-memory failure
 * instead of handing out a block it does not own. */
void libc_heap_runtime_fail_next_grow(void);

/* The current program break of the mock heap (0 when the heap could
 * not be queried). */
uint64_t libc_heap_runtime_brk(void);

/* The base address of the mock heap. */
uint64_t libc_heap_runtime_base(void);

#endif /* OS01_LIBC_HEAP_RUNTIME_H */

/* kernel/include/arch/aarch64/runtime_tree.h
 *
 * aarch64 M1 plan Task 4 — pure runtime page-table builder and strict
 * validator for the BSP's new TTBR1 root.
 *
 * The runtime tree covers exactly three sets per spec §5:
 *   R = the published PMM RAM map (sorted, 2 MiB aligned, ≤16 entries)
 *   B = [0x40000000, 0x40200000) — kernel + handoff normal block
 *   D = [0x08000000, 0x0a000000) — Device-nGnRnE window
 *
 * Each 2 MiB block is installed as an L2 block descriptor (no L3 leaves,
 * no Contiguous bit). Intermediate tables are minimal: V | TYPE_TABLE |
 * PA — any non-zero SBZ bit fails the walk-cache on QEMU TCG and is
 * rejected by the validator.
 *
 * The builder is pure: it never touches a real allocator, MMIO, or the
 * boot-time page-table walker. Instead it calls back through
 * `aarch64_tree_ops` to obtain and resolve page-table pages. Production
 * callbacks hand out monotonically-contiguous PAs from the prepared
 * arena pool (one call == one free PAs from `arena.table_base_pa`,
 * strictly increasing, never reusing a previously-emitted PA). The
 * `resolve` callback is the only way to reach a table page's virtual
 * pointer; it must accept only the PAs the builder has already been
 * issued (i.e. the used pool, not the entire arena) so a forged PA cannot
 * install a backdoor leaf.
 *
 * Host tests use an independent fake-PA pool backed by host heap; the
 * builder/validator are oblivious to the difference.
 *
 * The validator (`aarch64_runtime_tree_validate`) is a separate walk
 * that does NOT reuse the builder's traversal. It enumerates every
 * valid intermediate PA and every L2 block descriptor in the built
 * tree, builds the expected set on the fly from (R, B, D), and asserts:
 *
 *   - the intermediate set is a strict subset of the used pool, every
 *     intermediate PA is unique (no shared tables, no cycles), and
 *     every table-page's PA is inside [table_base_pa,
 *     table_used_end_pa);
 *   - the L2 block set equals the expected set (PA match, every
 *     attribute bit matches: SH, AttrIndx, AF, AP, PXN, UXN, type);
 *   - page_count == number of used pool pages == 1 + #PUDs + #PMDs;
 *   - root_pa is the L0 page (matches the used pool and the chain of
 *     tables).
 *
 * Failure returns a negative errno that names the offending class
 * (-EINVAL for input/range conflicts, -ERANGE for out-of-1TiB PAs,
 * -ENOMEM for pool exhaustion, -EIO for a tree contract violation
 * after build). The builder never publishes a partial tree; on
 * failure `*out` is left zeroed and no callback writes a descriptor.
 */
#ifndef OS01_AARCH64_RUNTIME_TREE_H
#define OS01_AARCH64_RUNTIME_TREE_H

#include <stddef.h>
#include <stdint.h>

#include <memory/memory_map.h>

/* Forward declaration so this header doesn't pull in early_arena.h —
 * the host test fixture and any production caller that already holds
 * an `aarch64_m1_arena *` will include the full header themselves. */
struct aarch64_m1_arena;

/* Page-table pool accessor callbacks. Both are mandatory; the builder
 * returns -EINVAL if either pointer is NULL. The `alloc` callback is
 * the sole way the builder writes to a table page: it issues one fresh
 * PA from the arena pool (or returns -ENOMEM if the pool is
 * exhausted), zeroes the page, and returns both the PA and its
 * already-direct-mapped virtual pointer through `*pa` and `*va`. The
 * `resolve` callback returns the virtual pointer for an already-issued
 * PA — i.e. the caller is expected to have previously handed that PA
 * out via `alloc`. The builder never constructs a VA by adding the PA to
 * a fixed offset; it always asks `resolve`.
 *
 * The host test supplies fake PAs starting at some sentinel (e.g.
 * 0x100000) and allocates 4 KiB host-heap buffers for each one; the
 * production callbacks bump a monotonic PA cursor inside the arena and
 * return PA + ARCH_PAGE_OFFSET (the boot-time M0 identity map covers
 * the arena window). */
struct aarch64_tree_ops {
    void *ctx;
    int   (*alloc)  (void *ctx, uint64_t *pa, uint64_t **va);
    uint64_t *(*resolve)(void *ctx, uint64_t pa);
};

/* The published tree metadata. The builder writes this on success;
 * callers pin `table_used_end_pa` and `page_count` so a subsequent
 * `arch_boot_direct_map_install` can publish the exact range to PoC
 * without re-deriving. */
struct aarch64_runtime_tree {
    /* PA of the L0 page (also the first PA the builder issued). */
    uint64_t root_pa;
    /* PA where the builder started issuing pages. For production
     * this equals `arena->table_base_pa`; for the host test the
     * fake pool's first PA. */
    uint64_t table_base_pa;
    /* PA where the builder stopped issuing pages — exclusive, so
     * the used pool is [table_base_pa, table_used_end_pa). */
    uint64_t table_used_end_pa;
    /* Number of 4 KiB pages actually issued = (table_used_end_pa -
     * table_base_pa) / 4096. Must be ≤ arena->table_pages (the
     * builder refuses to exceed the arena reservation). */
    size_t page_count;
};

/* Caller-provided scratch storage for `aarch64_runtime_tree_validate`.
 *
 * The validator's tree iterator tracks every intermediate PA it
 * descends into (`seen`) and every intermediate PA claimed as a
 * child (`children`). Together these bound the validator's memory
 * cost: 1 L0 + 2 PUDs + 1024 PMDs = 1027 intermediates at most, and
 * 2 + 1024 = 1026 child claims at most. The two arrays must live
 * OUTSIDE the validator's stack frame because the aarch64 BSP boot
 * stack is only AARCH64_BOOT_STACK_SIZE (4 KiB); allocating ~16 KiB
 * of bitmap on the boot stack would overflow. The caller passes a
 * pointer to a `aarch64_runtime_tree_validate_buf` that the caller
 * has placed in BSS, on the arena, or anywhere outside the boot
 * stack. The host test simply declares it as a local on the host
 * stack (which is effectively unbounded).
 *
 * Both arrays are mandatory and zeroed by the validator on entry.
 * The buf may be reused across multiple validate calls. */
struct aarch64_runtime_tree_validate_buf {
    uint64_t seen[1u + 2u + 1024u];      /* 1027 PA slots ≈ 8216 bytes */
    uint64_t children[2u + 1024u];      /* 1026 PA slots ≈ 8208 bytes */
};

/* Build the runtime tree. On success returns 0 and writes `*out`. On
 * failure returns a negative errno and leaves `*out` cleared.
 *
 *   -EINVAL   : null ops / arena / out, NULL ram with count > 0,
 *               count over MEMORY_RANGE_MAX, bad ram type, unaligned
 *               ram endpoints, ram contains B or D, ram out of
 *               1 TiB; ops->alloc returned NULL pa or non-4 KiB
 *               aligned pa; ops->resolve returned NULL for an issued
 *               PA.
 *   -ERANGE   : any ram PA >= AARCH64_M1_PA_LIMIT (1 TiB).
 *   -ENOMEM   : ops->alloc exhausted the pool before all intermediate
 *               pages were issued, or the arena's `table_pages`
 *               reservation would be exceeded.
 *   -EIO      : arithmetic / consistency failure in the builder's
 *               own bookkeeping (defensive; not expected once tests
 *               pass).
 *
 * The build never publishes a partial tree: any failure path leaves
 * the issued pages untouched (they will be cleaned up by the caller —
 * either released back to the PMM or, for the in-process arena pool,
 * frozen as part of the prepared arena). */
int aarch64_runtime_tree_build(const struct MEMORY_RANGE *ram, size_t count,
                               const struct aarch64_m1_arena *arena,
                               const struct aarch64_tree_ops *ops,
                               struct aarch64_runtime_tree *out);

/* Validate the tree produced by `aarch64_runtime_tree_build`. The
 * validator does NOT reuse the builder's traversal; it walks the tree
 * from `tree->root_pa`, enumerates every intermediate PA + every block
 * descriptor, and compares against the expected set derived from
 * (ram, B, D). The arena pointer is used only for `table_pages`
 * (upper bound on used pool size) and the `base_pa` / `end_pa` window
 * the host-identity map covers — both are only consulted for the
 * "no descriptor may exceed the pool bounds" check.
 *
 * `vbuf` MUST be non-NULL and point at caller-provided storage
 * (`struct aarch64_runtime_tree_validate_buf`) for the validator's
 * uniqueness and ownership bitmaps. The caller MUST NOT place
 * `vbuf` on the aarch64 BSP boot stack (4 KiB); the struct is ~16 KiB
 * and would overflow the boot stack. The validator zero-initialises
 * the bitmaps on entry, so a stale vbuf from a prior call is safe.
 *
 * Returns 0 on success. Returns a negative errno on any contract
 * violation:
 *
 *   -EINVAL   : null inputs.
 *   -EIO      : tree contract violation (set, PA, attributes,
 *               uniqueness, ownership, page_count). The validator
 *               does NOT identify the offending slot in the return
 *               code; the caller's invariant is binary.
 *
 * The validator never allocates: it walks the tree through the ops
 * callbacks (which the host test supplies to map fake PAs to host
 * pointers). For production the resolve callback returns
 * PA + ARCH_PAGE_OFFSET (the M0 identity map), same as the builder. */
int aarch64_runtime_tree_validate(const struct MEMORY_RANGE *ram, size_t count,
                                  const struct aarch64_m1_arena *arena,
                                  const struct aarch64_tree_ops *ops,
                                  struct aarch64_runtime_tree_validate_buf *vbuf,
                                  const struct aarch64_runtime_tree *tree);

#endif /* OS01_AARCH64_RUNTIME_TREE_H */
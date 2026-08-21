# getrandom(2) Syscall Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Linux-compatible `getrandom(2)` syscall backed by a ChaCha20 CSPRNG pool seeded from RDRAND/RDSEED, plus a `/dev/urandom` node sharing the same source.

**Architecture:** A stateless ChaCha20 block function lives in `libc/` (compiled into `libk.a` for kernel use). Kernel-private pool state (key, 64-bit block index, spinlock, periodic reseed) lives in `kernel/kernel/random.c`. The syscall and `/dev/urandom` read both funnel through `get_random_bytes()`, guarded by a new per-`mm_t` spinlock (`mm->lock`) plus per-page PTE validation so the kernel never writes an unmapped user VA (a kernel-mode page fault hangs the OS — there is no kernel-side demand paging).

**Tech Stack:** x86_64 freestanding C (clang, `-ffreestanding`), RFC 8439 ChaCha20, RDRAND/RDSEED instructions, OS01's existing VMA/`spinlock_T`/syscall-dispatch infrastructure.

**Spec:** [docs/superpowers/specs/2026-08-21-getrandom-design.md](../specs/2026-08-21-getrandom-design.md) — this plan implements spec v5 exactly (including review-round M-a/M-b/L-a implementation-time notes).

## Global Constraints

These apply to every task; each task's requirements implicitly include them.

- `SYS_getrandom = 66` — the number is added to **both** `kernel/include/uapi/syscall.h` and `libc/include/sys/syscall.h` and they must stay in sync.
- `GRND_NONBLOCK = 0x0001`, `GRND_RANDOM = 0x0002`; both are semantic no-ops (the pool never blocks). Any other flag bit → `-EINVAL`.
- Length cap: `RANDOM_MAX_LEN = 33554431` (Linux urandom ceiling). Values above it are **truncated, not errored**; the truncated value is what is written back and filled.
- Kernel never writes user memory without holding `mm->lock` and validating every page's PTE is `Present | R/W` (4KB pages only; a 2MB `PAGE_PS` PTE makes `vmm_pt_walk` return NULL → treated as `-EFAULT`, which is a documented deviation, not a bug — current userland is all 4KB).
- Spinlock init convention: `.lock = 1L` = unlocked (`spin_lock` = `decq` + `jns`). **`0` is the locked state** — any mm whose lock is left 0 deadlocks the first taker. All mm allocation flows through `mm_alloc()`; `init_mm` is defined `{ .lock = { .lock = 1L } }`.
- `mm->lock` is a **plain** `spin_lock` (not irqsave); the pool lock is `spin_lock_irqsave`. Lock nesting order is fixed and unique: `mm->lock` (outer) → pool lock (inner).
- ChaCha20 API and endianness: `counter` is a `uint32_t`; the 64-bit block index `blk` maps to `counter = (uint32_t)blk` and nonce word 0 = `blk >> 32` in **little-endian** byte order.
- RDRAND/RDSEED are used only for init seeding and periodic reseed (every 1 MiB of output), **never per-call**. A failed reseed (retries exhausted) skips the reseed and leaves the key unchanged — never XOR a zero buffer into the key.
- Kernel sees libc headers via the sysroot (`libc/Makefile install-headers` → `sysroot/usr/include`); top-level `kernel.bin: lib` guarantees ordering. Never `-I` libc/include directly from the kernel.

---

## File Structure

| File | Responsibility | Action |
|---|---|---|
| `libc/random/chacha20.c` | RFC 8439 ChaCha20 block function (pure, stateless) | Create |
| `libc/include/chacha20.h` | `chacha20_block()` declaration + LE endianness contract | Create |
| `libc/Makefile` | Add `random/` to `C_SOURCES` wildcard list | Modify |
| `kernel/include/kernel/arch/x86_64/random.h` | `rdrand64`/`rdseed64` inline asm (return bool) | Create |
| `kernel/include/kernel/arch/random.h` | Arch dispatch header (x86_64 impl, aarch64 stub) | Create |
| `kernel/include/kernel/arch/x86_64/regs.h` | `CPUID_FEAT_ECX_RDRAND`, `CPUID_FEAT_EBX_RDSEED` bits | Modify |
| `kernel/include/kernel/random.h` | `random_init()`/`get_random_bytes()` + `RANDOM_MAX_LEN` | Create |
| `kernel/kernel/random.c` | Pool state, seeding, periodic reseed, chunked fill | Create |
| `kernel/kernel/main.c` | Call `random_init()` at the right point | Modify |
| `kernel/include/kernel/task.h` | Add `spinlock_T lock` to `mm_t`; fix `init_mm` init | Modify |
| `kernel/include/kernel/vma.h` | Declare `mm_alloc()` | Modify |
| `kernel/include/kernel/vmm.h` | Declare `user_write_range_begin/end`; add `<stddef.h>` | Modify |
| `kernel/memory/vma.c` | `mm_alloc()`, `do_munmap_locked()` split, lock set, helper pair | Modify |
| `kernel/sched/task.c` | 3× `calloc(mm_t)` → `mm_alloc()`; reset child lock after fork memcpy | Modify |
| `kernel/include/uapi/syscall.h` | `SYS_getrandom 66` | Modify |
| `libc/include/sys/syscall.h` | `SYS_getrandom 66` | Modify |
| `kernel/arch/x86_64/trap.c` | `linux_to_os01[320]`, `syscall_names[67]`, `SYS_getrandom` case | Modify |
| `kernel/fs/devfs.c` | Rewrite `random_read`; register `/dev/urandom` | Modify |
| `libc/include/sys/random.h` | `GRND_*` macros + `getrandom()` decl | Create |
| `libc/unistd/getrandom.c` | `getrandom()` syscall wrapper | Create |
| `user/systest.c` | `test_getrandom()` + register in `tests[]` | Modify |
| `docs/syscall.md` | syscall count 66→67, getrandom entry, `linux_to_os01[320]` note | Modify |

---

### Task 1: ChaCha20 Block Function in libc

**Files:**
- Create: `libc/random/chacha20.c`
- Create: `libc/include/chacha20.h`
- Modify: `libc/Makefile:22-45`

**Interfaces:**
- Produces: `void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])` — used by Task 2's `kernel/kernel/random.c`. 64 bytes of keystream per call; all multi-byte values little-endian.

- [ ] **Step 1: Write the header**

Create `libc/include/chacha20.h`:

```c
#ifndef _CHACHA20_H
#define _CHACHA20_H

#include <stdint.h>

// RFC 8439 ChaCha20 block function — stateless, no allocation, no globals.
// Generates one 64-byte block of keystream:
//   key     : 32-byte key
//   counter : 32-bit block counter (little-endian word 12 of the state)
//   nonce   : 12-byte nonce (three little-endian 32-bit words, state 13..15)
//   out     : 64 bytes of keystream written here
//
// The caller (kernel PRNG) is responsible for monotonic (counter, nonce)
// pairs so keystream never repeats.  All 32-bit words are serialized
// little-endian, per RFC 8439 §2.3.
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);

#endif // _CHACHA20_H
```

- [ ] **Step 2: Write the implementation**

Create `libc/random/chacha20.c`:

```c
#include <chacha20.h>

// Rotate left. `c` is a compile-time constant in every use site.
static inline uint32_t rotl32(uint32_t v, int c)
{
    return (v << c) | (v >> (32 - c));
}

// One quarter-round: a += b; d ^= a; d <<<= 16; c += d; b ^= c; b <<<= 12;
//                      a += b; d ^= a; d <<<= 8;  c += d; b ^= c; b <<<= 7;
#define QR(a, b, c, d) do {     \
    (a) += (b); (d) ^= (a); (d) = rotl32((d), 16); \
    (c) += (d); (b) ^= (c); (b) = rotl32((b), 12); \
    (a) += (b); (d) ^= (a); (d) = rotl32((d), 8);  \
    (c) += (d); (b) ^= (c); (b) = rotl32((b), 7);  \
} while (0)

static inline uint32_t load32_le(const uint8_t p[4])
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void store32_le(uint8_t p[4], uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64])
{
    // State layout (16 little-endian 32-bit words):
    //   s[0..3]   = constant "expand 32-byte k"
    //   s[4..11]  = key (8 words)
    //   s[12]     = counter
    //   s[13..15] = nonce (3 words)
    uint32_t s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e;
    s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        s[4 + i] = load32_le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = load32_le(nonce + 4 * i);

    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = s[i];

    for (int i = 0; i < 10; i++) {
        // Column rounds
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        // Diagonal rounds
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++)
        store32_le(out + 4 * i, x[i] + s[i]);
}
```

- [ ] **Step 3: Add `random/` to the libc build**

In `libc/Makefile`, add one line to the `C_SOURCES` wildcard list (after `pthread/*.c` at line 45, before the closing `)`):

```makefile
    $(wildcard pthread/*.c) \
    $(wildcard random/*.c)
```

(`random/` must be a `$(wildcard ...)` entry like every other directory — the list is explicit per-directory wildcards; a new `.c` file is not picked up otherwise. `install-headers` already copies `libc/include/.` to sysroot, so `chacha20.h` needs no extra Makefile work.)

- [ ] **Step 4: Build libc**

Run: `make -C libc clean && make -C libc`
Expected: `libc.a` and `libk.a` built with no errors; `build/x86_64/libc/random/chacha20.o` and `.../chacha20.libk.o` both exist.

- [ ] **Step 5: Commit**

```bash
git add libc/random/chacha20.c libc/include/chacha20.h libc/Makefile
git commit -m "feat(libc): add RFC 8439 ChaCha20 block function to libk/libc"
```

---

### Task 2: Kernel PRNG (`random.c`) + Arch RDRAND/RDSEED

**Files:**
- Create: `kernel/include/kernel/arch/x86_64/random.h`
- Create: `kernel/include/kernel/arch/random.h`
- Modify: `kernel/include/kernel/arch/x86_64/regs.h:146-148`
- Create: `kernel/include/kernel/random.h`
- Create: `kernel/kernel/random.c`
- Modify: `kernel/kernel/main.c:170-174`

**Interfaces:**
- Consumes: `chacha20_block()` (Task 1), `arch_cycle_counter()` (`<kernel/arch/cpu.h>`), `jiffies` (`<device/timer.h>`), `spin_lock_irqsave`/`spin_unlock_irqrestore` (`<kernel/arch/spinlock.h>`), `log_warn` (`<kernel/log.h>`).
- Produces: `void random_init(void)`, `void get_random_bytes(void *buf, size_t len)` — used by Task 4 (trap.c) and Task 5 (devfs.c). Also `#define RANDOM_MAX_LEN 33554431UL`.

- [ ] **Step 1: Add CPUID feature bits**

In `kernel/include/kernel/arch/x86_64/regs.h`, inside the CPUID feature-bit block (extend the ECX section at lines 146-148, add a new EBX section for leaf 7):

```c
// ECX bits
#define CPUID_FEAT_ECX_X2APIC  (1 << 21)   // x2APIC supported
#define CPUID_FEAT_ECX_RDRAND  (1 << 30)   // RDRAND instruction (leaf 1 ECX)

// EBX bits (leaf 7, subleaf 0)
#define CPUID_FEAT_EBX_RDSEED  (1 << 18)   // RDSEED instruction (leaf 7 EBX)
```

- [ ] **Step 2: Write the x86_64 inline asm**

Create `kernel/include/kernel/arch/x86_64/random.h`:

```c
#ifndef _KERNEL_ARCH_X86_64_RANDOM_H
#define _KERNEL_ARCH_X86_64_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

// RDRAND/RDSEED with bounded retry (Intel SDM recommends ≤ 10 attempts).
// Returns true and writes `*out` on success (CF=1); false if retries
// exhausted (instruction genuinely did not produce entropy).

static inline bool rdrand64(uint64_t *out)
{
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        unsigned char cf;
        __asm__ __volatile__(
            "rdrand %0; setc %1"
            : "=r"(v), "=qm"(cf)
            :
            : "cc");
        if (cf) { *out = v; return true; }
    }
    return false;
}

static inline bool rdseed64(uint64_t *out)
{
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        unsigned char cf;
        __asm__ __volatile__(
            "rdseed %0; setc %1"
            : "=r"(v), "=qm"(cf)
            :
            : "cc");
        if (cf) { *out = v; return true; }
    }
    return false;
}

#endif // _KERNEL_ARCH_X86_64_RANDOM_H
```

- [ ] **Step 3: Write the arch dispatch header**

Create `kernel/include/kernel/arch/random.h` (same pattern as `kernel/include/kernel/arch/cpuid.h`):

```c
#ifndef _ARCH_RANDOM_H
#define _ARCH_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/random.h>
#elif defined(__aarch64__)
// aarch64 stub: no RDRAND/RDSEED.  Always returns false — the caller falls
// back to arch_cycle_counter().  Future work: RNDR/RNDRRS registers.
static inline bool rdrand64(uint64_t *out) { (void)out; return false; }
static inline bool rdseed64(uint64_t *out) { (void)out; return false; }
#else
#error "Unsupported architecture"
#endif

#endif // _ARCH_RANDOM_H
```

- [ ] **Step 4: Write the kernel PRNG header**

Create `kernel/include/kernel/random.h`:

```c
#ifndef _KERNEL_RANDOM_H
#define _KERNEL_RANDOM_H

#include <stddef.h>

// Linux urandom per-call ceiling; larger requests are truncated, not errored.
#define RANDOM_MAX_LEN 33554431UL

void random_init(void);                     // boot-time, once (BSP)
void get_random_bytes(void *buf, size_t len); // any context (IRQ-safe)

#endif // _KERNEL_RANDOM_H
```

- [ ] **Step 5: Write the PRNG implementation**

Create `kernel/kernel/random.c`:

```c
// kernel/kernel/random.c — ChaCha20 CSPRNG pool + RDRAND/RDSEED reseed.
//
// Stateless algorithm lives in libc (chacha20.c, linked via libk.a); this
// file owns the secret state: the 32-byte key, a 64-bit block index, and
// the pool spinlock.  RDRAND/RDSEED are used ONLY for initial seeding and
// periodic reseed (every 1 MiB of output) — never per-call — because RDRAND
// throughput is bounded and per-call rekeying would turn a CSPRNG back into
// a hardware dependency without adding security.
#include <kernel/random.h>
#include <kernel/arch/random.h>
#include <kernel/arch/cpu.h>        // arch_cycle_counter()
#include <kernel/arch/spinlock.h>
#include <kernel/log.h>
#include <device/timer.h>            // jiffies
#include <chacha20.h>
#include <string.h>
#include <stdbool.h>

#define RESEED_INTERVAL (1u << 20)   // 1 MiB of output between reseeds

static uint8_t  pool_key[32];
static uint64_t pool_blk;            // 64-bit block index (see below)
static uint64_t pool_bytes_since_reseed;
static spinlock_T pool_lock = { .lock = 1L };
static bool random_ready;

// Mix 32 bytes of hardware entropy.  Prefer RDSEED (higher quality, lower
// throughput) then RDRAND; if both fail (old CPU / aarch64 stub), fall back
// to a high-frequency cycle-counter sample — weak but no worse than the
// pre-existing rdtsc-based /dev/random.
static void mix_hw_entropy(uint8_t out[32])
{
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        if (!rdseed64(&w[i]) && !rdrand64(&w[i]))
            w[i] = arch_cycle_counter() ^ jiffies;
    }
    memcpy(out, w, 32);
}

void random_init(void)
{
    mix_hw_entropy(pool_key);
    pool_blk = 0;
    pool_bytes_since_reseed = 0;
    random_ready = true;
}

// Periodic reseed: XOR fresh hardware entropy into the key.  On RDRAND/
// RDSEED failure mix_hw_entropy() already falls back to cycle-counter, so
// this never XORs an uninitialized/zero buffer into the key.
static void reseed(void)
{
    uint8_t hw[32];
    mix_hw_entropy(hw);
    for (int i = 0; i < 32; i++)
        pool_key[i] ^= hw[i];
    pool_bytes_since_reseed = 0;
}

void get_random_bytes(void *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (!random_ready) {
        static bool warned;
        if (!warned) {
            log_warn("get_random_bytes before random_init — deterministic output\n");
            warned = true;
        }
    }

    uint8_t *out = (uint8_t *)buf;

    // Chunked fill: release the pool lock every 64 KiB so a large request
    // doesn't serialize every CPU's random generation in one long critical
    // section.
    while (len > 0) {
        size_t chunk = len;
        if (chunk > (64u << 10))
            chunk = 64u << 10;

        uint64_t flags = spin_lock_irqsave(&pool_lock);

        for (size_t off = 0; off < chunk; off += 64) {
            uint8_t block[64];
            uint8_t nonce[12] = {0};

            // 64-bit block index → (counter, nonce[0]) with little-endian
            // byte order (RFC 8439): low 32 bits are the counter, high 32
            // bits are nonce word 0.  When the low 32 bits wrap, the carry
            // moves into the high bits, so no (counter, nonce) pair ever
            // repeats — the keystream space is 2^96 blocks.
            uint32_t counter = (uint32_t)pool_blk;
            nonce[0] = (uint8_t)(pool_blk >> 32);
            nonce[1] = (uint8_t)(pool_blk >> 40);
            nonce[2] = (uint8_t)(pool_blk >> 48);
            nonce[3] = (uint8_t)(pool_blk >> 56);

            chacha20_block(pool_key, counter, nonce, block);

            size_t n = 64;
            if (off + n > chunk)
                n = chunk - off;
            memcpy(out + off, block, n);

            pool_blk++;
        }

        pool_bytes_since_reseed += chunk;
        if (pool_bytes_since_reseed >= RESEED_INTERVAL)
            reseed();

        spin_unlock_irqrestore(&pool_lock, flags);

        out += chunk;
        len -= chunk;
    }
}
```

- [ ] **Step 6: Call `random_init()` at boot**

In `kernel/kernel/main.c`, insert the call between `subsys_init_all()` and `vfs_init()`/`devfs_init()` (i.e., after line 170, before line 172). Add `#include <kernel/random.h>` to main.c's includes:

```c
    arch_register_subsys();
    subsys_init_all();

    random_init();                      // seed the CSPRNG pool (BSP, once)

    vfs_init();                         // init mount table BEFORE any mount calls
```

- [ ] **Step 7: Build the kernel**

Run: `make -C libc install-headers && make -C kernel kernel.bin` (the install-headers puts `chacha20.h` into sysroot for `kernel/kernel/random.c`'s `#include <chacha20.h>`).
Expected: `kernel.bin` links with no errors. (This is a compile/link gate — the PRNG has no visible consumer yet, which is fine.)

- [ ] **Step 8: Commit**

```bash
git add kernel/include/kernel/arch/x86_64/random.h kernel/include/kernel/arch/random.h \
        kernel/include/kernel/arch/x86_64/regs.h kernel/include/kernel/random.h \
        kernel/kernel/random.c kernel/kernel/main.c
git commit -m "feat(kernel): ChaCha20 CSPRNG pool with RDRAND/RDSEED periodic reseed"
```

---

### Task 3: mm->lock + user-write-range validation (TOCTOU closure)

This is the highest-risk task: a wrong lock init deadlocks the whole OS, and it must land before the syscall/devfs consumers so the existing mmap/mprotect regression suite is its test gate.

**Files:**
- Modify: `kernel/include/kernel/task.h:76-89` (add `lock` field), `:199-222` (init_mm)
- Modify: `kernel/include/kernel/vma.h:46-57` (declare `mm_alloc`)
- Modify: `kernel/include/kernel/vmm.h` (declare helper pair; add `<stddef.h>`)
- Modify: `kernel/memory/vma.c` (constructor, `do_munmap_locked`, lock set, helper pair)
- Modify: `kernel/sched/task.c:1091,1309,1485,1611` (calloc→mm_alloc, fork lock reset)

**Interfaces:**
- Consumes: `spinlock_T`/`spin_lock`/`spin_unlock`/`spin_init` (`<kernel/arch/spinlock.h>`), `vmm_pt_walk`, `PAGE_Present`/`PAGE_R_W` (`<kernel/vmm.h>`), `current`/`mm_t` (`<kernel/task.h>`).
- Produces:
  - `mm_t *mm_alloc(void)` — used by Task 3 itself (task.c) and any future mm site.
  - `int user_write_range_begin(uint64_t addr, size_t len)` / `void user_write_range_end(void)` — used by Task 4 (trap.c) and Task 5 (devfs.c).

- [ ] **Step 1: Add `lock` to `mm_t` and fix `init_mm`**

In `kernel/include/kernel/task.h`, add the explicit spinlock include near the top (after line 12) and the field (after `mmap_base` in `mm_t`):

```c
#include <stdbool.h>
#include <kernel/arch/spinlock.h>
```

```c
    // ── mmap / VMA support ──────────────────────────────
    list_t   vma_list;    // sorted by vm_start
    uint64_t mmap_base;   // start search address for mmap
    spinlock_T lock;      // guards munmap/MAP_FIXED/mprotect vs getrandom write
} mm_t;
```

Change the definition at line 222:

```c
mm_t init_mm = {0};
```

to:

```c
// .lock = { .lock = 1L }: mm_t.lock is a spinlock_T whose own field is
// `lock`.  1 = unlocked; leaving it 0 would deadlock the first task that
// takes it (INIT_TASK points .mm at this struct).
mm_t init_mm = { .lock = { .lock = 1L } };
```

- [ ] **Step 2: Declare the new symbols**

In `kernel/include/kernel/vma.h`, after `fork_vma_copy` (line 51):

```c
mm_t      *mm_alloc(void);   // allocate + init an mm_t (lock = unlocked)
```

In `kernel/include/kernel/vmm.h`, add `#include <stddef.h>` after line 4 (`#include <stdint.h>`), and declare after `vmm_pt_walk` (line 59):

```c
// Validate-and-lock a user write range before the kernel writes into it
// (getrandom / devfs random read).  Returns 0 with current->mm->lock HELD
// on success — caller MUST call user_write_range_end().  Returns -EFAULT
// (lock NOT held) on any bad address or non-writable page.  Kernel buffers
// (current->mm == NULL) are trusted and pass through without lock/check.
int  user_write_range_begin(uint64_t addr, size_t len);
void user_write_range_end(void);
```

- [ ] **Step 3: Add `mm_alloc`, `do_munmap_locked`, lock set, and the helper pair to vma.c**

Add `#include <kernel/arch/spinlock.h>` to `kernel/memory/vma.c`'s includes (it now takes locks). Then make four edits:

**(a)** Add `mm_alloc` after `fork_vma_copy` (before the `prot_to_page_flags` helper at line 117):

```c
// mm_alloc — allocate + initialize an mm_t.  Centralizes the "lock = 1L"
// invariant so no call site can forget it (lock == 0 is the LOCKED state
// and would deadlock the first taker).
mm_t *mm_alloc(void)
{
    mm_t *mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (!mm) return NULL;
    list_init(&mm->vma_list);
    mm->mmap_base = 0x40000000;
    spin_init(&mm->lock);
    return mm;
}
```

**(b)** Split `do_munmap` (currently lines 161-217) into `do_munmap_locked` + `do_munmap`. Replace the entire `do_munmap` function with:

```c
// ── do_munmap ─────────────────────────────────────────────────
// do_munmap_locked — unmaps WITHOUT taking mm->lock (caller holds it).
// Defined before do_mmap because do_mmap(MAP_FIXED) calls it.
static int64_t do_munmap_locked(uint64_t addr, uint64_t length)
{
    addr   = PAGE_4K_ALIGN(addr);
    length = PAGE_4K_ALIGN(length);
    if (length == 0)
        return -EINVAL;

    uint64_t end = addr + length;
    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);

    list_t *pos = current->mm->vma_list.next;
    while (pos != &current->mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        pos = pos->next;

        if (v->vm_end <= addr)   continue;
        if (v->vm_start >= end)  break;

        uint64_t u_start = (addr > v->vm_start) ? addr : v->vm_start;
        uint64_t u_end   = (end  < v->vm_end)   ? end  : v->vm_end;
        for (uint64_t va = u_start; va < u_end; va += PAGE_4K_SIZE)
            vmm_unmap_4k_page(user_pml4, va);

        uint64_t orig_start = v->vm_start;
        uint64_t orig_end   = v->vm_end;

        if (u_start <= orig_start && u_end >= orig_end) {
            vma_remove(current->mm, v);
            continue;
        }

        if (u_start > orig_start && u_end < orig_end) {
            // Split: left + right
            v->vm_end = u_start;

            vma_t *right = (vma_t *)kmalloc(sizeof(vma_t));
            if (right) {
                memcpy(right, v, sizeof(vma_t));
                list_init(&right->list);
                right->vm_start = u_end;
                right->vm_end   = orig_end;
                if (right->vm_file)
                    vfs_node_get(right->vm_file);
                vma_insert(current->mm, right);
            }
        } else if (u_start > orig_start) {
            // Truncate right side
            v->vm_end = u_start;
        } else {
            // Truncate left side
            v->vm_start = u_end;
        }
    }

    flush_tlb();
    return 0;
}

// do_munmap — public entry: take mm->lock, then unmap.
int64_t do_munmap(uint64_t addr, uint64_t length)
{
    spin_lock(&current->mm->lock);
    int64_t rc = do_munmap_locked(addr, length);
    spin_unlock(&current->mm->lock);
    return rc;
}
```

**(c)** In `do_mmap`, change the MAP_FIXED branch (currently line 276: `do_munmap(addr, length);`) to take the lock and call the `_locked` variant (avoiding recursive deadlock):

```c
    } else {
        spin_lock(&current->mm->lock);
        do_munmap_locked(addr, length);
        spin_unlock(&current->mm->lock);
    }
```

**(d)** In `do_mprotect`, take `mm->lock` after argument validation and release it before every return in the locked region. The three changes to the existing body:

- After `if (rc) return rc;` (line 376), insert `spin_lock(&current->mm->lock);`
- Replace the hole check `if (v->vm_start > addr) return -ENOMEM;` (lines 387-388) with:

```c
        // Hole check
        if (v->vm_start > addr) {
            spin_unlock(&current->mm->lock);
            return -ENOMEM;
        }
```

- After the loop's closing brace (before `if (addr < end)`), insert `spin_unlock(&current->mm->lock);`:

```c
        addr = v->vm_end;
        if (addr >= end) break;
        pos = pos->next;
    }

    spin_unlock(&current->mm->lock);

    if (addr < end)
        return -ENOMEM;

    flush_tlb();
    return 0;
```

- [ ] **Step 4: Add the `user_write_range_begin/end` helpers**

Append to `kernel/memory/vma.c` (after `do_mprotect`):

```c
// ── user_write_range_begin/end — validate + lock a kernel→user write ──
// Closes the TOCTOU between "check the pages are mapped+writable" and
// "write them": the caller holds current->mm->lock for the whole fill, and
// munmap/MAP_FIXED/mprotect take the same lock, so no concurrent call can
// tear down or narrow a page mid-write (which would fault into the
// do_page_fault hlt hang — there is no kernel-side demand paging).
//
// On success returns 0 with mm->lock HELD; on any failure returns -EFAULT
// and the lock is NOT held.  current->mm == NULL (kthread reading
// /dev/urandom) skips the check/lock entirely — its buffer is a trusted
// kernel buffer.
//
// Known limitation: vmm_pt_walk returns NULL for a 2MB huge-page PDE
// (PAGE_PS), so a buffer in a 2MB user mapping would false-positive
// -EFAULT.  Current userland is entirely 4KB pages; revisit if 2MB user
// mappings appear.
int user_write_range_begin(uint64_t addr, size_t len)
{
    if (current->mm == NULL)
        return 0;

    if (addr == 0 || addr >= current->addr_limit ||
        len > current->addr_limit - addr)
        return -EFAULT;

    spin_lock(&current->mm->lock);

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    for (uint64_t va = addr & PAGE_4K_MASK; va < addr + len; va += PAGE_4K_SIZE) {
        uint64_t *pte = vmm_pt_walk(user_pml4, va, 0, 0);
        // Require present + writable.  A COW page (P=1, W=0, PAGE_COW set)
        // fails here — a documented deviation from Linux's COW-fault-on-write.
        if (!pte || !(*pte & (PAGE_Present | PAGE_R_W))) {
            spin_unlock(&current->mm->lock);
            return -EFAULT;
        }
    }
    return 0;   // lock held
}

void user_write_range_end(void)
{
    if (current->mm != NULL)
        spin_unlock(&current->mm->lock);
}
```

- [ ] **Step 5: Replace the three `calloc(mm_t)` sites and reset the fork-child lock**

In `kernel/sched/task.c`:

Site 1 (lines 1091-1095) — replace:

```c
    mm_t *mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (mm) {
        list_init(&mm->vma_list);
        mm->mmap_base = 0x40000000;
    }
```

with:

```c
    mm_t *mm = mm_alloc();
```

Site 2 (lines 1309-1313) — replace:

```c
    mm_t *new_mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (new_mm) {
        list_init(&new_mm->vma_list);
        new_mm->mmap_base = 0x40000000;
    }
```

with:

```c
    mm_t *new_mm = mm_alloc();
```

Site 3 (line 1485) — replace:

```c
    mm_t *child_mm = (mm_t *)calloc(1, sizeof(mm_t));
```

with:

```c
    mm_t *child_mm = mm_alloc();
```

Fork-child reset (after `memcpy(child_mm, parent_mm, sizeof(mm_t));` at line 1611, and the `list_init` line that follows):

```c
    memcpy(child_mm, parent_mm, sizeof(mm_t));
    // vma_list must NOT be shared — fork_vma_copy will fill child's own
    list_init(&child_mm->vma_list);
    spin_init(&child_mm->lock);   // memcpy copied parent's lock value — reset
    child_mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)child_pml4);
```

- [ ] **Step 6: Build and run the mmap regression gate**

Run: `make -C kernel kernel.bin && make OS01_SYSTEST=1 disk.img` (or just `make OS01_SYSTEST=1 disk.img`), then `python3 tests/run_test.py systest`.
Expected: all existing tests pass (the mmap/mprotect/fork-mmap/COW suites exercise `do_munmap`, `do_mmap(MAP_FIXED)`, `do_mprotect`, and `fork_mm_copy` — the exact paths this task changed). No deadlock on boot (init process takes `mm->lock` via `init_mm`).

- [ ] **Step 7: Commit**

```bash
git add kernel/include/kernel/task.h kernel/include/kernel/vma.h \
        kernel/include/kernel/vmm.h kernel/memory/vma.c kernel/sched/task.c
git commit -m "feat(mem): add mm->lock + user_write_range validation (TOCTOU closure)"
```

---

### Task 4: Syscall Wiring (`SYS_getrandom` = 66)

**Files:**
- Modify: `kernel/include/uapi/syscall.h:90`
- Modify: `libc/include/sys/syscall.h:74`
- Modify: `kernel/arch/x86_64/trap.c` (linux_to_os01 guard+array, syscall_names, new case, includes)

**Interfaces:**
- Consumes: `get_random_bytes()` + `RANDOM_MAX_LEN` (Task 2), `user_write_range_begin/end` (Task 3), `GRND_NONBLOCK`/`GRND_RANDOM` (`<sys/random.h>` from Task 6 — note: this task must be ordered after Task 6's header is installed, OR include the flags locally; see Step 1 note).
- Produces: `SYS_getrandom = 66` (used by Task 6's libc wrapper and Task 7's test).

- [ ] **Step 1: Add the syscall number to both uapi headers**

In `kernel/include/uapi/syscall.h`, after line 90:

```c
#define SYS_clock_gettime 65
#define SYS_getrandom     66
```

In `libc/include/sys/syscall.h`, after line 74:

```c
#define SYS_clock_gettime 65
#define SYS_getrandom     66
```

> **Ordering note:** trap.c reads `GRND_NONBLOCK`/`GRND_RANDOM` from `<sys/random.h>`, which Task 6 creates in `libc/`. If you implement in order, Task 6's header must be installed (`make -C libc install-headers`) before this task builds. If you prefer this task to be self-contained, do Task 6's Step 1 (the `sys/random.h` header) first. The commit boundaries stay clean either way.

- [ ] **Step 2: Expand `linux_to_os01` to `[320]`**

In `kernel/arch/x86_64/trap.c`, change the guard (line 977) and array size (line 978):

```c
    if ((current->flags & PF_LINUX_ABI) && regs->rax < 320) {
        static const int8_t linux_to_os01[320] = {
```

Add the getrandom entry at the end of the initializer (after `[228] = 65` at line 1035):

```c
			[228] = 65,	// clock_gettime	→ SYS_clock_gettime
			[318] = 66,	// getrandom		→ SYS_getrandom
        };
```

(`int8_t` holds 66 fine; Linux x86_64 getrandom = syscall 318.)

- [ ] **Step 3: Add the syscall name**

In `kernel/arch/x86_64/trap.c`, change `syscall_names[66]` (line 1043) to `[67]` and add the entry after `[65]` (line 1099):

```c
    static const char *syscall_names[67] = {
        ...
        [65] = "clock_gettime",
        [66] = "getrandom",
    };
```

And update the `regs->rax < 66` guard on line 1101 to `< 67`.

- [ ] **Step 4: Add the dispatch case**

Add `#include <sys/random.h>` and `#include <kernel/random.h>` to trap.c's includes. Then insert the case (e.g., after `case SYS_clock_gettime` at line 1896):

```c
    case SYS_getrandom: {
        // getrandom(void *buf, size_t len, unsigned int flags)
        uint64_t addr  = regs->rdi;
        uint64_t len   = regs->rsi;
        uint64_t flags = regs->rdx;

        if (len == 0) { regs->rax = 0; break; }              // buf may be NULL
        if (flags & ~(GRND_NONBLOCK | GRND_RANDOM)) {        // pool never blocks
            regs->rax = -EINVAL; break;
        }
        if (len > RANDOM_MAX_LEN) len = RANDOM_MAX_LEN;      // truncate, not error

        int rc = user_write_range_begin(addr, len);          // mm->lock + per-page PTE
        if (rc < 0) { regs->rax = rc; break; }               // -EFAULT (lock released)
        get_random_bytes((void *)addr, len);                 // chunked pool fill; mm->lock held
        user_write_range_end();
        regs->rax = len;                                     // actual bytes filled
        break;
    }
```

- [ ] **Step 5: Build**

Run: `make -C libc install-headers && make -C kernel kernel.bin`
Expected: clean compile/link. (No userland caller yet; Task 7 adds the test.)

- [ ] **Step 6: Commit**

```bash
git add kernel/include/uapi/syscall.h libc/include/sys/syscall.h kernel/arch/x86_64/trap.c
git commit -m "feat(syscall): wire SYS_getrandom (66) + linux_to_os01[318] translation"
```

---

### Task 5: `/dev/urandom` via the shared helper

**Files:**
- Modify: `kernel/fs/devfs.c:113-125` (random_read), `:399-402` (registration), includes

**Interfaces:**
- Consumes: `get_random_bytes()` + `RANDOM_MAX_LEN` (Task 2), `user_write_range_begin/end` (Task 3).
- Produces: `/dev/urandom` character device node (same ops as `/dev/random`), read by Task 7's test.

- [ ] **Step 1: Rewrite `random_read`**

Add `#include <kernel/random.h>` and `#include <kernel/vmm.h>` to `kernel/fs/devfs.c`'s includes. Replace the body of `random_read` (lines 113-125):

```c
// ── /dev/random + /dev/urandom — kernel CSPRNG pool ────────
// Same source as getrandom(2); uses the same user_write_range_begin/end
// helper so the devfs read path has the same kernel-write safety as the
// syscall (a deliberate deviation from the other devices' raw writes —
// see spec §5.2).  Cap mirrors the syscall so one huge read can't hold
// mm->lock while serializing that mm's munmap/mprotect.
static int random_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    if (size > RANDOM_MAX_LEN) size = RANDOM_MAX_LEN;

    int rc = user_write_range_begin((uint64_t)buffer, (size_t)size);
    if (rc < 0) return rc;                 // -EFAULT (lock released)
    get_random_bytes(buffer, (size_t)size);
    user_write_range_end();
    return (int)size;
}
```

(`random_write` is unchanged — it accepts and ignores.)

- [ ] **Step 2: Register `/dev/urandom`**

In `devfs_init` (after line 401), add the urandom registration reusing `random_ops`:

```c
    devfs_register_chrdev("random", NULL, &random_ops);
    devfs_register_chrdev("urandom", NULL, &random_ops);
```

- [ ] **Step 3: Build**

Run: `make -C kernel kernel.bin`
Expected: clean compile/link.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/devfs.c
git commit -m "feat(devfs): /dev/urandom node sharing the CSPRNG pool; symmetric write safety"
```

---

### Task 6: libc Userland Interface

**Files:**
- Create: `libc/include/sys/random.h`
- Create: `libc/unistd/getrandom.c`
- (Modify: `libc/include/sys/syscall.h` — already done in Task 4 Step 1)

**Interfaces:**
- Consumes: `SYS_getrandom` (Task 4), the 3-arg `syscall()` inline (`<sys/syscall.h>`), `ssize_t`/`size_t` (`<sys/types.h>`).
- Produces: `ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)` + `GRND_NONBLOCK`/`GRND_RANDOM` — used by Task 7.

- [ ] **Step 1: Write the header**

Create `libc/include/sys/random.h`:

```c
#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <sys/types.h>   // size_t, ssize_t

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

#ifdef __cplusplus
extern "C" {
#endif

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif // _SYS_RANDOM_H
```

- [ ] **Step 2: Write the syscall wrapper**

Create `libc/unistd/getrandom.c` (follows the `read.c`/`open.c` wrapper convention: `__is_libk` stub, errno translation):

```c
#include <sys/random.h>
#include <errno.h>
#include <sys/syscall.h>

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
#if defined(__is_libk)
    (void)buf; (void)buflen; (void)flags;
    return -1;   // not implemented in kernel mode
#else
    int64_t ret = syscall(SYS_getrandom, (uint64_t)buf, (uint64_t)buflen,
                          (uint64_t)flags);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (ssize_t)ret;
#endif
}
```

(`libc/unistd/` is already in the `C_SOURCES` wildcard list, so no Makefile change. `sys/random.h` is installed to sysroot by the existing `install-headers` rule.)

- [ ] **Step 3: Build libc**

Run: `make -C libc clean && make -C libc`
Expected: `build/x86_64/libc/unistd/getrandom.o` and `.libk.o` built.

- [ ] **Step 4: Commit**

```bash
git add libc/include/sys/random.h libc/unistd/getrandom.c
git commit -m "feat(libc): getrandom(2) wrapper + GRND_* flags"
```

---

### Task 7: systest `test_getrandom`

**Files:**
- Modify: `user/systest.c` (add `#include <sys/random.h>`, `#include <sys/mman.h>`, `#include <fcntl.h>`; add `test_getrandom()`; register it in `tests[]`)

**Interfaces:**
- Consumes: `getrandom()` (Task 6), `mmap`/`munmap` (`<sys/mman.h>`), `open`/`read`/`close`, `fork`/`waitpid`/`_exit`, `malloc`/`free`, `pipe`/`write`/`read`.

- [ ] **Step 1: Write the test**

Add the includes near the top of `systest.c` (after line 24):

```c
#include <sys/random.h>
#include <sys/mman.h>
#include <fcntl.h>
```

Add `test_getrandom` (place it just before the `// ── Runner ──` section around line 1531):

```c
// ── 66: getrandom(2) + /dev/urandom ─────────────────────────
static void test_getrandom(void)
{
    uint8_t a[32], b[32];
    memset(a, 0, 32);
    memset(b, 0, 32);

    // 1. Basic: returns 32, non-zero, two calls differ.
    errno = 0;
    ssize_t ra = getrandom(a, 32, 0);
    CHECK3(ra == 32, "getrandom", "basic returns 32");
    int nz = 0;
    for (int i = 0; i < 32; i++) if (a[i]) nz++;
    CHECK3(nz > 0, "getrandom", "non-zero output");
    ssize_t rb = getrandom(b, 32, 0);
    CHECK3(rb == 32, "getrandom", "second call returns 32");
    CHECK3(memcmp(a, b, 32) != 0, "getrandom", "two calls differ");

    // 2. Flags: both GRND flags (and their OR) accepted; unknown → EINVAL.
    CHECK3(getrandom(a, 32, GRND_NONBLOCK) == 32, "getrandom", "GRND_NONBLOCK ok");
    CHECK3(getrandom(a, 32, GRND_RANDOM) == 32, "getrandom", "GRND_RANDOM ok");
    CHECK3(getrandom(a, 32, GRND_NONBLOCK | GRND_RANDOM) == 32, "getrandom", "both flags ok");
    errno = 0;
    ssize_t rbad = getrandom(a, 32, 0x100);
    CHECK3(rbad == -1 && errno == EINVAL, "getrandom", "bad flag EINVAL");

    // 3. Bad pointer: out-of-range → EFAULT.
    errno = 0;
    ssize_t r0 = getrandom((void *)0xFFFF800000000000ULL, 32, 0);
    CHECK3(r0 == -1 && errno == EFAULT, "getrandom", "out-of-range EFAULT");

    // 3b. Deterministic unmapped page: mmap one page, munmap it, use that VA.
    //     Must be in-range and unmapped → exercises the per-page PTE check.
    //     NOT a COW/MAP_PRIVATE file map (that's a documented -EFAULT deviation).
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        munmap(p, 4096);
        errno = 0;
        ssize_t r1 = getrandom(p, 4096, 0);
        CHECK3(r1 == -1 && errno == EFAULT, "getrandom", "unmapped VA EFAULT");
    } else {
        FAIL("getrandom: mmap for EFAULT test");
    }

    // 4. len=0 → 0 (buf may be NULL).
    errno = 0;
    CHECK3(getrandom(NULL, 0, 0) == 0, "getrandom", "len=0 returns 0");

    // 5. Large len (1 MiB) must not hang (exercises chunked lock release).
    //     Touch the buffer first: anon pages are demand-mapped on first
    //     access; user_write_range_begin requires PTEs to already exist.
    {
        size_t big = 1024 * 1024;
        void *buf = mmap(NULL, big, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK3(buf != MAP_FAILED, "getrandom", "1MiB mmap");
        if (buf != MAP_FAILED) {
            memset(buf, 0, big);   // fault in all pages
            errno = 0;
            ssize_t r = getrandom(buf, big, 0);
            CHECKF(r == (ssize_t)big, "getrandom", "1MiB = %ldB",
                   "1MiB = %ldB", (long)r);
            munmap(buf, big);
        }
    }

    // 6. /dev/urandom: read 16B, two reads differ; unmapped VA → read < 0.
    {
        int fd = open("/dev/urandom", O_RDONLY);
        CHECK3(fd >= 0, "getrandom", "/dev/urandom open");
        if (fd >= 0) {
            uint8_t ua[16], ub[16];
            ssize_t n1 = read(fd, ua, 16);
            ssize_t n2 = read(fd, ub, 16);
            CHECK3(n1 == 16 && n2 == 16, "getrandom", "/dev/urandom read 16");
            CHECK3(memcmp(ua, ub, 16) != 0, "getrandom", "/dev/urandom two reads differ");
            close(fd);
        }
        void *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (q != MAP_FAILED) {
            munmap(q, 4096);
            int f2 = open("/dev/urandom", O_RDONLY);
            if (f2 >= 0) {
                errno = 0;
                ssize_t r = read(f2, q, 4096);
                CHECK3(r < 0, "getrandom", "/dev/urandom unmapped VA <0");
                close(f2);
            }
        }
    }

    // 7. Concurrency: fork, child loops 1000×getrandom(32B), parent draws
    //    its own sample; both must not hang and must differ (no cross-process
    //    keystream repeat).  Note: same-mm munmap race is NOT expressible in
    //    current userland (clone→fork, no CLONE_VM); mm->lock correctness is
    //    covered by the mmap/mprotect/fork-mmap/COW suites.
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            int pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                uint8_t c[32];
                for (int i = 0; i < 1000; i++) getrandom(c, 32, 0);
                getrandom(c, 32, 0);   // fresh sample for comparison
                write(pipefd[1], c, 32);
                close(pipefd[1]);
                _exit(0);
            }
            close(pipefd[1]);
            uint8_t pa[32], ca[32];
            getrandom(pa, 32, 0);
            ssize_t got = read(pipefd[0], ca, 32);
            close(pipefd[0]);
            int st;
            waitpid(pid, &st, 0);
            CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0, "getrandom", "fork child 1000 iters");
            CHECK3(got == 32, "getrandom", "pipe received child sample");
            CHECK3(memcmp(pa, ca, 32) != 0, "getrandom", "parent/child first differ");
        }
    }

    // 8. Monobit smoke test (32 KiB, no statistical power — catches only
    //    constant/all-zero/strongly-periodic disasters, not weak RNGs).
    //    Integer math only: OS01's printf has no %f and no %z modifier.
    {
        size_t big = 32768;
        uint8_t *buf = (uint8_t *)malloc(big);
        CHECK3(buf != NULL, "getrandom", "32KiB malloc");
        if (buf) {
            getrandom(buf, big, 0);
            long ones = 0;
            for (size_t i = 0; i < big; i++) {
                uint8_t v = buf[i];
                for (int k = 0; k < 8; k++) ones += (v >> k) & 1;
            }
            long total = (long)big * 8;
            long pct = (ones * 100) / total;   // integer % of 1-bits
            CHECKF(pct > 45 && pct < 55, "getrandom",
                   "monobit 1-bit%% in (45,55)", "monobit 1-bit%% = %ld", pct);
            free(buf);
        }
    }
}
```

- [ ] **Step 2: Register the test**

Add to the `tests[]` array in `systest.c` (after `{"termios", test_termios},` at line 1594):

```c
    {"termios",             test_termios},
    {"getrandom",           test_getrandom},
};
```

- [ ] **Step 3: Build and run the full suite**

Run: `make OS01_SYSTEST=1 disk.img` then `python3 tests/run_test.py systest` (or the equivalent `make test-syscall`).
Expected: all tests pass, `0 failed`; the new `getrandom` entries appear with `[PASS]`. The count is now 132 + the new assertions (the exact total is whatever the summary prints — only "0 failed" matters).

- [ ] **Step 4: Manual Linux-ABI verification (one-off, not in systest)**

`linux_to_os01[318]` only fires for `PF_LINUX_ABI` processes; systest is a native process, so 318 isn't asserted there. If busybox is available, boot the normal init and run a program that calls `getrandom` (busybox/musl use Linux syscall 318); otherwise verify by a temporary `PF_LINUX_ABI` process calling raw syscall 318 and printing the return. Confirm it returns the requested length.

- [ ] **Step 5: Commit**

```bash
git add user/systest.c
git commit -m "test(systest): getrandom + /dev/urandom coverage (basic/flags/EFAULT/large/fork/monobit)"
```

---

### Task 8: Documentation

**Files:**
- Modify: `docs/syscall.md` (header count line 3, syscall table, dispatch notes)

**Interfaces:** none (docs only).

- [ ] **Step 1: Update the syscall count and table**

In `docs/syscall.md`:
- Line 3: change "66 syscalls total (0..65)" → "67 syscalls total (0..66)".
- Add a table row after the `SYS_clock_gettime` row:

```
| 66 | `SYS_getrandom` | rdi=buf, rsi=len, rdx=flags | 内核 ChaCha20 池；GRND_NONBLOCK/GRND_RANDOM 为语义 NOP；len>33554431 截断；未映射/只读 buffer → -EFAULT |
```

- In the kernel-dispatch section, add a note that `linux_to_os01` is now `[320]` (not `[256]`) and maps Linux `getrandom` (318) → `SYS_getrandom` (66).

- [ ] **Step 2: Commit**

```bash
git add docs/syscall.md
git commit -m "docs(syscall): document getrandom (66) + linux_to_os01[318] translation"
```

---

## Self-Review

**Spec coverage:** §3 ChaCha20 in libc → Task 1; §4 kernel PRNG (interface/ready guard/arch dispatch/seed+reseed/counter wraparound/chunked lock/init timing) → Task 2; §5.1 mm->lock + per-page PTE + recursive split + lock set + COW/2MB notes → Task 3; §5 syscall wiring (uapi×2, `[320]`, `[318]=66`, names, case) → Task 4; §5.2 + §6 devfs → Task 5; §7 libc interface → Task 6; §8 tests (all 9 items) → Task 7; §9 docs → Task 8. The three v5 implementation-time notes are carried verbatim: **M-a** (truncate writes back `len` before `begin`/`get_random_bytes`) → Task 4 Step 4; **M-b** (devfs same 32 MiB cap) → Task 5 Step 1; **L-a** (fork memcpy resets child lock) → Task 3 Step 5.

**Placeholder scan:** No TBD/TODO/"similar to Task N"/"add error handling" — every code step contains the full source. The one soft spot is Task 4's ordering note (GRND flags come from Task 6's header); this is an explicit, resolvable instruction, not a placeholder.

**Type consistency:** `chacha20_block(const uint8_t[32], uint32_t, const uint8_t[12], uint8_t[64])` matches across Tasks 1 and 2. `get_random_bytes(void*, size_t)` + `RANDOM_MAX_LEN 33554431UL` match across Tasks 2/4/5. `user_write_range_begin(uint64_t, size_t)` / `user_write_range_end(void)` match across Tasks 3/4/5. `mm_alloc()` matches across Task 3's vma.c/vma.h/task.c. `SYS_getrandom`/`GRND_*`/`getrandom()` match across Tasks 4/6/7. `mm_t` lock init is consistently `{ .lock = { .lock = 1L } }` (definition) / `spin_init(&mm->lock)` (runtime).

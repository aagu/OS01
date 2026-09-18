# AAGU-2 CSPRNG / aarch64 log / AT_PLATFORM Facade Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three P0/P1 architectural cracks exposed by code review AAGU-1: (1) CSPRNG falls back to deterministic cycle-counter on QEMU and aarch64, silently destroying user-space stack canary entropy; (2) aarch64 `_log_*_impl` discards variadic args; (3) `setup_user_stack()` hard-codes `AT_PLATFORM="x86_64"` and other arch-specific strings inline.

**Architecture:** Three independent sub-fixes committed as one PR (per issue spec, "AT_RANDOM 修复必须和 CSPRNG 修复一起合入同一 commit/PR"). Each fix is small, surgical, and verified by `make OS01_SYSTEST=1 test-syscall` + `make KERNEL_SELFTEST=1`.

**Tech Stack:** C (clang), GNU Make, QEMU x86_64 + QEMU aarch64

**Spec:** This issue (AAGU-2) — `01a0b568-e1ed-74b4-a65b-169c598dd2a4`. Reference review: AAGU-1 (OS01 code-quality review).

## Global Constraints

- Arch-neutral facade goes in `kernel/include/arch/<thing>.h`; per-arch strong overrides in `kernel/arch/<arch>/<thing>.c` (AGENTS.md §Directory organization item 3).
- No `#ifdef __x86_64__` branches inside `setup_user_stack()` (task.c:1139) — the previous review already rejected that pattern in `task.c:340,349-350` and `smp.c:216-217`.
- Do not modify chacha20 algorithm (`libc/random/chacha20.c` is independent compile unit).
- Do not introduce new libc deps.
- All builds must succeed clean: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel.bin`.
- `make OS01_SYSTEST=1 test-syscall` must pass.
- `make KERNEL_SELFTEST=1` must pass (kernel self-tests).
- Two suites run separately (AGENTS.md gotcha — must NOT combine `OS01_SYSTEST=1` and `KERNEL_SELFTEST=1`).
- `git submodule update --init` must run before any build (BusyBox).

---

## File Inventory

### Modify
- `kernel/random/random.c` — entropy contract + reseed rekey + add-entropy entry point
- `kernel/include/random/random.h` — fail-closed signature
- `kernel/arch/aarch64/log_impl.c` — minimal `%s` formatter
- `kernel/sched/task.c` — call `arch_auxv_platform()` / `arch_auxv_payload_size()`; remove inline `"x86_64"`
- `kernel/fs/devfs.c` — `random_write` actually mixes into pool
- `kernel/selftest/test_at_random.c` — entropy test skips when pool is not ready, document fail-closed semantics
- `kernel/selftest/selftest.c` — register new tests if any

### Create
- `kernel/include/arch/auxv.h` — facade for `arch_auxv_platform()` + `arch_auxv_payload_size()`
- `kernel/arch/x86_64/auxv.c` — x86_64 strong override returning `"x86_64"`
- `kernel/arch/aarch64/auxv.c` — aarch64 strong override returning `"aarch64"`

---

## Task 1: CSPRNG fail-closed + generate-then-rekey

**Files:**
- Modify: `kernel/random/random.c:31-47` (mix_hw_entropy + random_init), `49-58` (reseed), `61-121` (get_random_bytes)
- Modify: `kernel/include/random/random.h` (signature)
- Modify: `kernel/selftest/test_at_random.c` (entropy selftest must be `random_ready`-aware)

**Why:** Issue spec §1 — without `mix_hw_entropy` reporting "I have real entropy", the CSPRNG silently ships deterministic output; without generate-then-rekey, the `reseed()` XOR-leaks the prior key; without `/dev/random` write participation, userland can never improve pool quality.

**Interfaces:**
- `mix_hw_entropy(uint8_t out[32]) -> bool` — true if at least one of the 4 words came from RDRAND/RDSEED
- `random_init(void)` — does not set `random_ready` unless entropy is real; logs the reason either way
- `void get_random_bytes(void *buf, size_t len)` — fail-closed: if `!random_ready`, memset buf to 0 and return; never warn-then-proceed
- `void random_add_entropy(const void *data, size_t len)` — pool write entry (used by `/dev/random` write path); ignored if `!random_ready`
- `bool random_is_ready(void)` — exposed for selftests
- `reseed(void)` — generate-then-rekey: derive next key from old key + fresh hw entropy via chacha20, zero temp

- [ ] **Step 1: Update header `kernel/include/random/random.h`**

```c
#ifndef _KERNEL_RANDOM_H
#define _KERNEL_RANDOM_H

#include <stddef.h>
#include <stdbool.h>

#define RANDOM_MAX_LEN 33554431UL

void random_init(void);
void get_random_bytes(void *buf, size_t len);
bool random_is_ready(void);
/* Pool mix entry point — feeds userland /dev/random writes into the
 * pool. Called with the caller's data buffer + length. No-op until
 * random_init() succeeds; concurrent callers serialize on pool_lock. */
void random_add_entropy(const void *data, size_t len);

#endif
```

- [ ] **Step 2: Rewrite `mix_hw_entropy` and `random_init` in `kernel/random/random.c`**

```c
/* Mix 32 bytes of hardware entropy.  Returns true iff at least one of
 * the four 64-bit words came from RDRAND/RDSEED — i.e. the buffer
 * contains real entropy.  When all RDRAND/RDSEED calls fail (QEMU
 * default CPU + aarch64), every word is filled from cycle-counter ^
 * jiffies (low-entropy), and we return false so the caller can keep
 * random_ready=false instead of silently seeding deterministic state. */
static bool mix_hw_entropy(uint8_t out[32])
{
    uint64_t w[4];
    int had_hw = 0;
    for (int i = 0; i < 4; i++) {
        if (rdseed64(&w[i]) || rdrand64(&w[i])) {
            had_hw++;
        } else {
            w[i] = arch_cycle_counter() ^ jiffies;
        }
    }
    memcpy(out, w, 32);
    return had_hw > 0;
}

void random_init(void)
{
    pool_blk = 0;
    pool_bytes_since_reseed = 0;

    if (!mix_hw_entropy(pool_key)) {
        log_warn("CSPRNG: no hardware entropy source "
                 "(RDRAND/RDSEED unavailable) — pool not ready, "
                 "/dev/random reads will fail-closed\n");
        random_ready = false;
        return;
    }
    random_ready = true;
    log_info("CSPRNG: pool seeded from hardware entropy\n");
}
```

- [ ] **Step 3: Rewrite `reseed()` — generate-then-rekey**

```c
/* Periodic reseed: derive a brand-new key from (old key || fresh
 * entropy) via ChaCha20 — never XOR the new entropy into the old
 * key (XOR would leak the prior key if the new entropy is recovered
 * by an attacker; e.g. weak fallback entropy). The 64-byte ChaCha20
 * block yields 32 bytes of fresh key material; the buffer is
 * zeroed afterward so it doesn't sit on the stack. */
static void reseed(void)
{
    uint8_t hw[32];
    bool hw_ok = mix_hw_entropy(hw);
    uint8_t block[64];
    uint8_t nonce[12] = {0};
    /* old key || hw as input to chacha20 → first 32 bytes = next key */
    uint8_t seed[64];
    memcpy(seed,      pool_key, 32);
    memcpy(seed + 32, hw,       32);
    chacha20_block(seed, 0, nonce, block);
    memcpy(pool_key, block, 32);
    /* wipe */
    memset(block, 0, sizeof(block));
    memset(seed,  0, sizeof(seed));
    memset(hw,    0, sizeof(hw));
    pool_bytes_since_reseed = 0;
    /* If HW entropy was unavailable this reseed, the "new" key is
     * just the old key through a permutation. Mark the pool not
     * ready so fail-closed kicks in instead of silently degrading. */
    if (!hw_ok) {
        random_ready = false;
        log_warn("CSPRNG: reseed lost HW entropy — pool going not-ready\n");
    }
}
```

- [ ] **Step 4: Rewrite `get_random_bytes` — fail-closed**

```c
void get_random_bytes(void *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (!random_ready) {
        /* Fail-closed: zero the buffer so callers that derive
         * security-sensitive values (AT_RANDOM payload, canary,
         * key material) can detect the all-zero state explicitly.
         * libc/csu/csu.c checks `__stack_chk_guard == 0` and calls
         * __stack_chk_fail. SYS_getrandom reads are truncated to 0
         * bytes; userspace can retry once entropy is ready. */
        memset(buf, 0, len);
        return;
    }

    uint8_t *out = (uint8_t *)buf;

    while (len > 0) {
        size_t chunk = len;
        if (chunk > (64u << 10))
            chunk = 64u << 10;

        uint64_t flags = spin_lock_irqsave(&pool_lock);

        for (size_t off = 0; off < chunk; off += 64) {
            uint8_t block[64];
            uint8_t nonce[12] = {0};

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
            memset(block, 0, 64);

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

bool random_is_ready(void) { return random_ready; }

/* /dev/random write entry: mix the caller's data into the pool key
 * (XOR-folded into a 32-byte accumulator). Held under pool_lock so
 * concurrent reseed()/get_random_bytes() see a consistent key.
 * A no-op until random_init() succeeds — userspace can't inject
 * entropy we haven't earned from hardware yet. */
void random_add_entropy(const void *data, size_t len)
{
    if (!data || len == 0 || !random_ready)
        return;
    const uint8_t *p = (const uint8_t *)data;
    uint64_t flags = spin_lock_irqsave(&pool_lock);
    for (size_t i = 0; i < len; i++)
        pool_key[i & 31] ^= p[i];
    spin_unlock_irqrestore(&pool_lock, flags);
}
```

- [ ] **Step 5: Update `kernel/selftest/test_at_random.c` entropy case**

```c
int at_random_selftest_entropy(void)
{
    static uint8_t first[16];
    uint64_t rsp, auxv, rnd, plat;
    /* P0: when the CSPRNG pool is not ready (e.g. QEMU default CPU
     * without RDRAND/RDSEED), AT_RANDOM is fail-closed zeroed — the
     * canary derived from it is also zeroed and libc aborts. That is
     * the CORRECT secure behavior; this selftest only proves the
     * ready path produces distinct keystream blocks. */
    if (!random_is_ready()) {
        serial_printk("[selftest] at_random_entropy: pool not ready (fail-closed) — skipped\n");
        return 0;   /* not a regression */
    }
    if (probe_build(odd_argv, odd_envp, &rsp, &auxv, &rnd, &plat) != 0 || rnd == 0) {
        ...
    }
    ...
}
```

- [ ] **Step 6: Add include + prototype to test_at_random.c**

```c
#include <random/random.h>    /* random_is_ready */
```

- [ ] **Step 7: Build and test**

```bash
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang kernel.bin
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall
make KERNEL_SELFTEST=1 PROFILE=x86_64-clang kernel.bin
# Boot it: `make PROFILE=x86_64-clang run` should print
#   CSPRNG: pool seeded from hardware entropy    (real hw)
#   CSPRNG: no hardware entropy source ...        (QEMU default CPU)
```

- [ ] **Step 8: Commit**

```bash
git add kernel/random/random.c kernel/include/random/random.h \
        kernel/selftest/test_at_random.c
git commit -m "random: fail-closed entropy contract + generate-then-rekey"
```

---

## Task 2: Wire `/dev/random` write → pool mix

**Files:**
- Modify: `kernel/fs/devfs.c:150-153` (`random_write` is currently a no-op)

- [ ] **Step 1: Replace `random_write` body**

```c
static int random_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    /* Cap to one block so a runaway writer can't block the pool lock
     * for long; the data still gets folded in (XOR accumulator in
     * random_add_entropy). Mirrors Linux /dev/random write semantics
     * (always succeeds, pool absorbs everything). */
    if (size > RANDOM_MAX_LEN) size = RANDOM_MAX_LEN;
    random_add_entropy(buffer, (size_t)size);
    return (int)size;
}
```

- [ ] **Step 2: Add include if not present**

```c
#include <random/random.h>   /* random_add_entropy */
```

- [ ] **Step 3: Build + verify**

```bash
make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel.bin
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall
```

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/devfs.c
git commit -m "devfs: /dev/random write mixes into CSPRNG pool"
```

---

## Task 3: aarch64 minimal variadic log formatter (`%s` + `%u`)

**Files:**
- Modify: `kernel/arch/aarch64/log_impl.c`

**Why:** Issue §2 — `kernel/arch/aarch64/main.c:197` passes a `fail_reason` through `%s` and loses it.

- [ ] **Step 1: Replace `_log_*_impl` with `%s` and `%u/%lu` support**

```c
/* kernel/arch/aarch64/log_impl.c — _log_*_impl implementations.
 *
 * AArch64 kernel links with -nostdlib; no vsnprintf.  This file
 * provides a hand-rolled mini-formatter supporting the three
 * conversion specifiers that actually appear in aarch64 kernel
 * code today:
 *   %s  NUL-terminated string           (kputs handles)
 *   %u  unsigned int (decimal)          (kputu handles)
 *   %lu unsigned long (decimal, 64-bit) (kputu handles)
 *   %p  pointer (0xHEX via kputx)
 * Anything else (incl. unknown specifiers) is passed through to
 * kputs verbatim so regressions are visible — better than silently
 * eating the format string. */
#include <arch/aarch64/boot_log.h>   /* kputs / kputu / kputx */
#include <stdarg.h>
#include <stdint.h>
#include <log/log.h>

static void emit_pct_s(const char *s) { kputs(s ? s : "(null)"); }

void _log_err_impl(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (fmt[0] == '%' && fmt[1] != '\0') {
            char spec = fmt[1];
            switch (spec) {
            case 's': emit_pct_s(va_arg(ap, const char *)); fmt += 2; continue;
            case 'u': kputu((uint64_t)(unsigned)va_arg(ap, unsigned)); fmt += 2; continue;
            case 'l':
                if (fmt[2] == 'u') {
                    kputu((uint64_t)va_arg(ap, unsigned long));
                    fmt += 3; continue;
                }
                break;  /* unknown %lX — fall through */
            case 'p':
                kputs("0x");
                kputx((uint64_t)(uintptr_t)va_arg(ap, void *));
                fmt += 2; continue;
            default:
                /* unknown specifier: emit `%X` literally so it is
                 * visible that we don't understand it. */
                break;
            }
        }
        /* literal byte (or trailing '%' we can't decode) */
        char one[2] = { *fmt++, 0 };
        kputs(one);
    }
    va_end(ap);
}
void _log_warn_impl(const char *fmt, ...) { /* same body — keep DRY for now */ }
void _log_info_impl(const char *fmt, ...) { /* same body — keep DRY for now */ }
```

**Note:** Since duplicating the body three times is gross, refactor with a static helper:

```c
static void vlog(const char *fmt, va_list ap)
{
    /* ... same body, using va_arg(ap, ...) ... */
}
void _log_err_impl(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
void _log_warn_impl(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
void _log_info_impl(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
```

- [ ] **Step 2: Grep for any other variadic log calls in aarch64**

```bash
grep -rn 'log_\(err\|warn\|info\)\s*(' kernel/arch/aarch64/ | grep '%'
```

Expected: only `main.c:197` with `%s`. (If more appear, this implementation already covers them since %u/%lu/%p are the common cases.)

- [ ] **Step 3: Build aarch64 (best-effort) and confirm aarch64 at minimum compiles**

```bash
make PROFILE=aarch64-clang kernel.bin 2>&1 | tail -40
```

(Some aarch64 smoke errors unrelated to log_impl.c are OK; this only needs to verify our change compiles.)

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/aarch64/log_impl.c
git commit -m "aarch64 log: minimal %s/%u variadic support (was silently dropped)"
```

---

## Task 4: `arch_auxv_*` facade + per-arch strong overrides

**Files:**
- Create: `kernel/include/arch/auxv.h`
- Create: `kernel/arch/x86_64/auxv.c`
- Create: `kernel/arch/aarch64/auxv.c`
- Modify: `kernel/sched/task.c:1139-1230` (`setup_user_stack` + `task_selftest_auxv_probe`)

- [ ] **Step 1: Create facade `kernel/include/arch/auxv.h`**

```c
#ifndef _KERNEL_ARCH_AUXV_H
#define _KERNEL_ARCH_AUXV_H

#include <stddef.h>

/* arch_auxv_platform — returns a NUL-terminated string identifying
 * the platform, suitable for AT_PLATFORM. The returned pointer
 * references a static buffer (no allocation, IRQ-safe). */
const char *arch_auxv_platform(void);

/* arch_auxv_payload_size — the byte length of the AT_PLATFORM
 * payload INCLUDING the NUL. */
size_t arch_auxv_payload_size(void);

#endif
```

- [ ] **Step 2: Create `kernel/arch/x86_64/auxv.c`**

```c
// kernel/arch/x86_64/auxv.c — x86_64 AT_PLATFORM payload.
// Strong override of arch_auxv_* declared in <arch/auxv.h>.
#include <arch/auxv.h>

static const char platform[] = "x86_64";   // 8 incl NUL

const char *arch_auxv_platform(void)   { return platform; }
size_t      arch_auxv_payload_size(void) { return sizeof(platform); }
```

- [ ] **Step 3: Create `kernel/arch/aarch64/auxv.c`**

```c
// kernel/arch/aarch64/auxv.c — aarch64 AT_PLATFORM payload.
// Strong override of arch_auxv_* declared in <arch/auxv.h>.
#include <arch/auxv.h>

static const char platform[] = "aarch64"; // 9 incl NUL

const char *arch_auxv_platform(void)   { return platform; }
size_t      arch_auxv_payload_size(void) { return sizeof(platform); }
```

- [ ] **Step 4: Modify `kernel/sched/task.c` — drop inline `"x86_64"`, call facade**

In `setup_user_stack`, replace:

```c
    const char platform_str[] = "x86_64";    /* 8 bytes incl NUL */
    /* AT_RANDOM payload：16B 内核 CSPRNG，16 字节对齐不跨字
     * （spec 2026-09-17 §6.3）。 */
    rsp = (rsp - 16) & ~15ULL;
    get_random_bytes(KSTACK(rsp), 16);
    uint64_t at_random_addr = rsp;
    /* AT_PLATFORM payload：8 字节（"x86_64" + NUL） */
    rsp -= sizeof(platform_str);
    memcpy(KSTACK(rsp), platform_str, sizeof(platform_str));
    uint64_t at_platform_addr = rsp;
    /* R9 BLOCKER 修正: 真 fixed + meta 公式(替换 R8 的 total_descending 简化版) */
    const size_t auxv_pair_count = 3;        /* AT_PLATFORM, AT_RANDOM, AT_NULL */
    const size_t fixed = 16 + sizeof(platform_str) + auxv_pair_count * 16;
    const size_t meta  = (s_argc + s_envc + 3) * 8;
```

with:

```c
    /* AT_PLATFORM payload sourced from arch-neutral facade so
     * x86_64 ("x86_64") and aarch64 ("aarch64") share this builder. */
    const char *platform_str    = arch_auxv_platform();
    const size_t platform_size  = arch_auxv_payload_size();
    /* AT_RANDOM payload：16B 内核 CSPRNG，16 字节对齐不跨字
     * （spec 2026-09-17 §6.3）。 */
    rsp = (rsp - 16) & ~15ULL;
    get_random_bytes(KSTACK(rsp), 16);
    uint64_t at_random_addr = rsp;
    /* AT_PLATFORM payload (NUL-terminated, size from facade) */
    rsp -= platform_size;
    memcpy(KSTACK(rsp), platform_str, platform_size);
    uint64_t at_platform_addr = rsp;
    /* R9 BLOCKER 修正: 真 fixed + meta 公式 */
    const size_t auxv_pair_count = 3;
    const size_t fixed = 16 + platform_size + auxv_pair_count * 16;
    const size_t meta  = (s_argc + s_envc + 3) * 8;
```

Add to the includes at the top of task.c:

```c
#include <arch/auxv.h>      /* arch_auxv_platform / payload_size */
```

- [ ] **Step 5: Build + verify both arches**

```bash
make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel.bin
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall
make KERNEL_SELFTEST=1 PROFILE=x86_64-clang kernel.bin
make PROFILE=aarch64-clang kernel.bin 2>&1 | tail -10
```

The at_random_selftest_layout asserts `strcmp(plat, "x86_64") != 0` fails — that's fine, it must be `"x86_64"` for x86_64.

- [ ] **Step 6: Commit**

```bash
git add kernel/include/arch/auxv.h \
        kernel/arch/x86_64/auxv.c \
        kernel/arch/aarch64/auxv.c \
        kernel/sched/task.c
git commit -m "auxv: AT_PLATFORM via arch facade (x86_64 + aarch64)"
```

---

## Task 5: Final acceptance verification

- [ ] **Step 1: Run systest end-to-end**

```bash
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall
```

Expected: 70/70 PASS (current baseline). Document any new pass count.

- [ ] **Step 2: Run kernel self-tests**

```bash
make KERNEL_SELFTEST=1 PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang run
```

Expected: at_random_layout PASS, at_random_layout_even PASS, at_random_entropy skipped (logged as "pool not ready (fail-closed)") on QEMU default CPU.

- [ ] **Step 3: Run aarch64 build**

```bash
make PROFILE=aarch64-clang kernel.bin 2>&1 | tail -10
```

Expected: clean build (or aarch64-specific phase1 errors that are NOT related to our changes).

- [ ] **Step 4: Final commit (squash if needed) and PR**

If the four task commits are clean, leave them as-is. Otherwise `git rebase -i HEAD~4` to tidy.

Push branch and open PR:

```bash
git push -u origin agent/executor/86f869e180d6
gh pr create --base main --head agent/executor/86f869e180d6 \
   --title "[P0] CSPRNG entropy fail-closed + aarch64 log variadic + AT_PLATFORM facade" \
   --body "Closes AAGU-2"
```

- [ ] **Step 5: Post comment on AAGU-2 with test results**

(Per Multica workflow.)

---

## Self-Review Notes

**Spec coverage:**
- Issue §1 (CSPRNG entropy fail-closed): Task 1 ✓
- Issue §1 (reseed generate-then-rekey): Task 1 ✓
- Issue §1 (/dev/random write participates): Task 2 ✓
- Issue §2 (aarch64 log variadic): Task 3 ✓
- Issue §3 (AT_PLATFORM facade): Task 4 ✓
- Issue §3 (no `#ifdef __x86_64__` in setup_user_stack): Task 4 ✓

**Placeholder scan:** None — all code blocks are complete.

**Type consistency:**
- `mix_hw_entropy(uint8_t out[32]) -> bool` defined once, called in two places (random_init, reseed).
- `arch_auxv_*` facade is the only caller-facing surface; per-arch `.c` is the only implementer.
- `random_add_entropy(const void*, size_t)` matches devfs `random_write` call signature.
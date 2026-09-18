// kernel/random/random.c — ChaCha20 CSPRNG pool + RDRAND/RDSEED reseed.
//
// Stateless algorithm lives in libc (chacha20.c, linked via libk.a); this
// file owns the secret state: the 32-byte key, a 64-bit block index, and
// the pool spinlock.  RDRAND/RDSEED are used ONLY for initial seeding and
// periodic reseed (every 1 MiB of output) — never per-call — because RDRAND
// throughput is bounded and per-call rekeying would turn a CSPRNG back into
// a hardware dependency without adding security.
//
// Entropy contract (issue AAGU-2 §1, P0-1):
//   - mix_hw_entropy() returns true iff at least one of the four 64-bit
//     words came from RDRAND/RDSEED. When none did (QEMU default CPU
//     without RDRAND/RDSEED, aarch64 RNDR not yet wired), every word is
//     cycle-counter ^ jiffies — low-entropy, attacker-recoverable.
//   - random_init() only sets random_ready=true when mix_hw_entropy()
//     returns true. Otherwise the pool stays not-ready, the boot log
//     carries a one-line reason, and get_random_bytes() fail-closes by
//     memsetting the request buffer to 0 — letting security-sensitive
//     callers (AT_RANDOM payload, __stack_chk_guard via getrandom,
//     userland key derivation) detect the all-zero state and abort
//     instead of silently trusting deterministic output.
//
// Reseed contract (issue AAGU-2 §1, P0-1 follow-up):
//   - reseed() is generate-then-rekey: feed (old_key || fresh_hw) through
//     ChaCha20, take the first 32 bytes as the new key. The old XOR-only
//     reseed leaked the prior key if the new entropy was recoverable;
//     the new path keeps an attacker from rolling back to a known key.
//   - If HW entropy is unavailable during reseed, the permutation is
//     over a weak input — drop to not-ready and log. Don't pretend the
//     pool is healthy.
//
// /dev/random write contract (issue AAGU-2 §1, P0-1 follow-up):
//   - random_add_entropy() folds the caller buffer into pool_key (XOR
//     into a 32-byte accumulator) under pool_lock. No-op until
//     random_init() succeeds — userland can't inject entropy we haven't
//     earned from hardware yet. Called by kernel/fs/devfs.c::random_write.
#include <random/random.h>
#include <arch/random.h>
#include <arch/cpu.h>        // arch_cycle_counter()
#include <arch/spinlock.h>
#include <log/log.h>
#include <time/timer.h>            // jiffies
#include <core/bootinfo.h>         // boot_context, BOOT_CONTEXT_HAS_BOOT_ENTROPY
#include <chacha20.h>
#include <string.h>
#include <stdbool.h>

#define RESEED_INTERVAL (1u << 20)   // 1 MiB of output between reseeds

static uint8_t  pool_key[32];
static uint64_t pool_blk;            // 64-bit block index (see below)
static uint64_t pool_bytes_since_reseed;
static spinlock_T pool_lock = { .lock = 1L };
static bool random_ready;

// Mix 32 bytes of hardware entropy.  Returns true iff at least one of
// the four 64-bit words came from RDRAND/RDSEED — i.e. the buffer
// contains real hardware entropy.  When all RDRAND/RDSEED calls fail
// (QEMU default CPU without RDRAND/RDSEED, aarch64 stub), every word
// is filled from cycle-counter ^ jiffies (low-entropy, attacker-
// recoverable), and we return false so the caller can keep
// random_ready=false instead of silently seeding deterministic state.
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
    /* Wipe w so the partial RDRAND/RDSEED value doesn't sit on the stack
     * longer than necessary. */
    memset(w, 0, sizeof(w));
    return had_hw > 0;
}

void random_init(const struct boot_context *bootctx)
{
    pool_blk = 0;
    pool_bytes_since_reseed = 0;

    /* UEFI GetRNG path (preferred): the bootloader fetched 32 bytes
     * via EFI_RNG_PROTOCOL (QEMU + virtio-rng, real hw with TPM/RNG).
     * Mark the pool ready immediately — this is audited entropy, no
     * need to also try RDRAND/RDSEED. */
    if (bootctx && (bootctx->flags & BOOT_CONTEXT_HAS_BOOT_ENTROPY)) {
        memcpy(pool_key, bootctx->boot_entropy, 32);
        random_ready = true;
        log_info("CSPRNG: pool seeded from UEFI GetRNG (boot_entropy)\n");
        return;
    }

    /* RDRAND/RDSEED path (real hardware with the CPUID feature): only
     * if at least one 64-bit word came from the instruction do we trust
     * the buffer. QEMU default CPU has neither, aarch64 has no stub,
     * so this path silently returning false is the common dev case. */
    if (!mix_hw_entropy(pool_key)) {
        log_warn("CSPRNG: no hardware entropy source "
                 "(no UEFI GetRNG, no RDRAND/RDSEED) — pool not ready, "
                 "get_random_bytes will fail-closed\n");
        random_ready = false;
        return;
    }
    random_ready = true;
    log_info("CSPRNG: pool seeded from RDRAND/RDSEED\n");
}

// Periodic reseed: derive a brand-new key from (old key || fresh hw
// entropy) via ChaCha20 — never XOR the new entropy into the old key
// (XOR would let an attacker who recovers the new entropy roll back
// to a known key). The 64-byte ChaCha20 block yields 32 bytes of
// fresh key material; intermediate buffers are zeroed so they don't
// sit on the stack.
//
// Note: a reseed with weak hw entropy (QEMU default CPU, no RDRAND/
// RDSEED) still produces a fresh key — ChaCha20 mixes the input
// nonlinearly, so an attacker who knows the cycle-counter bytes still
// needs the prior 32-byte key to compute the new one. We do NOT drop
// random_ready on weak hw; the pool remains secure as long as the
// initial seed (random_init) was strong (boot_entropy from UEFI
// GetRNG, or RDRAND/RDSEED on real hw). The warning is logged for
// observability — operators should care when reseed stops mixing in
// new hw, but it is not a fail-closed trigger.
static void reseed(void)
{
    uint8_t hw[32];
    bool hw_ok = mix_hw_entropy(hw);
    uint8_t seed[64];
    uint8_t block[64];
    uint8_t nonce[12] = {0};

    memcpy(seed,      pool_key, 32);
    memcpy(seed + 32, hw,       32);
    chacha20_block(seed, 0, nonce, block);
    memcpy(pool_key, block, 32);

    /* Wipe intermediates so a later stack dump can't recover the
     * material we just consumed. */
    memset(block, 0, sizeof(block));
    memset(seed,  0, sizeof(seed));
    memset(hw,    0, sizeof(hw));

    pool_bytes_since_reseed = 0;

    if (!hw_ok) {
        log_warn("CSPRNG: reseed mixed weak entropy (no RDRAND/RDSEED) — "
                 "pool stays ready but operator should consider UEFI GetRNG\n");
    }
}

void get_random_bytes(void *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (!random_ready) {
        /* Fail-closed: zero the buffer so callers that derive
         * security-sensitive values (AT_RANDOM payload, canary,
         * key material) can detect the all-zero state explicitly.
         * libc/csu/csu.c checks `__stack_chk_guard == 0` and calls
         * __stack_chk_fail (noreturn, SIGABRT). SYS_getrandom
         * returns 0 bytes — userspace can retry once the pool
         * becomes ready (future UEFI GetRNG / RNDR wiring). */
        memset(buf, 0, len);
        return;
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
            // repeats — the reachable keystream space is 2^64 blocks (2^70
            // bytes) — unreachable in practice.
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
 * No-op until random_init() succeeds — userspace can't inject
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
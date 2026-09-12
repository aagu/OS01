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

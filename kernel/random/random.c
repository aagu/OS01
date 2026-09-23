// kernel/random/random.c — ChaCha20 CSPRNG pool + arch-neutral facade 接入。
//
// 旧 mix_hw_entropy()（内联 asm + cycle-counter fallback）已删除（AAGU-5）。
// 新的熵获取走 kernel/include/arch/random.h 的 facade：
//   - arch_random_get_entropy(buf, &q) 一次性 32B + 质量标签
//   - q ∈ { NONE, WEAK, STRONG }  在 kernel/include/arch/random.h 定义（spec §2）
//
// random_init() 对 q 做显式三分支 switch（spec §5.1），不再用 facade bool
// 返回值决定 ready — q 才是控制流输入。
//
// 状态转移（AAGU-2 衔接对照见 docs/arch/entropy-source-facade.md §5.2）：
//   - UEFI GetRNG 早返回    → ready=true (STRONG, boot_entropy)
//   - facade q==STRONG      → ready=true
//   - facade q==WEAK        → ready=true （AAGU-5 父契约 §交付物 2 授权）
//   - facade q==NONE/false  → ready=false (fail-closed)
//
// AAGU-2 fail-closed 在通用 ready 决策上保持不变；AT_RANDOM 走
// arch_random_get_strong() 单独 STRONG-only（spec §6）— task.c 改动见 Task 5。
#include <random/random.h>
#include <arch/random.h>
#include <arch/spinlock.h>
#include <log/log.h>
#include <core/bootinfo.h>         // boot_context, BOOT_CONTEXT_HAS_BOOT_ENTROPY
#include <chacha20.h>
#include <string.h>
#include <stdbool.h>

#define RESEED_INTERVAL (1u << 20)   // 1 MiB of output between reseeds

static uint8_t  pool_key[32];
static uint64_t pool_blk;
static uint64_t pool_bytes_since_reseed;
static spinlock_T pool_lock = { .lock = 1L };
static bool random_ready;

/* v8: C 预处理器 stringify 宏（spec §3.1 32B 契约 / spec §5.1 ready 决策外的
 * 辅助宏）。双层结构是 C99 标准 — 内层 #x 不展开参数，外层强制先展开再 stringify。 */
#define OS01_STRINGIFY_(x) #x
#define OS01_STRINGIFY(x) OS01_STRINGIFY_(x)

void random_init(const struct boot_context *bootctx)
{
    pool_blk = 0;
    pool_bytes_since_reseed = 0;

    /* v8: variant log 在 random_init 入口处打印 — 用 C 预处理器 stringify 把
     * -DKERNEL_VARIANT_NAME=weak-selftest token 转成字符串字面量 "weak-selftest"，
     * 避免 Makefile/shell/clang 三层引号转义的脆弱性。OS01_STRINGIFY 是 C99
     * 标准 idiom（双层宏防 #x 不展开参数）。 */
#ifdef KERNEL_VARIANT_NAME
    log_info("CSPRNG: kernel build variant=%s\n", OS01_STRINGIFY(KERNEL_VARIANT_NAME));
#else
    log_info("CSPRNG: kernel build variant=default\n");
#endif

    /* UEFI GetRNG 路径（独立于 facade；AAGU-2 已落地）— spec §5.1。
     * 父契约 §交付物 2 标 STRONG；与 spec §2.1 UEFI 行对齐。 */
    if (bootctx && (bootctx->flags & BOOT_CONTEXT_HAS_BOOT_ENTROPY)) {
        memcpy(pool_key, bootctx->boot_entropy, 32);
        random_ready = true;
        log_info("CSPRNG: pool seeded STRONG from UEFI GetRNG (boot_entropy)\n");
        return;
    }

    /* 通用 facade 路径 — spec §5.1。必须对 q 显式三分支 switch。 */
    arch_entropy_source_t q = ARCH_ENTROPY_NONE;
    uint8_t buf[32];
    bool facade_ok = arch_random_get_entropy(buf, &q);

    if (facade_ok) {
        switch (q) {
        case ARCH_ENTROPY_STRONG:
            memcpy(pool_key, buf, 32);
            random_ready = true;
            log_info("CSPRNG: pool seeded STRONG (RDSEED/RNDRRS)\n");
            break;
        case ARCH_ENTROPY_WEAK:
            memcpy(pool_key, buf, 32);
            random_ready = true;   /* spec §5.2 AAGU-5 父契约 §交付物 2 授权 */
            log_warn("CSPRNG: pool seeded WEAK (RDRAND/RNDR DRBG); "
                     "AT_RANDOM will fail-closed via arch_random_get_strong\n");
            break;
        case ARCH_ENTROPY_NONE:
            /* facade return true 但 q==NONE 不应发生 — 防御性处理。 */
            memset(buf, 0, 32);
            random_ready = false;
            log_warn("CSPRNG: facade returned true with NONE quality\n");
            break;
        }
    } else {
        /* facade return false ⇒ q 已是 NONE，buf 已 memset 0。 */
        random_ready = false;
        log_warn("CSPRNG: no hardware entropy source "
                 "(no UEFI GetRNG, no STRONG/WEAK via facade); "
                 "pool not ready, get_random_bytes fail-closed\n");
    }
    memset(buf, 0, 32);   // wipe stack buffer
}

static void reseed(void)
{
    arch_entropy_source_t q = ARCH_ENTROPY_NONE;
    uint8_t hw[32];
    bool hw_ok = arch_random_get_entropy(hw, &q);

    if (!hw_ok || q == ARCH_ENTROPY_NONE) {
        /* NONE 重置 — 与 AAGU-2 差异（spec §7.3）：
         * 旧行为混 cycle-counter 字节（低熵但非零），保持 ready=true；
         * 新行为显式 NONE ⇒ 零新熵 ⇒ drop ready=false，
         * 让下一个 get_random_bytes 走 fail-closed。 */
        memset(pool_key, 0, 32);
        random_ready = false;
        log_err("CSPRNG: reseed got NONE; pool dropped to not-ready\n");
        pool_bytes_since_reseed = 0;
        return;
    }

    uint8_t seed[64];
    uint8_t block[64];
    uint8_t nonce[12] = {0};

    memcpy(seed,      pool_key, 32);
    memcpy(seed + 32, hw,       32);
    chacha20_block(seed, 0, nonce, block);
    memcpy(pool_key, block, 32);

    memset(block, 0, sizeof(block));
    memset(seed,  0, sizeof(seed));
    memset(hw,    0, sizeof(hw));

    pool_bytes_since_reseed = 0;

    if (q == ARCH_ENTROPY_WEAK) {
        log_warn("CSPRNG: reseed mixed WEAK entropy; pool stays ready\n");
    }
    /* STRONG reseed 不打日志（常规路径）。 */
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

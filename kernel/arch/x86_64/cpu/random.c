// kernel/arch/x86_64/random.c — x86_64 facade 实现
//
// STRONG / WEAK / NONE 定义在 kernel/include/arch/random.h 一处（spec §0.2.1）。
// 本文件只列"我能产出哪个 quality + 试/退顺序"，不重述定义（spec §4.1）。
//
// 试/退：RDSEED×4 → STRONG；RDRAND×4 → WEAK；失败 → NONE（memset 后返回）。
// 边界：第 1 步 4 次 RDSEED 1 次失败即**全部退到第 2 步**，
//       禁止"3 RDSEED + 1 RDRAND"拼凑 STRONG（spec §4.1）。
#include <arch/random.h>
#include <arch/cpuid.h>
#include <arch/x86_64/random.h>   // 当前空 header，未来可能再补 CPUID 缓存声明
#include <string.h>

#define RDRAND_RETRY_MAX 10   // Intel SDM Vol. 2 §RDSEED

/* 各指令一次取 64-bit；CF=1 表示成功；最多 RDRAND_RETRY_MAX 次重试。 */
static inline bool rdrand64_one(uint64_t *out)
{
    for (int i = 0; i < RDRAND_RETRY_MAX; i++) {
        uint64_t v;
        unsigned char cf;
        __asm__ __volatile__("rdrand %0; setc %1" : "=r"(v), "=qm"(cf) : : "cc");
        if (cf) { *out = v; return true; }
    }
    return false;
}

static inline bool rdseed64_one(uint64_t *out)
{
    for (int i = 0; i < RDRAND_RETRY_MAX; i++) {
        uint64_t v;
        unsigned char cf;
        __asm__ __volatile__("rdseed %0; setc %1" : "=r"(v), "=qm"(cf) : : "cc");
        if (cf) { *out = v; return true; }
    }
    return false;
}

/* 一次取 32B（4×64-bit）。返回 true iff 全部 4 次都成功。
 * 失败时 out 内容未定义；调用方契约不读。 */
static bool fill32_rdseed(uint8_t out[32])
{
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        if (!rdseed64_one(&w[i])) {
            memset(w, 0, sizeof(w));   // 不让 partial RDSEED 字节残留
            return false;
        }
    }
    memcpy(out, w, 32);
    memset(w, 0, sizeof(w));
    return true;
}

static bool fill32_rdrand(uint8_t out[32])
{
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        if (!rdrand64_one(&w[i])) {
            memset(w, 0, sizeof(w));
            return false;
        }
    }
    memcpy(out, w, 32);
    memset(w, 0, sizeof(w));
    return true;
}

bool arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality)
{
    if (!out || !quality)
        return false;

    /* CPUID 探测：L1.ECX[30]=RDRAND；L7.S0.EBX[18]=RDSEED。
     * 一次性查，缓存在 percpu cpu_features（与既有 regs.h 风格一致）。
     * 此处用局部 static 缓存第一次结果，避免重复 CPUID — 实施时若需
     * percpu，可由后续 plan 替换。 */
    static int cached = 0;
    static bool has_rdseed, has_rdrand;
    if (!cached) {
        uint32_t eax, ebx, ecx, edx;
        cpuid(1, &eax, &ebx, &ecx, &edx);
        has_rdrand = !!(ecx & CPUID_FEAT_ECX_RDRAND);
        cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        has_rdseed = !!(ebx & CPUID_FEAT_EBX_RDSEED);
        cached = 1;
    }

    /* 第 1 步：RDSEED → STRONG（spec §4.1）。4 次必须全成功 — 1 次失败
     * 立即退到第 2 步，禁止拼接。 */
    if (has_rdseed && fill32_rdseed(out)) {
        *quality = ARCH_ENTROPY_STRONG;
        return true;
    }

    /* 第 2 步：RDRAND → WEAK（spec §4.1）。4 次必须全成功。 */
    if (has_rdrand && fill32_rdrand(out)) {
        *quality = ARCH_ENTROPY_WEAK;
        return true;
    }

    /* 第 3 步：NONE — memset 后返回（spec §2.3 禁止 cycle-counter 伪熵）。 */
    memset(out, 0, 32);
    *quality = ARCH_ENTROPY_NONE;
    return false;
}

bool arch_random_get_strong(uint8_t out[32])
{
    arch_entropy_source_t q;
    if (!arch_random_get_entropy(out, &q))
        return false;
    return q == ARCH_ENTROPY_STRONG;
}

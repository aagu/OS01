// kernel/arch/aarch64/random.c — aarch64 facade 实现
//
// STRONG / WEAK / NONE 定义在 kernel/include/arch/random.h 一处（spec §0.2.1）。
// 本文件只列"我能产出哪个 quality + 试/退顺序"，不重述定义（spec §4.2）。
//
// 试/退：RNDRRS×4 → STRONG；RNDR×4 → WEAK；失败 → NONE（memset 后返回）。
// 边界：第 1 步 4 次 RNDRRS 1 次失败即**全部退到第 2 步**，
//       禁止"3 RNDRRS + 1 RNDR"拼凑 STRONG（spec §4.2）。
//
// aarch64 phase-2 没有 libc 链接（kernel/Makefile:103+ 关闭 -isystem
// 与 libc 链入），所以 memcpy 不可用；memset 由
// kernel/arch/aarch64/memset.c 提供。这里只调 byte copy 避免
// 引入新依赖；32B 复制对 RNDRRS/RNDR 系统寄存器延迟可忽略。
#include <arch/random.h>
#include <arch/aarch64/regs.h>   // ID_AA64ISAR0_EL1_RNDR_*
#include <string.h>              // memset (provided by kernel/arch/aarch64/memset.c)

#define RNDR_RETRY_MAX 10   // ARM ARM D17.1.5

/* RNDRRS / RNDR 系统寄存器编码（S3_3_C2_C4_0 / S3_3_C2_C4_1）。
 * NZCV 全 0 表示成功。 */
static inline bool rndr_one(uint64_t *out)
{
    for (int i = 0; i < RNDR_RETRY_MAX; i++) {
        uint64_t v;
        uint64_t flags;
        __asm__ __volatile__(
            "mrs %0, s3_3_c2_c4_1\n\t"   /* RNDR */
            "cset %w1, eq"              /* flags == 0 ⇒ success */
            : "=r"(v), "=r"(flags)
            :
            : "cc");
        if (flags) { *out = v; return true; }
    }
    return false;
}

static inline bool rndrrs_one(uint64_t *out)
{
    for (int i = 0; i < RNDR_RETRY_MAX; i++) {
        uint64_t v;
        uint64_t flags;
        __asm__ __volatile__(
            "mrs %0, s3_3_c2_c4_0\n\t"   /* RNDRRS */
            "cset %w1, eq"
            : "=r"(v), "=r"(flags)
            :
            : "cc");
        if (flags) { *out = v; return true; }
    }
    return false;
}

/* 自包含的 32B byte copy：aarch64 phase-2 不链 libc，无 memcpy。 */
static void copy32(uint8_t dst[32], const uint64_t src[4])
{
    for (int i = 0; i < 4; i++) {
        dst[i * 8 + 0] = (uint8_t)(src[i] >>  0);
        dst[i * 8 + 1] = (uint8_t)(src[i] >>  8);
        dst[i * 8 + 2] = (uint8_t)(src[i] >> 16);
        dst[i * 8 + 3] = (uint8_t)(src[i] >> 24);
        dst[i * 8 + 4] = (uint8_t)(src[i] >> 32);
        dst[i * 8 + 5] = (uint8_t)(src[i] >> 40);
        dst[i * 8 + 6] = (uint8_t)(src[i] >> 48);
        dst[i * 8 + 7] = (uint8_t)(src[i] >> 56);
    }
}

static bool fill32_rndrrs(uint8_t out[32])
{
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        if (!rndrrs_one(&w[i])) {
            memset(w, 0, sizeof(w));
            return false;
        }
    }
    copy32(out, w);
    memset(w, 0, sizeof(w));
    return true;
}

static bool fill32_rndr(uint8_t out[32])
{
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        if (!rndr_one(&w[i])) {
            memset(w, 0, sizeof(w));
            return false;
        }
    }
    copy32(out, w);
    memset(w, 0, sizeof(w));
    return true;
}

bool arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality)
{
    if (!out || !quality)
        return false;

    /* ID_AA64ISAR0_EL1.RNDR 探测。一次性查，缓存到 static。
     * 如需 per-CPU，后续 plan 替换为 percpu 字段。 */
    static int cached = 0;
    static bool has_rndr, has_rndrrs;
    if (!cached) {
        uint64_t isar0;
        __asm__ __volatile__("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
        uint64_t rndr_field = (isar0 & ID_AA64ISAR0_EL1_RNDR_MASK)
                              >> ID_AA64ISAR0_EL1_RNDR_SHIFT;
        has_rndr   = (rndr_field >= 1);
        has_rndrrs = (rndr_field == 1);   // 仅 0b0001 同时支持 RNDRRS
        cached = 1;
    }

#ifdef KERNEL_TEST_FORCE_NO_RNDRRS
    /* 测试专用：强制 has_rndrrs=false 以验证 WEAK 档契约。
     * 配合 QEMU `-cpu max`（有 RNDR）即可触发"仅 RNDR 成功"路径，
     * 不依赖 QEMU 是否存在仅-RNDR 无-RNDRRS 的 CPU model。
     *
     * 宏由 build system 通过 OS01_SUBMAKE_ALLOWED 白名单传入
     * （详见 plan Task 8 + mk/project.mk），不在 shell 端注入 CFLAGS
     * （受控 sub-make 不透传未授权 CFLAGS）。 */
    has_rndrrs = false;
#endif

    /* 第 1 步：RNDRRS → STRONG（spec §4.2）。4 次必须全成功。 */
    if (has_rndrrs && fill32_rndrrs(out)) {
        *quality = ARCH_ENTROPY_STRONG;
        return true;
    }

    /* 第 2 步：RNDR → WEAK（spec §4.2）。4 次必须全成功。 */
    if (has_rndr && fill32_rndr(out)) {
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

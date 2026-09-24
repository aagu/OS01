# AAGU-5 arch-neutral entropy facade — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 OS01 内核实现 `arch_random_get_entropy()` arch-neutral facade + x86_64 / aarch64 强 override，让 `quality` 标签真影响 `random_ready` 与 AT_RANDOM 决策（关闭 AAGU-5 交付物 1-3）。

**Architecture:** 三层结构 — (1) `kernel/include/arch/random.h` 集中定义 `arch_entropy_source_t` 与两个函数；(2) `kernel/arch/{x86_64,aarch64}/random.c` 各自试/退 RDSEED→RDRAND 或 RNDRRS→RNDR；(3) `kernel/random/random.c` 用 facade 替换 `mix_hw_entropy()` 并对 `q` 做三分支 switch；AT_RANDOM 调用方走单独的 `arch_random_get_strong()` helper。

**Tech Stack:** C99，clang `-std=c11` 默认；aarch64 inline asm 用 MSR/MRS 编码；x86_64 沿用现有 RDRAND/RDSEED inline asm 模式（已迁移出 header）。

**Spec:** `docs/arch/entropy-source-facade.md`（本计划实现目标；plan 不再裁决 STRONG/WEAK/NONE，引用 spec §2 即可）

---

## Global Constraints

1. **STRONG/WEAK/NONE 的定义集中且唯一** — 仅在 `kernel/include/arch/random.h` 一处定义（spec §2）；架构 `.c` 文件与调用方不重述
2. **x86_64 与 aarch64 实现各自独立评审** — 禁止把 RDRAND/RDSEED inline asm 路径机械复制到 aarch64（spec §0.2.2）
4. **禁止装饰性 facade** — `random_init()` 必须对 `q` 显式三分支 switch；`arch_random_get_strong()` 必须真拒绝 WEAK/NONE
5. **禁止重定义 AAGU-2 fail-closed** — 通用 ready 决策与既有 `mix_hw_entropy()` 行为逐档一致（spec §5.2）；AT_RANDOM STRONG-only 是新路径，不动通用 ready
6. **禁止 cycle-counter 伪熵** — NONE 路径 `memset(out, 0, 32)` 后返回；不掺 cycle-counter 字节（spec §2.3）
7. **aarch64 whitelist 必须显式列出** — `kernel/Makefile` aarch64 分支需加 `random/random.c`；x86_64 由 `$(wildcard random/*.c)` 自动收纳
8. **测试覆盖三档 × 两架构** — `test_entropy_quality.c` 在三种 QEMU 模式（NONE / WEAK / STRONG）下分别跑，每档断言 facade 返回值、`*quality` 标签、`out[32]` 内容（NONE 全零 / STRONG 非零）、`random_is_ready()` 状态；QEMU 编排由 `tests/scripts/qemu_entropy_modes.sh` 控制
9. **commit message 风格** — `feat(random):`（新功能）、`refactor(random):`（重构）、`docs(arch):`（文档）、`test(kernel):`（测试）

---

## File Structure

**新增（5 个文件）**：

| 文件 | 职责 |
|---|---|
| `kernel/include/arch/random.h`（**重写**） | facade：`arch_entropy_source_t` 枚举 + `arch_random_get_entropy()` + `arch_random_get_strong()` 声明；删除 aarch64 inline stub |
| `kernel/include/arch/x86_64/random.h`（**重写**） | 删除 inline `rdrand64`/`rdseed64`（已迁到 `.c`）；只保留 CPUID 宏依赖 |
| `kernel/arch/x86_64/random.c`（**新建**） | x86_64 facade 实现：RDSEED×4 → STRONG；RDRAND×4 → WEAK；失败 → NONE |
| `kernel/arch/aarch64/random.c`（**新建**） | aarch64 facade 实现：RNDRRS×4 → STRONG；RNDR×4 → WEAK；失败 → NONE |
| `kernel/selftest/test_entropy_quality.c`（**新建**） | selftest：facade 调用 + quality / out / random_ready 断言 |
| `tests/scripts/qemu_entropy_modes.sh`（**新建**） | QEMU 编排：3 模式 × 2 架构 = 6 次启动，校验 boot 日志 |

**修改（3 个文件）**：

| 文件 | 改动 |
|---|---|
| `kernel/random/random.c` | 删除 `mix_hw_entropy()` 内联 asm + cycle-counter fallback；`random_init()` 改为 facade + 三分支 switch；`reseed()` 改 facade 并新增 NONE→drop ready 路径 |
| `kernel/sched/task.c` | AT_RANDOM 16B 改用 `arch_random_get_strong()`；失败返回 `-1` 让 caller 决策 |
| `kernel/Makefile` | aarch64 whitelist 显式加 `random/random.c`（一行修改） |

**文档（2 个文件）**：

| 文件 | 改动 |
|---|---|
| `docs/arch/entropy-source-facade.md` | spec（v2，已写） |
| `docs/random/entropy-quality.md`（**新建**） | 运营商向摘要：quality 定义 + 各路径 caller 列表 + 真硬件/KVM 启用建议（spec §2 + §4.3 的简版） |

文件结构映射：1 个 facade header + 2 个架构 `.c` + 1 个 selftest + 1 个 QEMU 脚本 + 1 个 ops 文档 → 6 个新文件。3 个修改集中在 `kernel/random/`、`kernel/sched/`、`kernel/Makefile`。每 task 增 ≤ 2 个新文件或 1 个修改 — 满足 review-gate 粒度。

---

## Task 1: Add facade header `kernel/include/arch/random.h`

**Files:**
- Modify: `kernel/include/arch/random.h`（**完全重写**）

**Interfaces:**
- Produces:
  - `typedef enum { ARCH_ENTROPY_NONE, ARCH_ENTROPY_WEAK, ARCH_ENTROPY_STRONG } arch_entropy_source_t;`
  - `bool arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality);`
  - `bool arch_random_get_strong(uint8_t out[32]);`

- [ ] **Step 1: 重写 `kernel/include/arch/random.h`**

```c
#ifndef _ARCH_RANDOM_H
#define _ARCH_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

/* OS01 arch-neutral entropy facade. 语义定义见 docs/arch/entropy-source-facade.md §2。
 * 本 header 是 STRONG/WEAK/NONE 定义的唯一来源（spec §0.2.1）；架构 .c 与调用方不重述。 */
typedef enum {
    ARCH_ENTROPY_NONE  = 0,
    ARCH_ENTROPY_WEAK  = 1,
    ARCH_ENTROPY_STRONG = 2,
} arch_entropy_source_t;

/* 一次性产出 32B entropy + 质量标签（spec §3）。
 *
 *   out[32]   成功（*quality != NONE）时填满 32B；失败时 memset(out, 0, 32)。
 *   *quality  成功时设 STRONG/WEAK；失败时设 NONE。
 *   return    true iff *quality != NONE。
 *
 * IRQ 安全。Boot 前可调（架构实现各自测一次）。
 */
bool arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality);

/* AT_RANDOM 专用 helper：拒绝 WEAK/NONE（spec §6）。
 *
 *   out[32]   成功时填满 32B（STRONG）。
 *   return    true iff STRONG 已填入。
 *
 * 失败时 out 内容调用方契约不依赖 — caller 须把 out 当作不可用。
 */
bool arch_random_get_strong(uint8_t out[32]);

#endif // _ARCH_RANDOM_H
```

> **注**：此文件原含 aarch64 `static inline bool rdrand64/rdseed64 { return false; }` 占位 stub — 整体被本 facade 取代，stub 删除。架构 .c 文件接管实现。

- [ ] **Step 2: 删除 `kernel/include/arch/x86_64/random.h` 内的 inline `rdrand64`/`rdseed64` 实现**

重写为：

```c
#ifndef _KERNEL_ARCH_X86_64_RANDOM_H
#define _KERNEL_ARCH_X86_64_RANDOM_H

/* 旧 inline rdrand64 / rdseed64 实现已迁到 kernel/arch/x86_64/random.c
 * （AAGU-5.6 arch-neutral facade）。本 header 现在仅留作编译占位 —
 * 后续 plan 决定是否连 header 一并删除。 */

#endif // _KERNEL_ARCH_X86_64_RANDOM_H
```

- [ ] **Step 3: 编译 sanity check（x86_64）**

```bash
cd /home/aagu/multica_workspaces/aagu-63nr-00b367d31049/aagu-25-4aeb67a8cfea/workdir/OS01
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -40
```

期望：本步尚未实现 `arch_random_get_entropy()` 函数体，编译报 `undefined reference`。**这是预期失败** — Task 2 落地后即解。如非预期失败（如 syntax error），停下排查。

- [ ] **Step 4: 提交**

```bash
git add kernel/include/arch/random.h kernel/include/arch/x86_64/random.h
git commit -m "feat(random): arch-neutral facade header (spec §2-3)

- Add arch_entropy_source_t { NONE, WEAK, STRONG } enum and
  arch_random_get_entropy(uint8_t[32], quality) / arch_random_get_strong
  prototypes in kernel/include/arch/random.h
- Remove aarch64 inline rdrand64/rdseed64 stub (replaced by facade)
- Move x86_64 inline rdrand64/rdseed64 out of header (will be in
  kernel/arch/x86_64/random.c, ref AAGU-5 spec §0.2.1: STRONG/WEAK/NONE
  defined in one place)
- Quality classification reference: docs/arch/entropy-source-facade.md §2
- Spec compliance: §0.2.1 (centralized definition), §3 (facade contract)
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md Task 1"
```

---

## Task 2: Implement x86_64 strong override

**Files:**
- Create: `kernel/arch/x86_64/random.c`
- Modify: `kernel/include/arch/x86_64/random.h`（已 Task 1 重写为占位）

**Interfaces:**
- Consumes: `arch_entropy_source_t *` 输出形参；CPUID 特征宏（`CPUID_FEAT_ECX_RDRAND` / `CPUID_FEAT_EBX_RDSEED` 已在 `kernel/include/arch/cpuid.h`）
- Produces: 实现 `arch_random_get_entropy()` / `arch_random_get_strong()`

- [ ] **Step 1: 写 selftest 用失败用例（在 `kernel/selftest/test_entropy_quality.c` —— 待 Task 6 完成；但本 Task 先验证编译链路）**

略：本 task 不写 selftest，仅编译验证。完整 selftest 在 Task 6。

- [ ] **Step 2: 创建 `kernel/arch/x86_64/random.c`**

```c
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
```

- [ ] **Step 3: 编译 x86_64 — 期望链接错误（kernel/random/random.c 还在调旧的 mix_hw_entropy）**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -30
```

期望：`undefined reference to mix_hw_entropy` 或 `undefined reference to rdrand64/rdseed64`（取决于 kernel/random/random.c 是否已改）。两类预期失败都将在 Task 4 解决。如有其它错误停下排查。

- [ ] **Step 4: 提交**

```bash
git add kernel/arch/x86_64/random.c
git commit -m "feat(random): x86_64 strong override (RDSEED STRONG, RDRAND WEAK)

- kernel/arch/x86_64/random.c implements arch_random_get_entropy:
  try RDSEED×4 (raw entropy, STRONG) → fall through to RDRAND×4
  (DRBG output, WEAK) → fall through to memset(out, 0, 32) NONE
- 4-times-must-all-succeed boundary (spec §4.1): no partial RDSEED +
  RDRAND mixing to claim STRONG
- CPUID feature detection cached at first invocation (per-CPU cache
  upgrade deferred to follow-up plan)
- arch_random_get_strong helper filters STRONG only (spec §6)
- Spec compliance: §0.2.1, §0.2.2 (independent asm per arch), §0.2.3,
  §2.1 (RDSEED = STRONG), §2.2 (RDRAND = WEAK), §2.3 (NONE memset),
  §4.1 (x86_64 try/fall order)
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 2"
```

---

## Task 3: Implement aarch64 strong override

**Files:**
- Create: `kernel/arch/aarch64/random.c`
- Modify: `kernel/include/arch/aarch64/regs.h`（**新增** `ISAR0_RNDR_RNDRRS` 宏）

**Interfaces:**
- Consumes: `ID_AA64ISAR0_EL1.RNDR` 特征值（`kernel/include/arch/aarch64/regs.h` 需补宏）
- Produces: `arch_random_get_entropy()` / `arch_random_get_strong()` aarch64 实现

- [ ] **Step 1: 在 `kernel/include/arch/aarch64/regs.h` 补 `ID_AA64ISAR0_EL1.RNDR` 编码宏**

读 `kernel/include/arch/aarch64/regs.h`，定位合适的"system register encoding"区段，添加：

```c
/* ID_AA64ISAR0_EL1.RNDR 字段（bits[63:60]）：
 *   0b0000 = 不支持 RNDR/RNDRRS
 *   0b0001 = 支持 RNDR + RNDRRS
 *   0b0010 = 支持 RNDR（IMPL_DEF RNDRRS 处理）
 *   ≥0b0011 = 保留
 * 实施者用：((mrs ID_AA64ISAR0_EL1) >> 60) & 0xF
 */
#define ID_AA64ISAR0_EL1_RNDR_SHIFT  60
#define ID_AA64ISAR0_EL1_RNDR_MASK   (0xFUL << ID_AA64ISAR0_EL1_RNDR_SHIFT)
#define ID_AA64ISAR0_EL1_RNDR_RNDRRS (1UL << ID_AA64ISAR0_EL1_RNDR_SHIFT)
```

- [ ] **Step 2: 创建 `kernel/arch/aarch64/random.c`**

```c
// kernel/arch/aarch64/random.c — aarch64 facade 实现
//
// STRONG / WEAK / NONE 定义在 kernel/include/arch/random.h 一处（spec §0.2.1）。
// 本文件只列"我能产出哪个 quality + 试/退顺序"，不重述定义（spec §4.2）。
//
// 试/退：RNDRRS×4 → STRONG；RNDR×4 → WEAK；失败 → NONE（memset 后返回）。
// 边界：第 1 步 4 次 RNDRRS 1 次失败即**全部退到第 2 步**，
//       禁止"3 RNDRRS + 1 RNDR"拼凑 STRONG（spec §4.2）。
#include <arch/random.h>
#include <arch/aarch64/regs.h>   // ID_AA64ISAR0_EL1_RNDR_*
#include <string.h>

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

static bool fill32_rndrrs(uint8_t out[32])
{
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        if (!rndrrs_one(&w[i])) {
            memset(w, 0, sizeof(w));
            return false;
        }
    }
    memcpy(out, w, 32);
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
    memcpy(out, w, 32);
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
```

- [ ] **Step 3: 编译 aarch64（不依赖 kernel/random/random.c 已改）**

```bash
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -30
```

期望：`kernel/arch/aarch64/random.o` 编译成功；其它链接错误（kernel/random/random.c 还在用旧接口）属预期失败，Task 4 解决。如本步有 syntax/asm 错误，停下排查。

- [ ] **Step 4: 提交**

```bash
git add kernel/arch/aarch64/random.c kernel/include/arch/aarch64/regs.h
git commit -m "feat(random): aarch64 strong override (RNDRRS STRONG, RNDR WEAK)

- kernel/arch/aarch64/random.c implements arch_random_get_entropy:
  try RNDRRS×4 (raw entropy, STRONG) → fall through to RNDR×4
  (DRBG output, WEAK) → fall through to memset(out, 0, 32) NONE
- 4-times-must-all-succeed boundary (spec §4.2): no partial RNDRRS +
  RNDR mixing to claim STRONG
- ID_AA64ISAR0_EL1.RNDR feature detection (bits[63:60] = 0b0001 ⇒
  both RNDR and RNDRRS supported)
- Add ID_AA64ISAR0_EL1_RNDR_* macros to kernel/include/arch/aarch64/regs.h
- MSR/MRS encodings: s3_3_c2_c4_0 (RNDRRS), s3_3_c2_c4_1 (RNDR);
  NZCV==0 success check via cset; \"cc\" clobber
- Spec compliance: §0.2.1, §0.2.2 (independent asm per arch), §0.2.3,
  §2.1 (RNDRRS = STRONG), §2.2 (RNDR = WEAK per AAGU-5.3 decision),
  §2.3 (NONE memset), §4.2 (aarch64 try/fall order)
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 3"
```

> **关键约束（spec §0.2.6）**：本 Task commit message 与 spec §2.2 一致地标 RNDR=WEAK。RNDR=STRONG 是 AAGU-5 父契约初稿提法，已被 spec §2.2 重裁定为 WEAK（AAGU-5.3 决定，合并稿 §5.1 Option A 严格分级）。本 commit 不再重新裁决 RNDR 等级。

---

## Task 4: Refactor `kernel/random/random.c` — three-way q switch

**Files:**
- Modify: `kernel/random/random.c`（删除 `mix_hw_entropy()`，改 facade + 三分支 switch；改 reseed）

**Interfaces:**
- Consumes: `arch_random_get_entropy()` / `arch_random_get_strong()`（Task 2 / 3 已实现）
- Produces: `random_init()` STRONG/WEAK/NONE 三分支显式决策 `random_ready`；`reseed()` NONE→drop ready=false 新行为

- [ ] **Step 1: 替换文件顶部 includes 与 helper 定义**

将 `kernel/random/random.c:1-79` 的注释 + `mix_hw_entropy()` 实现替换为：

```c
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
```

**删除**：`mix_hw_entropy()` 函数体（行 63-79）— 已被 facade 取代。

- [ ] **Step 2: 重写 `random_init()` 为三分支 switch**

替换 `kernel/random/random.c` 当前 `random_init()` 函数体（行 81-110）为：

```c
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
```

- [ ] **Step 3: 重写 `reseed()` — facade + NONE→drop ready 新行为**

替换 `kernel/random/random.c` 当前 `reseed()` 函数体（行 128-153）为：

```c
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
        log_error("CSPRNG: reseed got NONE; pool dropped to not-ready\n");
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
```

- [ ] **Step 4: 编译 x86_64 + aarch64 — 期望全绿**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -10
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -10
```

期望：两个 profile 都编译通过，无 `undefined reference`。如有警告 `unused variable` 等可接受但需备注；其它停下排查。

- [ ] **Step 5: 在 QEMU 默认 x86_64 跑一次，验证 boot log**

```bash
make PROFILE=x86_64-clang run 2>&1 | tee /tmp/boot_default.log | tail -40
grep -E "CSPRNG.*pool seeded|CSPRNG.*no hardware" /tmp/boot_default.log
```

期望命中 `CSPRNG: no hardware entropy source ... pool not ready`。

- [ ] **Step 6: 在 QEMU 加 `+rdrand,+rdseed` 跑一次，验证 STRONG**

```bash
make PROFILE=x86_64-clang QEMU_EXTRA_CPU_FLAGS="+rdrand,+rdseed" run 2>&1 | tee /tmp/boot_strong.log | tail -40
grep -E "CSPRNG.*pool seeded|CSPRNG.*no hardware" /tmp/boot_strong.log
```

期望命中 `CSPRNG: pool seeded STRONG (RDSEED/RNDRRS)`。

- [ ] **Step 7: 提交**

```bash
git add kernel/random/random.c
git commit -m "refactor(random): use facade with explicit three-way q switch

- Replace inline mix_hw_entropy() (cycle-counter fallback) with
  arch_random_get_entropy() facade call
- random_init() branches on q explicitly (STRONG / WEAK / NONE)
  rather than the facade's bool return value — quality drives
  random_ready decision, not just log message
- AAGU-2 fail-closed mapping (spec §5.2):
    STRONG   → ready=true (RDSEED/RNDRRS, raw entropy)
    WEAK     → ready=true (RDRAND/RNDR DRBG, AAGU-5 §交付物 2 授权)
    NONE     → ready=false (fail-closed, AAGU-2 既有)
- reseed() with NONE drops ready=false (new behavior vs AAGU-2's
  cycle-counter mix — rationale: NONE means zero new entropy, not
  low entropy; spec §7.3)
- AT_RANDOM STRONG-only path NOT in this task — kernel/sched/task.c
  caller update is in Task 5 (random_init/random.c unchanged for
  AT_RANDOM path)
- Spec compliance: §0.2.1, §0.2.3, §0.2.4, §5.1, §5.2, §7.2, §7.3
- Verified QEMU default → NONE log + QEMU +rdrand,+rdseed → STRONG log
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 4"
```

---

### Task 3.5: Build system whitelist + macro propagation

**Files:**
- Modify: `mk/project.mk`（加 `KERNEL_TEST_FORCE_NO_RNDRRS` 到 `OS01_SUBMAKE_ALLOWED`）
- Modify: `kernel/Makefile`（加 `ifdef` 块注入 `-DKERNEL_TEST_FORCE_NO_RNDRRS=1`）
- Modify: `mk/profiles/x86_64-clang.mk`（加 `KERNEL_VARIANT=weak-selftest` 分支 + 条件 `KERNEL_BUILD_DIR`）
- Modify: `mk/profiles/aarch64-clang.mk`（同上，加 variant 支持）

> **v5 设计变更理由（review item 5 v4 反馈）**：v4 的 `build_kernel_for_mode()` 直接覆盖 `KERNEL_BUILD_DIR` env + 透传未授权 `CFLAGS`，但 profile 用 `:=` 硬编码 `KERNEL_BUILD_DIR`（`mk/profiles/aarch64-clang.mk:13`），env 覆盖失效；受控 sub-make 白名单 `OS01_SUBMAKE_ALLOWED`（`mk/project.mk:78`）不含 `CFLAGS`，透传宏不可靠。本任务把 test 宏正式纳入 build system：白名单 + profile variant + kernel Makefile `ifdef`，与 `KERNEL_SELFTEST` 模式一致。

**Interfaces:**
- Consumes: 用户/脚本传 `KERNEL_TEST_FORCE_NO_RNDRRS=1 KERNEL_SELFTEST=1`
- Produces: kernel 编译时 `-DKERNEL_TEST_FORCE_NO_RNDRRS=1`；aarch64 build dir 变 `build/<profile>/kernel/weak-selftest/`；与 regular selftest build 隔离

- [ ] **Step 1: `mk/project.mk` 加白名单项**

定位 `OS01_SUBMAKE_ALLOWED := CLANG ... KERNEL_CANARY_SELFTEST OS01_SYSTEST ...`（`mk/project.mk:78`），在 `KERNEL_CANARY_SELFTEST` 后加：

```diff
- OS01_SUBMAKE_ALLOWED := CLANG UEFI_CLANG ... KERNEL_CANARY_SELFTEST OS01_SYSTEST OS01_NETTEST ...
+ OS01_SUBMAKE_ALLOWED := CLANG UEFI_CLANG ... KERNEL_CANARY_SELFTEST KERNEL_TEST_FORCE_NO_RNDRRS OS01_SYSTEST OS01_NETTEST ...
```

- [ ] **Step 2: `kernel/Makefile` 加 ifdef 注入**

定位 `kernel/Makefile:217` 现有 `ifdef KERNEL_SELFTEST / ALL_CFLAGS += -DOS01_SELFTEST=1` 块，在其后加：

```makefile
ifdef KERNEL_TEST_FORCE_NO_RNDRRS
ALL_CFLAGS += -DKERNEL_TEST_FORCE_NO_RNDRRS=1
endif
```

> 该宏注入只走白名单 env var；不可由外部 CFLAGS 触发（受控 sub-make 不透传 CFLAGS）。

> **v6 修订**：再加一个 variant 标识宏，让 kernel 在 boot log 打印当前 build variant，script 可在启动后校验 image 与期望 mode 一致（reviewer v6 要求）。

定位同一 `ifdef KERNEL_TEST_FORCE_NO_RNDRRS` 块**之后**，追加：

```makefile
# v8: 用 KERNEL_VARIANT_NAME 标识符（无引号）+ C 预处理器字符串化（#x）
# 完全避开 Makefile/shell/clang 三层之间的引号转义问题。
ifdef KERNEL_VARIANT
ALL_CFLAGS += -DKERNEL_VARIANT_NAME=$(KERNEL_VARIANT)
endif
```

> **v8 设计变更**：v7 的 `-D...=\"$(KERNEL_VARIANT)\"` 在 Makefile → shell 路径中引号被 shell 移除（v8 review item 1 指出），clang 仍收到 token 而非字符串字面量。v8 改用 C 预处理器字符串化（`#x`）— Makefile 只注入 token `KERNEL_VARIANT_NAME=weak-selftest`，由 C 端 `#x` stringification 转成字符串字面量 `"weak-selftest"`。彻底脱离 shell 引号层。

> C 代码侧宏（kernel/random/random.c v8）：

```c
#define OS01_STRINGIFY_(x) #x
#define OS01_STRINGIFY(x) OS01_STRINGIFY_(x)
#ifdef KERNEL_VARIANT_NAME
log_info("CSPRNG: kernel build variant=%s\n", OS01_STRINGIFY(KERNEL_VARIANT_NAME));
#else
log_info("CSPRNG: kernel build variant=default\n");
#endif
```

> `OS01_STRINGIFY` 两层宏是 C99 标准 idiom — 内层 `#x` 不会展开参数，外层强制宏展开后再 stringify。clang 收到 `-DKERNEL_VARIANT_NAME=weak-selftest` 后，`OS01_STRINGIFY(KERNEL_VARIANT_NAME)` 展开为 `"weak-selftest"`，可作 `%s` 参数。

> **实施时 sanity check**（v8 修正）：预处理宏不成为符号 — 改用：
> ```bash
> clang -E -DKERNEL_VARIANT_NAME=weak-selftest -dM kernel/random/random.c | grep KERNEL_VARIANT_NAME
> strings build/aarch64-clang/kernel/weak-selftest/kernel.bin | grep weak-selftest
> ```
> 前者验证宏定义（应见 `#define KERNEL_VARIANT_NAME weak-selftest`），后者验证 stringify 后的字符串确实编进 image。





- [ ] **Step 3: `mk/profiles/x86_64-clang.mk` 加 variant 分支**

定位 `mk/profiles/x86_64-clang.mk:27-33`（`KERNEL_VARIANT := selftest` 块），改为：

```makefile
ifneq ($(filter 1,$(KERNEL_TEST_FORCE_NO_RNDRRS)),)
KERNEL_VARIANT := weak-selftest
else ifneq ($(filter 1,$(KERNEL_SELFTEST)),)
KERNEL_VARIANT := selftest
else ifneq ($(filter 1,$(KERNEL_CANARY_SELFTEST)),)
KERNEL_VARIANT := canary-selftest
else
KERNEL_VARIANT :=
endif
```

`KERNEL_BUILD_DIR`（line 34）已是 `$(BUILD_DIR)/kernel$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))`，variant=weak-selftest 自动得 `build/<profile>/kernel/weak-selftest/`，与 regular selftest build 隔离。

> 实施时**同时**设 `KERNEL_TEST_FORCE_NO_RNDRRS=1 KERNEL_SELFTEST=1`（后者让 `-DOS01_SELFTEST=1` 触发 selftest 编译进 kernel）。variant 优先选 weak-selftest，所以不会落到 selftest variant。

- [ ] **Step 4: `mk/profiles/aarch64-clang.mk` 加 variant 支持（aarch64 当前无 variant）**

定位 `mk/profiles/aarch64-clang.mk` 文件开头（紧跟 `OS01_ROOT := ...` 块），**新增**：

```makefile
# Compile-affecting variant — mirror x86_64 profile's KERNEL_VARIANT block
# so weak-selftest build produces an isolated kernel/weak-selftest/ dir.
ifneq ($(filter 1,$(KERNEL_TEST_FORCE_NO_RNDRRS)),)
KERNEL_VARIANT := weak-selftest
else ifneq ($(filter 1,$(KERNEL_SELFTEST)),)
KERNEL_VARIANT := selftest
else ifneq ($(filter 1,$(KERNEL_CANARY_SELFTEST)),)
KERNEL_VARIANT := canary-selftest
else
KERNEL_VARIANT :=
endif
# Replace existing hardcoded KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel with:
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))
```

`aarch64-uefi` target 消费 `KERNEL_BUILD_DIR` 下的 `kernel.bin`（`mk/targets/aarch64.mk` 路径），variant 化后 aarch64 WEAK build 自动落到 `build/aarch64-clang/kernel/weak-selftest/`；对应 UEFI image 在同 build root 下生成。

> **v6 修订**：以上 KERNEL_BUILD_DIR variant 化让 kernel.bin 路径正确（`kernel/weak-selftest/`），但 `mk/targets/aarch64.mk:5` 的 `AARCH64_UEFI_DISK` 仍指向共享路径 `$(BUILD_DIR)/image/aarch64-uefi.img` — WEAK build 会覆盖 STRONG/NONE 的 image，STRONG/NONE 测试因 image 已"存在"跳过 rebuild，导致启动错位 image。本 Step 同时改 image path。

- [ ] **Step 4.5: aarch64 image 路径 + 生成规则 variant 化**

`mk/targets/aarch64.mk:5` 改路径：

```diff
- AARCH64_UEFI_DISK     := $(BUILD_DIR)/image/aarch64-uefi.img
- AARCH64_UEFI_FIRMWARE := $(BUILD_DIR)/image/QEMU_EFI.fd
+ AARCH64_UEFI_DISK     := $(BUILD_DIR)/image$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))/aarch64-uefi.img
+ AARCH64_UEFI_FIRMWARE := $(BUILD_DIR)/image$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))/QEMU_EFI.fd
```

**v7 修订**：仅改路径还不够 — `mk/components/image.mk:154` 的 image 生成规则**仍硬编码** `$(BUILD_DIR)/image/aarch64-uefi.img` 作为 target。WEAK build 会因找不到 `.../image/weak-selftest/aarch64-uefi.img` 的 recipe 报 "No rule to make target"。同时修：

`mk/components/image.mk:154` 改为用 `$(AARCH64_UEFI_DISK)` 作 target，并让依赖项中的 `$(AARCH64_UEFI_FIRMWARE)` 引用路径：

```diff
-$(BUILD_DIR)/image/aarch64-uefi.img: $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI \
-		$(BUILD_DIR)/artifacts/kernel.elf $(AARCH64_UEFI_FIRMWARE)
+$(AARCH64_UEFI_DISK): $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI \
+		$(BUILD_DIR)/artifacts/kernel.elf $(AARCH64_UEFI_FIRMWARE)
```

recipe 内容（mkdir / truncate / mkfs.fat / mmd / mcopy）不变 — `$@` 自动展开为 variant 路径。

现在三档 image 路径 + 生成规则：

| variant | image 路径 | rule target |
|---|---|---|
| (empty) — STRONG/NONE | `build/aarch64-clang/image/aarch64-uefi.img` | `$(AARCH64_UEFI_DISK)` = 同上 |
| `weak-selftest` — WEAK | `build/aarch64-clang/image/weak-selftest/aarch64-uefi.img` | `$(AARCH64_UEFI_DISK)` = 同上 |

`$(AARCH64_UEFI_DISK)` 在 `mk/targets/aarch64.mk` 内已 variant 化，`mk/components/image.mk` 引用它即可。两侧引用同一变量避免再次脱钩。

- [ ] **Step 5: 编译 sanity check（aarch64 weak-selftest variant）**

```bash
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 KERNEL_TEST_FORCE_NO_RNDRRS=1 all 2>&1 | tail -20
ls -la build/aarch64-clang/kernel/weak-selftest/kernel.bin  # 必须存在
nm build/aarch64-clang/kernel/weak-selftest/kernel.bin | grep arch_random_get_entropy  # 必须有符号
```

期望：kernel 编译成功，build dir 为 `kernel/weak-selftest/`（不与 regular selftest 冲突），symbol `arch_random_get_entropy` 存在。

- [ ] **Step 6: 编译 sanity check（aarch64 regular selftest 不变）**

```bash
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -10
ls -la build/aarch64-clang/kernel/selftest/kernel.bin  # 必须存在（与 weak-selftest 分开）
```

期望：regular selftest build 仍落到 `kernel/selftest/`，不受 variant=weak-selftest 污染。

- [ ] **Step 7: 提交**

```bash
git add mk/project.mk kernel/Makefile \
    mk/profiles/x86_64-clang.mk mk/profiles/aarch64-clang.mk \
    mk/targets/aarch64.mk mk/components/image.mk
git commit -m "build(aagu-5): KERNEL_TEST_FORCE_NO_RNDRRS whitelisted + variant

- mk/project.mk: add KERNEL_TEST_FORCE_NO_RNDRRS to OS01_SUBMAKE_ALLOWED
  so the env var crosses the controlled sub-make boundary without
  needing shell-side CFLAGS injection (which the whitelist rejects)
- kernel/Makefile: add ifdef KERNEL_TEST_FORCE_NO_RNDRRS to inject
  -DKERNEL_TEST_FORCE_NO_RNDRRS=1 into ALL_CFLAGS, mirroring the
  existing KERNEL_SELFTEST pattern at line 217
- mk/profiles/x86_64-clang.mk: extend KERNEL_VARIANT logic so
  KERNEL_TEST_FORCE_NO_RNDRRS=1 → variant=weak-selftest, isolating
  weak-test build from regular selftest under kernel/weak-selftest/
- mk/profiles/aarch64-clang.mk: add the same variant logic (was
  missing — aarch64 had hardcoded KERNEL_BUILD_DIR := \$(BUILD_DIR)/kernel
  with no conditional); now aarch64 weak-test build produces
  build/aarch64-clang/kernel/weak-selftest/kernel.bin + UEFI image
- Spec compliance: §0.2.7 (test infrastructure must use established
  build patterns, not shell hacks)
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 3.5"
```

---

## Task 5: AT_RANDOM 路径改用 `arch_random_get_strong()` + 错误传播

**Files:**
- Modify: `kernel/sched/task.c`
  - `setup_user_stack()` 行 1140：`static void` → `static int`（返回 0 成功 / -1 失败）
  - `setup_user_stack()` 内部 AT_RANDOM 段（行 1178-1182）：16B 缓冲 → 32B，调 `arch_random_get_strong()`，失败 `return -1`
  - 三处调用点全部更新：
    - `spawn_user_task()` 行 1362 — 检查返回值，失败返回 `-EAGAIN`（沿用 AAGU-2 的 fail-closed 错误码）
    - `sys_exec()` 行 1488 — 检查返回值，失败返回 `-EAGAIN`
    - `task_selftest_auxv_probe()` 行 2296 — 检查返回值，失败返回 `-1`（selftest 现有契约）

**Interfaces:**
- Consumes: `arch_random_get_strong()`（Task 2 / 3 已实现）
- Produces: `setup_user_stack()` 现在可失败；三处调用点传播失败

- [ ] **Step 1: 读 `kernel/sched/task.c` 1140、1362、1488、2296 行确认实际签名**

```bash
grep -n "setup_user_stack" kernel/sched/task.c
awk 'NR<=1362 && /^[a-zA-Z_].*\(/ {f=$0} END{print "caller1:", f}' kernel/sched/task.c
awk 'NR<=1488 && /^[a-zA-Z_].*\(/ {f=$0} END{print "caller2:", f}' kernel/sched/task.c
awk 'NR<=2296 && /^[a-zA-Z_].*\(/ {f=$0} END{print "caller3:", f}' kernel/sched/task.c
```

期望：
- 行 1140 附近：`static void setup_user_stack(uint8_t *kstack, char *const argv[], ...)`
- caller1 = `int64_t spawn_user_task(const char *path, const char *const *argv)`
- caller2 = `int64_t sys_exec(const char *path, pt_regs_t *regs, ...)`
- caller3 = `int task_selftest_auxv_probe(char *const argv[], char *const envp[], ...)`

三个 caller 都已返回 int/int64_t — 加一段 `-EAGAIN` / `-1` 路径即可，不需改 caller 函数签名。

- [ ] **Step 2: 改 `setup_user_stack()` 签名为 `static int`**

定位行 1140，把：

```c
static void setup_user_stack(uint8_t *kstack, char *const argv[], char *const envp[],
                             int s_argc, int s_envc,
                             uint64_t *out_argv_ptr, uint64_t *out_envp_ptr,
                             uint64_t *out_rsp)
```

改为：

```c
/* 返回 0 成功 / -1 失败（仅失败源：AT_RANDOM STRONG-only 拒绝 WEAK/NONE，
 * spec §6）。栈布局本身仍 infallible — 失败一定由 AT_RANDOM 引起。 */
static int setup_user_stack(uint8_t *kstack, char *const argv[], char *const envp[],
                            int s_argc, int s_envc,
                            uint64_t *out_argv_ptr, uint64_t *out_envp_ptr,
                            uint64_t *out_rsp)
```

- [ ] **Step 3: 改 AT_RANDOM 段用 32B 缓冲 + 失败返回 -1**

定位行 1178-1182 附近（`get_random_bytes(KSTACK(rsp), 16);` 上下文），把：

```c
    /* AT_RANDOM payload：16B 内核 CSPRNG，16 字节对齐不跨字
     * （spec 2026-09-17 §6.3）。 */
    rsp = (rsp - 16) & ~15ULL;
    get_random_bytes(KSTACK(rsp), 16);
    uint64_t at_random_addr = rsp;
```

改为：

```c
    /* AT_RANDOM payload：16B 内核 STRONG-only（spec §6）。
     * 旧实现走 get_random_bytes()，WEAK-only pool 下也会成功 — AAGU-5
     * 父契约 §交付物 3 要求 AT_RANDOM 仅 STRONG，否则 fail-closed。
     * arch_random_get_strong() 写入 32B（facade 契约 — spec §3.1），
     * 取前 16B 写到 KSTACK；剩余 16B 立即 memset 0 不残留栈。
     * 失败时 WEAK/NONE 环境整个 setup_user_stack() 返 -1。 */
    uint8_t at_random_buf[32];
    if (!arch_random_get_strong(at_random_buf)) {
        memset(at_random_buf, 0, 32);
        return -1;
    }
    rsp = (rsp - 16) & ~15ULL;
    memcpy(KSTACK(rsp), at_random_buf, 16);
    memset(at_random_buf, 0, 32);
    uint64_t at_random_addr = rsp;
```

- [ ] **Step 4: 改函数尾部 `}` 前为 `return 0;`**

定位原 `setup_user_stack()` 函数末尾（应在 `out_envp_ptr = user_env_ptr;` 之类之后，行 ~1230-1240 附近），在 `}` 前加：

```c
    return 0;
}
```

原代码 `}` 前是栈默认成功路径（无显式 return）。需要补 `return 0;` 因为签名从 void 改 int。

- [ ] **Step 5: 更新 `spawn_user_task()`（行 1362 调用点）**

定位 `setup_user_stack(kstack, (char *const *)argv, NULL, s_argc, s_envc, ...);` 行（1362 附近），把：

```c
    setup_user_stack(kstack, (char *const *)argv, NULL, s_argc, s_envc,
                     &user_arg_ptr, &user_env_ptr, &user_rsp);
```

改为：

```c
    if (setup_user_stack(kstack, (char *const *)argv, NULL, s_argc, s_envc,
                         &user_arg_ptr, &user_env_ptr, &user_rsp) != 0) {
        /* AT_RANDOM STRONG-only 失败 — 清理已分配资源后返回 -EAGAIN。
         * 顺序与现有 elf_load 失败路径（kernel/sched/task.c:1316-1322）一致，
         * **外加** task_list_lock 释放后再调 files_unpin（`kernel/include/fs/file.h:140-142`
         * 明确：files_unpin/files_put_file 不得在 task_list_lock / fs->lock / rq lock 持锁下调用，
         * 其 drop-to-zero 路径可能同步 files_free/file_free）。 */
        uint64_t tl_flags2 = spin_lock_irqsave(&task_list_lock);
        list_del(&tsk->list);
        spin_unlock_irqrestore(&task_list_lock, tl_flags2);
        /* 现在 task_list_lock 已释放，可安全调 files_unpin。 */
        if (tsk->files) {
            files_unpin(tsk->files);
            tsk->files = NULL;
        }
        if (tsk->fpu_save) fpu_area_free(tsk->fpu_save);
        free_pages(stack_page, 1);
        vmm_free_user_map(user_pgd);
        kfree(mm);
        kfree(thd);
        kfree(raw_alloc);
        return -EAGAIN;
    }
```

`spawn_user_task` 返回 `int64_t`，沿用 `-EAGAIN`（与现有 spawn 失败错误码一致）。

> **资源回收（review item 5 新增）**：调用 `setup_user_stack` 前已分配并部分初始化 `tsk` / `thd` / `mm` / `user_pgd` / `stack_page` / `tsk->files` / `tsk->fpu_save` — 失败时必须清理以免资源泄漏。具体见下方代码块。

- [ ] **Step 6: 更新 `sys_exec()`（行 1488 调用点）**

定位 `setup_user_stack(kstack, (char *const *)argv, (char *const *)envp, ...)` 行（1488 附近），把：

```c
    setup_user_stack(kstack, (char *const *)argv, (char *const *)envp,
                     s_argc, s_envc, &user_arg_ptr, &user_env_ptr, &user_rsp);
```

改为：

```c
    if (setup_user_stack(kstack, (char *const *)argv, (char *const *)envp,
                         s_argc, s_envc, &user_arg_ptr, &user_env_ptr, &user_rsp) != 0) {
        /* AT_RANDOM STRONG-only 失败 — 清理已分配资源后返回 -EAGAIN。
         * sys_exec 路径（与 spawn_user_task 不同）：node 已在 step 1 vfs_node_put；
         * 只需释放 stack_page + new_pgd + new_mm。 */
        free_pages(stack_page, 1);
        vmm_free_user_map(new_pgd);
        kfree(new_mm);
        return -EAGAIN;
    }
```

`sys_exec` 返回 `int64_t`，同样沿用 `-EAGAIN`。

- [ ] **Step 7: 更新 `task_selftest_auxv_probe()`（行 2296 调用点）**

定位 `setup_user_stack(task_selftest_stack_img, argv, envp, argc, envc, ...)` 行（2296 附近），把：

```c
    setup_user_stack(task_selftest_stack_img, argv, envp, argc, envc,
                     &argp, &envpp, &rsp);
```

改为：

```c
    if (setup_user_stack(task_selftest_stack_img, argv, envp, argc, envc,
                         &argp, &envpp, &rsp) != 0) {
        return -1;   /* selftest 中 AT_RANDOM 失败 → probe 失败 */
    }
```

`task_selftest_auxv_probe` 返回 `int`，沿用现有 `-1` 失败路径（与 `argc+envc > STARTUP_STR_MAX` 的早返回一致）。

- [ ] **Step 8: 编译 x86_64 + aarch64 — 期望全绿**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -10
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -10
```

期望：全绿。原有 `setup_user_stack` 注释 "this function cannot fail" 现在不准确了 — 实施时一并更新注释（在 Step 2 签名下方加 "AT_RANDOM STRONG-only 可失败"）。

- [ ] **Step 9: QEMU 默认（NONE）跑一次，验证 spawn 路径触发 -EAGAIN**

```bash
make PROFILE=x86_64-clang run 2>&1 | tee /tmp/aagu5_task5.log | tail -40
grep -E "AT_RANDOM|spawn_user_task|EAGAIN" /tmp/aagu5_task5.log
```

期望：boot log 含 `CSPRNG: no hardware entropy source`（NONE），init 进程 spawn 因 AT_RANDOM 失败被 `-EAGAIN` 拒绝。注：init 进程 spawn 失败意味着系统不会启动到 shell — 这是预期 fail-closed 行为。如需 QEMU 持续运行观察详细 trace，可加 `-d guest_errors`。

- [ ] **Step 10: 提交**

```bash
git add kernel/sched/task.c
git commit -m "refactor(task): AT_RANDOM STRONG-only + setup_user_stack error propagation

- setup_user_stack() signature: static void → static int
  (returns 0 on success, -1 if AT_RANDOM STRONG-only rejects
  WEAK/NONE entropy — spec §6, AAGU-5 父契约 §交付物 3)
- AT_RANDOM section: 16B buffer → 32B buffer (matches facade
  contract spec §3.1); arch_random_get_strong() fills 32B,
  first 16B copied to KSTACK, remaining 16B + buffer memset 0
- Three callers propagate -1 failure:
    spawn_user_task (line 1362)         → return -EAGAIN
    sys_exec         (line 1488)         → return -EAGAIN
    task_selftest_auxv_probe (line 2296) → return -1
- Stack layout construction itself stays infallible; -1 is
  sourced ONLY from AT_RANDOM (single failure point makes
  caller branches simple)
- In QEMU default (no STRONG entropy), spawn_user_task rejects
  init → kernel reaches no-shell fail-closed terminal, matching
  AAGU-2 fail-closed behavior on a pool that's not ready
- Spec compliance: §0.2.3 (quality drives control flow),
  §6 (AT_RANDOM path), §3.3 (caller table)
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 5"
```

---

## Task 5.7: AAGU-5.7 spawn/exec 策略决策（fail-fast，理由与边界）

**目的**：把 `setup_user_stack()` 在 WEAK/NONE 时的失败处理策略**写到本 plan 文件**，作为 spec §6 AT_RANDOM STRONG-only 失败的运行时契约。AAGU-5.7 父 issue 要求三选一，本节锁定本任务的选择。

### 决策（已落地于 Task 5）

**选项 b — fail-fast 返回用户态错误**。`setup_user_stack()` 在 WEAK/NONE 时返回 `-1`；`spawn_user_task()` 与 `sys_exec()` 调用点把 `-1` 翻译为 `-EAGAIN` 返回给用户态；`task_selftest_auxv_probe()` 返回 `-1`。**不重试、不阻塞、不 panic**。

### 候选方案对比

| 方案 | 行为 | 优点 | 缺点 | 本任务选 / 不选 |
|---|---|---|---|---|
| (a) 阻塞重试等 STRONG | `setup_user_stack()` 轮询 `pool_quality` 直到 STRONG，再产 AT_RANDOM | 自动恢复，dev 环境可能救回 | OS01 不支持运行时硬件重新探测（spec §4.2）——STRONG 永不出现 → 永久阻塞；init 不启动 → 看起来像 init hang，根因不可观测 | **不选** |
| (b) fail-fast 返回用户态错误（**本任务选**） | `setup_user_stack()` 返 `-1` → 调用点返 `-EAGAIN` | 失败立即可见；用户态可观测、可决策（重试 syscall / 退到 fallback init）；与 AAGU-2 fail-closed 行为一致；不阻塞 boot | 用户态需处理 `-EAGAIN`（POSIX 语义：`exec` / `spawn` 已习惯） | **选** |
| (c) panic | `kernel_panic("AT_RANDOM STRONG unavailable")` | 失败最不可错过 | 违反 OS01 现有 fail-closed 哲学（AAGU-2 §1：pool 不 ready 时 `get_random_bytes` memset(0)，不 panic）；让 kernel 失去 fail-soft 余地 | **不选** |

### 为什么 (b) 优于 (a) 与 (c)

1. **与 AAGU-2 fail-closed 一致**：AAGU-2 已有契约是"无硬件熵时 `get_random_bytes()` memset(0) → caller 检测全零 → 显式 abort"。AT_RANDOM 选 fail-fast 把同样的契约从用户态搬到内核边界——`spawn_user_task` 返 `-EAGAIN` 让 init 的 retry/spawn-fallback 逻辑有显式信号，而不是 pool "看起来 ready 但产出全零"。
2. **可观测性**：init 启动时 `-EAGAIN` 立即被 `kernel/printk` 打出来；QEMU dev 环境一秒内就能定位是 entropy 缺失，而不是去调试 spawn 的 elf_load 路径。
3. **运行时不可逆性约束**：spec §4.2 明文"运行时不存在从 WEAK/NONE 升级到 STRONG 的路径"——意味着阻塞重试策略在 OS01 上**物理不可能成功**。重试策略等同于永久阻塞，是 bug 而非容错。

### 错误码选择（spec §6）

- `spawn_user_task()` 与 `sys_exec()` 返 `-EAGAIN`（POSIX `EAGAIN` 语义：资源暂时不可用，可重试）。这与 AAGU-2 既有的 `get_random_bytes` 在 pool 不 ready 时 memset(0) 的"caller 检测 → 决策"模式一致。
- AAGU-5.7 issue body 提到 `ENOSYS` / `ENOENTROPY` 等候选。本任务**沿用** `-EAGAIN`，理由：(1) 与 AAGU-2 fail-closed 错误码一致；(2) `EAGAIN` 在 POSIX 语义里就是"重试也许会成功"，正好匹配 entropy 不可用时的语义；(3) 不引入新 errno 是 OS01 小内核哲学的偏好。新 errno（如 `ENOENTROPY`）留待后续 follow-up（如有 userspace 工具需要精确区分）。

### 边界条件（Task 5 实施已覆盖）

- `task_selftest_auxv_probe()`（内核自测）返 `-1`：selftest 不抛错误，预期失败计入 `failed` 计数；这与现有 `argc+envc > STARTUP_STR_MAX` 早返 `-1` 路径一致。
- QEMU 默认（NONE 熵）启动：`spawn("/bin/init")` 失败 → init 不启动 → kernel 停在 no-shell 终端，**这是预期 fail-closed 行为**（Task 5 Step 9 验证）。

### 后续 follow-up（本期不实施）

- (a) 若未来 OS01 支持运行时硬件重探测（如 hotplug CPU 或 UEFI GetRNG 后向注入），则 spawn/exec 可以从 fail-fast 升级为阻塞重试 + timeout。但本期 spec §4.2 锁定不可逆，无须实现。
- (b) 若有 userspace 工具需要更精确的错误区分，可新增 `ENOENTROPY` errno 并把 `kernel_random_get_strong()` 的失败翻译为它。本期沿用 `-EAGAIN`。

### Task 5.7 验收

- [x] 策略写入 plan 文件（本节）—— **本节为契约**
- [x] 决策为选项 b（fail-fast）—— Task 5 Step 5/6/7 已落地
- [x] 错误码选择（`-EAGAIN`）+ 理由 —— 本节
- [x] 不引入新 errno —— 沿用 `-EAGAIN`，留 follow-up 余地

---

## Task 6: Add single-mode-aware selftest `test_entropy_quality.c`

**Files:**
- Create: `kernel/selftest/test_entropy_quality.c`
- Modify: `kernel/selftest/selftest.c`（注册 **单个** 新 selftest）

**Interfaces:**
- Consumes: `arch_random_get_entropy()` / `arch_random_get_strong()` / `random_is_ready()`
- Produces: 单一 selftest 函数 `entropy_quality_selftest_current_mode()`，运行一次后从 `q` 推断当前质量档并断言该档的全部契约；同一函数在三种 QEMU 模式下都 PASS

> **设计变更理由（v3 评审 item 5）**：v2 注册 4 个互斥 selftest（`_strong/_weak/_none/_filter`），现有 runner (`kernel/selftest/selftest.c:179-193`) 无条件跑全部，任一失败即整个 selftest 失败。runner 没有按 QEMU 模式过滤的机制；改 runner 加 mode filter 影响所有现有 selftest，风险大。新方案：单一测试，运行时检测 `q` 后跑对应档的断言 — 同一函数在 STRONG / WEAK / NONE 模式下都通过。

- [ ] **Step 1: 创建 `kernel/selftest/test_entropy_quality.c`**

```c
// kernel/selftest/test_entropy_quality.c — facade 当前质量档契约验证。
//
// 单次 boot 检测 facade 在当前硬件/QEMU 配置下产出的 quality，
// 然后断言该档的全部契约（spec §3.2 / §3.3 / §5.1 / §5.2）。
// 同一函数在 STRONG / WEAK / NONE 三种 QEMU 模式下都 PASS；
// 编排脚本（tests/scripts/qemu_entropy_modes.sh, Task 8）通过 grep
// `[selftest] entropy_quality_selftest_current_mode... PASS` 校验。
//
// 断言总览（每档独立）：
//   NONE  → facade return false, q==NONE, out memset 0, !random_is_ready,
//           arch_random_get_strong() return false
//   WEAK  → facade return true,  q==WEAK, out non-zero,  random_is_ready
//           (spec §5.2 WEAK→ready),  arch_random_get_strong() return false
//   STRONG→ facade return true,  q==STRONG, out non-zero, random_is_ready,
//           arch_random_get_strong() return true
#ifdef OS01_SELFTEST

#include <core/selftest.h>
#include <core/printk.h>
#include <arch/random.h>
#include <random/random.h>     // random_is_ready
#include <string.h>
#include <stdint.h>

static int buf_all_zero(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 0) return 0;
    return 1;
}

static int buf_any_nonzero(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 0) return 1;
    return 0;
}

/* 通用 fail-with-context：避免每个 assert 都写 serial_printk。 */
#define FAIL(fmt, ...) do {                                  \
    serial_printk("[selftest] entropy_quality_selftest_current_mode: " \
                  fmt "\n", ##__VA_ARGS__);                  \
    return -1;                                               \
} while (0)

int entropy_quality_selftest_current_mode(void)
{
    /* 第 1 步：调 facade 拿当前 quality 档。预填 buf 为非零 —
     * 若 facade 走失败路径（NONE），必须主动 memset 0 覆盖预填值。 */
    uint8_t buf[32];
    memset(buf, 0xAA, 32);
    arch_entropy_source_t q = ARCH_ENTROPY_STRONG;  /* 故意错初值 */
    bool facade_ok = arch_random_get_entropy(buf, &q);

    switch (q) {
    case ARCH_ENTROPY_NONE:
        if (facade_ok)
            FAIL("NONE mode: facade returned true (expected false)");
        if (!buf_all_zero(buf, 32))
            FAIL("NONE mode: out not all zero (facade must memset 0)");
        if (random_is_ready())
            FAIL("NONE mode: random_is_ready()=true (should be fail-closed)");
        break;

    case ARCH_ENTROPY_WEAK:
        if (!facade_ok)
            FAIL("WEAK mode: facade returned false");
        if (!buf_any_nonzero(buf, 32))
            FAIL("WEAK mode: out all zero (DRBG should produce entropy)");
        if (!random_is_ready())
            FAIL("WEAK mode: random_is_ready()=false "
                 "(spec §5.2: WEAK → ready=true is authorized)");
        break;

    case ARCH_ENTROPY_STRONG:
        if (!facade_ok)
            FAIL("STRONG mode: facade returned false");
        if (!buf_any_nonzero(buf, 32))
            FAIL("STRONG mode: out all zero (raw entropy should produce bytes)");
        if (!random_is_ready())
            FAIL("STRONG mode: random_is_ready()=false");
        break;

    default:
        FAIL("unknown quality=%d (must be 0/1/2)", q);
    }
    memset(buf, 0, 32);

    /* 第 2 步：arch_random_get_strong() 严格只接受 STRONG —
     * WEAK/NONE 模式下必须返 false（spec §6 / §3.3）。 */
    uint8_t strong_buf[32];
    memset(strong_buf, 0xAA, 32);
    bool strong_ok = arch_random_get_strong(strong_buf);
    int expect_strong = (q == ARCH_ENTROPY_STRONG) ? 1 : 0;
    if (strong_ok != expect_strong)
        FAIL("strong_filter: got=%d want=%d (q=%d)",
             strong_ok ? 1 : 0, expect_strong, q);
    if (strong_ok && !buf_any_nonzero(strong_buf, 32))
        FAIL("strong_filter: out all zero despite true return");
    memset(strong_buf, 0, 32);

    return 0;
}
#endif /* OS01_SELFTEST */
```

- [ ] **Step 2: 在 `kernel/selftest/selftest.c` 注册单一新 selftest**

定位 `kernel/selftest/selftest.c` 现有 `selftest_register(...)` 段（约 175 行），加前置声明：

```c
int entropy_quality_selftest_current_mode(void);
```

然后在 `#ifdef OS01_SELFTEST` 块内（行 156-177 范围）已有 selftest 注册末尾加：

```c
    selftest_register("entropy_quality_selftest_current_mode",
                      entropy_quality_selftest_current_mode);
```

> **关键**：只注册 1 个，不是 4 个。现有 runner 无需改动。

- [ ] **Step 3: 编译 x86_64 KERNEL_SELFTEST=1**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -10
```

期望：编译通过。

- [ ] **Step 4: 在 QEMU 默认 x86_64 跑 selftest — 期望 PASS（NONE 档）**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 run 2>&1 | tee /tmp/selftest_none.log | tail -60
grep -E "entropy_quality_selftest_current_mode" /tmp/selftest_none.log
```

期望：日志含 `[selftest] entropy_quality_selftest_current_mode... PASS`。NONE 档所有断言（facade false / q NONE / buf 全零 / !ready / strong 返 false）通过。

- [ ] **Step 5: 在 QEMU 加 `+rdrand` 跑 selftest — 期望 PASS（WEAK 档）**

QEMU 默认硬编码 `-cpu qemu64` 不支持外部 CPU flag 注入。直接调 QEMU：

```bash
KERNEL_BIN=$(find build/x86_64-clang/ -name 'kernel.bin' | head -1)
timeout 30 qemu-system-x86_64 -M q35 -smp 1 -m 256M \
    -kernel "$KERNEL_BIN" -nographic -no-reboot \
    -cpu qemu64,+rdrand \
    -append "OS01_KERNEL_SELFTEST=1" 2>&1 | tee /tmp/selftest_weak.log | tail -60
grep -E "entropy_quality_selftest_current_mode" /tmp/selftest_weak.log
```

期望：日志含 PASS（WEAK 档所有断言通过：facade true / q WEAK / buf 非零 / ready=true（spec §5.2 授权）/ strong 返 false）。

> **注**：`-cpu qemu64,+rdrand` 在 QEMU 9.2.0/11.1.1 验证可用。如实际 QEMU 报错，executor 可改用 `-cpu host`（KVM 模式）或 follow-up。

- [ ] **Step 6: 在 QEMU 加 `+rdrand,+rdseed` 跑 selftest — 期望 PASS（STRONG 档）**

```bash
KERNEL_BIN=$(find build/x86_64-clang/ -name 'kernel.bin' | head -1)
timeout 30 qemu-system-x86_64 -M q35 -smp 1 -m 256M \
    -kernel "$KERNEL_BIN" -nographic -no-reboot \
    -cpu qemu64,+rdrand,+rdseed \
    -append "OS01_KERNEL_SELFTEST=1" 2>&1 | tee /tmp/selftest_strong.log | tail -60
grep -E "entropy_quality_selftest_current_mode" /tmp/selftest_strong.log
```

期望：PASS（STRONG 档全部断言通过：facade true / q STRONG / buf 非零 / ready / strong 返 true）。

- [ ] **Step 7: 提交**

```bash
git add kernel/selftest/test_entropy_quality.c kernel/selftest/selftest.c
git commit -m "test(kernel): entropy facade single-mode-aware selftest

- kernel/selftest/test_entropy_quality.c defines ONE selftest
  (entropy_quality_selftest_current_mode) that detects the
  current QEMU mode at runtime via arch_random_get_entropy()
  and asserts the matching facet's full invariant set:
    NONE  → facade=false, q=NONE, buf=0, !ready, strong=false
    WEAK  → facade=true,  q=WEAK, buf≠0, ready=true (§5.2),
            strong=false
    STRONG→ facade=true,  q=STRONG, buf≠0, ready=true,
            strong=true
- Single test runs in all 3 QEMU modes without failure —
  no need to add filtering to selftest runner
  (kernel/selftest/selftest.c:179-193 runs all unconditionally)
- Verified QEMU modes:
    -cpu qemu64 (default)       → NONE  PASS
    -cpu qemu64,+rdrand         → WEAK  PASS (direct QEMU invocation
                                           since make run hardcodes CPU)
    -cpu qemu64,+rdrand,+rdseed → STRONG PASS
- Spec compliance: §0.2.8, §3.2, §3.3, §5.1, §5.2
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 6"

---

## Task 7: Update `kernel/Makefile` aarch64 whitelist

**Files:**
- Modify: `kernel/Makefile`（仅 aarch64 分支加 `random/random.c`）

**Interfaces:**
- Consumes: `kernel/random/random.c`（Task 4 已重写）
- Produces: aarch64 build 把 `kernel/random/random.c` 链入 `kernel.bin`

- [ ] **Step 1: 读 `kernel/Makefile:42-53` 确认 aarch64 whitelist 当前内容**

```bash
sed -n '42,55p' kernel/Makefile
```

期望：
```
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c \
                   percpu/percpu.c \
                   time/clocksource.c time/tick.c time/timer.c \
                   intr/softirq.c \
                   $(wildcard compiler_rt/*.c) arch/aarch64/subsys.c \
                   arch/aarch64/serial_printk.c arch/aarch64/strcmp.c \
                   subsys/subsys.c \
                   arch/aarch64/idle_resume_stub.c \
                   arch/aarch64/libc_stub.c \
                   arch/aarch64/atomic.c \
                   selftest/test_arch_atomic_u64.c
```

- [ ] **Step 2: 修改 whitelist 加 `random/random.c`**

在 `arch/aarch64/atomic.c \` 行后插入：

```makefile
                   random/random.c \
                   arch/aarch64/random.c \
```

完整修改后 aarch64 段应为：

```makefile
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c \
                   percpu/percpu.c \
                   time/clocksource.c time/tick.c time/timer.c \
                   intr/softirq.c \
                   $(wildcard compiler_rt/*.c) arch/aarch64/subsys.c \
                   arch/aarch64/serial_printk.c arch/aarch64/strcmp.c \
                   subsys/subsys.c \
                   arch/aarch64/idle_resume_stub.c \
                   arch/aarch64/libc_stub.c \
                   arch/aarch64/atomic.c \
                   random/random.c \
                   arch/aarch64/random.c \
                   selftest/test_arch_atomic_u64.c
```

> **注 1**：`arch/aarch64/random.c` 严格说会被 `$(wildcard $(ARCHDIR)/*.c)` 自动收纳，但显式列出让 whitelist 自描述，避免后续删除 wildcard 后丢失。
>
> **注 2**：x86_64 用 `$(wildcard random/*.c)`（line 57）+ `$(wildcard $(ARCHDIR)/*.c)`（line 81），Task 2 新增的 `kernel/arch/x86_64/random.c` 与 `kernel/random/random.c` 改动**自动收纳**，无需 Makefile 修改。
>
> **注 3**：aarch64 phase 1 是显式 whitelist（AGENTS.md 与 Makefile 注释明说），不能依赖 wildcard；本 Task 是**唯一**改 Makefile 的 task。

- [ ] **Step 3: 编译 + 跑 aarch64 QEMU 默认**

```bash
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -10
```

期望：编译通过；`kernel.bin` 含 `arch/aarch64/random.o` 与 `random/random.o`（可用 `nm kernel.bin | grep arch_random_get_entropy` 验证）。

- [ ] **Step 4: 提交**

```bash
git add kernel/Makefile
git commit -m "build(aarch64): add random/random.c to aarch64 whitelist

- aarch64 phase-1 build is explicit-whitelist (kernel/Makefile:42-53,
  comment line 38-41 explains: 'Sources are added back to the
  aarch64 whitelist incrementally as the port grows'). After AAGU-5
  Task 4 refactored kernel/random/random.c to use the arch-neutral
  facade, the file must be explicitly listed for aarch64 to link it
- Add random/random.c + arch/aarch64/random.c to KERNEL_C_SOURCES
- x86_64 KERNEL_C_SOURCES uses \$(wildcard random/*.c) (line 57) +
  \$(wildcard \$(ARCHDIR)/*.c) (line 81) so kernel/arch/x86_64/random.c
  + kernel/random/random.c are auto-discovered — no Makefile change
  needed for x86_64
- arch/aarch64/random.c is technically picked up by
  \$(wildcard \$(ARCHDIR)/*.c) (line 81) but listed explicitly here
  to make the whitelist self-documenting
- Spec compliance: §0.1 (file scope), §0.2.7 (aarch64 whitelist rule)
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 7"
```

---

## Task 8: QEMU orchestration script

**Files:**
- Create: `tests/scripts/qemu_entropy_modes.sh`

**Interfaces:**
- Consumes: 直接调 `qemu-system-{x86_64,aarch64}` + 真实 UEFI image (aarch64) / `kernel.bin` (x86_64)；通过 whitelisted env vars (`KERNEL_TEST_FORCE_NO_RNDRRS`, `KERNEL_SELFTEST`) 触发 variant build
- Produces: x86_64 3 模式 + aarch64 3 模式 = 6 次启动 PASS/FAIL 矩阵

> **v5 设计变更（review item 5 v4 反馈）**：v4 的 `build_kernel_for_mode()` 直接覆盖 `KERNEL_BUILD_DIR` env（被 profile `:=` 硬编码覆盖）+ 透传未授权 `CFLAGS`（被 `OS01_SUBMAKE_ALLOWED` 拦截）。**v5 改用 Task 3.5 新增的 build system 路径**：调用方只设白名单 env vars (`KERNEL_TEST_FORCE_NO_RNDRRS=1 KERNEL_SELFTEST=1`)，build system 自动产出 `build/<profile>/kernel/weak-selftest/kernel.bin` 与对应 UEFI image，与 regular selftest build 完全隔离。脚本不再做 env 覆盖。

- [ ] **Step 1: 创建 `tests/scripts/qemu_entropy_modes.sh`**

```bash
#!/usr/bin/env bash
# tests/scripts/qemu_entropy_modes.sh — AAGU-5 entropy facade 三档验证编排（v5）
#
# 用法：tests/scripts/qemu_entropy_modes.sh [ARCH] [MODE]
#   ARCH = x86_64 (默认) | aarch64
#   MODE = NONE | WEAK | STRONG | all（默认 all）
#
# 通过白名单 env vars 触发 build system variant 化（Task 3.5）：
#   - KERNEL_SELFTEST=1 → variant=selftest (build/<profile>/kernel/selftest/)
#   - KERNEL_TEST_FORCE_NO_RNDRRS=1 + KERNEL_SELFTEST=1 → variant=weak-selftest
#     (build/<profile>/kernel/weak-selftest/)
# 无任何 env 覆盖 / CFLAGS 透传 / KERNEL_BUILD_DIR 覆盖。

set -euo pipefail

ARCH="${1:-x86_64}"
MODE="${2:-all}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROFILE="${ARCH}-clang"

# 每个 mode 对应一组 (label | qemu_cpu_flag | kernel_build_subdir | extra_make_env)
make_mode_entry() {
    local label="$1" cpu="$2" subdir="$3" extra_env="$4"
    echo "$label|$cpu|$subdir|$extra_env|entropy_quality_selftest_current_mode"
}

declare -a MODES=()
case "$ARCH:$MODE" in
    x86_64:all)
        # v8: KERNEL_SELFTEST=1 → variant=selftest → kernel/selftest/kernel.bin
        # 不能用空 subdir（否则启动 kernel//kernel.bin 不存在）
        MODES=(
            "$(make_mode_entry NONE   qemu64                selftest '')"
            "$(make_mode_entry WEAK   qemu64,+rdrand        selftest '')"
            "$(make_mode_entry STRONG qemu64,+rdrand,+rdseed selftest '')"
        ) ;;
    aarch64:all)
        # aarch64 三模式都跑真实 UEFI image（mk/components/run.mk:153 模板）；
        # NONE / STRONG 用 regular selftest variant；WEAK 用 weak-selftest variant。
        MODES=(
            "$(make_mode_entry NONE   cortex-a53 selftest '')"
            "$(make_mode_entry WEAK   max         weak-selftest 'KERNEL_TEST_FORCE_NO_RNDRRS=1')"
            "$(make_mode_entry STRONG max         selftest '')"
        ) ;;
    *:NONE)
        cpu="$([[ $ARCH == x86_64 ]] && echo qemu64 || echo cortex-a53)"
        MODES=("$(make_mode_entry NONE "$cpu" selftest '')") ;;
    *:WEAK)
        cpu="$([[ $ARCH == x86_64 ]] && echo qemu64,+rdrand || echo max)"
        # v8: x86_64 WEAK uses selftest variant (KERNEL_SELFTEST=1); aarch64 uses weak-selftest + flag
        if [[ $ARCH == x86_64 ]]; then
            MODES=("$(make_mode_entry WEAK "$cpu" selftest '')")
        else
            MODES=("$(make_mode_entry WEAK "$cpu" weak-selftest 'KERNEL_TEST_FORCE_NO_RNDRRS=1')")
        fi ;;
    *:STRONG)
        cpu="$([[ $ARCH == x86_64 ]] && echo qemu64,+rdrand,+rdseed || echo max)"
        MODES=("$(make_mode_entry STRONG "$cpu" selftest '')") ;;
    *) echo "MODE must be NONE | WEAK | STRONG | all (got: $MODE)" >&2; exit 2 ;;
esac

SELFTEST_NAME="entropy_quality_selftest_current_mode"
LOG_DIR="${LOG_DIR:-/tmp/aagu5_entropy_${ARCH}_$$}"
mkdir -p "$LOG_DIR"
trap 'rm -rf "$LOG_DIR"' EXIT

# build_kernel_for_variant：调 build system，whitelisted env vars 让 build 系统
# 自己决定 variant 与 build dir。不覆盖 KERNEL_BUILD_DIR，不透传 CFLAGS。
build_kernel_for_variant() {
    local subdir="$1" extra_env="$2"
    local build_dir="$REPO_ROOT/build/${PROFILE}/kernel/${subdir}"

    # v8: image path 同步 run_qemu() 的 variant-aware 计算 — 避免 build 与 run 分叉
    # 导致 WEAK 重建后 run_qemu 才发现 image 缺失，或反过来。
    local image_subdir=""
    if [[ "$subdir" == "weak-selftest" ]]; then
        image_subdir="/weak-selftest"
    fi
    local image_path="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/aarch64-uefi.img"
    local firmware_path="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/QEMU_EFI.fd"

    local kernel_bin="$build_dir/kernel.bin"
    local needs_build=0
    [[ ! -f "$kernel_bin" ]] && needs_build=1
    # aarch64 需要 UEFI image + firmware
    if [[ "$ARCH" == "aarch64" ]]; then
        [[ ! -f "$image_path" ]] && needs_build=1
        [[ ! -f "$firmware_path" ]] && needs_build=1
    fi
    if [[ "$needs_build" -eq 0 ]]; then
        return 0
    fi

    echo "Building ${subdir} variant for $ARCH..."
    env $extra_env KERNEL_SELFTEST=1 \
        make -C "$REPO_ROOT" PROFILE="$PROFILE" all >/dev/null 2>&1 || true
    if [[ ! -f "$kernel_bin" ]]; then
        echo "ERROR: kernel build failed for $PROFILE variant=$subdir" >&2
        echo "Hint: extra_env was '$extra_env'; check KERNEL_TEST_FORCE_NO_RNDRRS whitelisted" >&2
        return 1
    fi
    if [[ "$ARCH" == "aarch64" ]]; then
        env $extra_env KERNEL_SELFTEST=1 \
            make -C "$REPO_ROOT" PROFILE="$PROFILE" aarch64-uefi >/dev/null 2>&1 || true
        if [[ ! -f "$image_path" ]]; then
            echo "ERROR: aarch64-uefi image build failed at $image_path" >&2
            return 1
        fi
        if [[ ! -f "$firmware_path" ]]; then
            echo "ERROR: QEMU_EFI.fd not found at $firmware_path" >&2
            return 1
        fi
    fi
    return 0
}

# 直接调 QEMU
run_qemu() {
    local kernel_path="$1" cpu="$2" log="$3"
    if [[ "$ARCH" == "x86_64" ]]; then
        timeout 30 qemu-system-x86_64 -M q35 -smp 1 -m 256M \
            -kernel "$kernel_path" -nographic -no-reboot \
            -cpu "$cpu" \
            -append "OS01_KERNEL_SELFTEST=1" \
            > "$log" 2>&1 || true
    else
        # v6: variant-aware image path（与 KERNEL_BUILD_DIR 同步）
        # build_kernel_for_variant() 把 variant 写到 $subdir，image path 同步：
        #   weak-selftest → build/<profile>/image/weak-selftest/aarch64-uefi.img
        #   selftest (or empty) → build/<profile>/image/aarch64-uefi.img
        local image_subdir=""
        if [[ "$subdir" == "weak-selftest" ]]; then
            image_subdir="/weak-selftest"
        fi
        local image="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/aarch64-uefi.img"
        local firmware="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/QEMU_EFI.fd"
        if [[ ! -f "$image" || ! -f "$firmware" ]]; then
            echo "ERROR: aarch64 UEFI image/firmware not found at image${image_subdir}/" >&2
            echo "Hint: build dir is set: <$subdir> expected image=$image" >&2
            return 1
        fi
        # 与 mk/components/run.mk:153 同模板；可选 DTB
        local extra_dtb=""
        if [[ "${AARCH64_UEFI_SMP_DIAGNOSTIC_DTB:-1}" != "0" ]]; then
            local dtb_dir="$LOG_DIR/dtb"
            mkdir -p "$dtb_dir"
            local sparse="$dtb_dir/qemu-virt.dtb.sparse"
            local packed="$dtb_dir/qemu-virt.dtb"
            qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 \
                -machine "dumpdtb=$sparse" -display none -m 256M >/dev/null 2>&1 || true
            if command -v dtc >/dev/null && [[ -f "$sparse" ]]; then
                dtc -I dtb -O dtb -o "$packed" "$sparse" || true
                rm -f "$sparse"
                extra_dtb="-dtb $packed"
            fi
        fi
        timeout 45 qemu-system-aarch64 -M virt,gic-version=2${extra_dtb:+,acpi=off} \
            -cpu "$cpu" -smp 1 -m 256M \
            -drive if=pflash,format=raw,readonly=on,file="$firmware" \
            -drive if=none,file="$image",format=raw,readonly=on,id=disk \
            -device virtio-blk-device,drive=disk \
            $extra_dtb \
            -serial stdio -display none -no-reboot \
            > "$log" 2>&1 || true
    fi
}

fail=0
for entry in "${MODES[@]}"; do
    IFS='|' read -r label cpu subdir extra_env selftest <<< "$entry"
    log="$LOG_DIR/${label}.log"
    kernel_bin="$REPO_ROOT/build/${PROFILE}/kernel/${subdir}/kernel.bin"

    # Build (whitelisted env vars only — no KERNEL_BUILD_DIR or CFLAGS override)
    if ! build_kernel_for_variant "$subdir" "$extra_env"; then
        echo "FAIL: $label — build for variant=$subdir failed"
        fail=1
        continue
    fi

    echo "=== $ARCH $label (cpu=$cpu, variant=$subdir) ==="
    if ! run_qemu "$kernel_bin" "$cpu" "$log"; then
        echo "FAIL: $label — QEMU invocation failed"
        fail=1
        continue
    fi

    # 1. selftest PASS
    if ! grep -qE "\[selftest\] ${SELFTEST_NAME}\.\.\. PASS" "$log"; then
        echo "FAIL: $label — selftest did not PASS"
        tail -10 "$log"
        fail=1
        continue
    fi

    # 2. CSPRNG seed log 匹配 quality 档
    case "$label" in
        NONE)   expect_log="CSPRNG: no hardware entropy source" ;;
        WEAK)   expect_log="CSPRNG: pool seeded WEAK" ;;
        STRONG) expect_log="CSPRNG: pool seeded STRONG" ;;
    esac
    if ! grep -qE "$expect_log" "$log"; then
        echo "FAIL: $label — expected CSPRNG log '$expect_log' not found"
        tail -10 "$log"
        fail=1
        continue
    fi

    # 3. NONE / WEAK 档必须确认"没有 UEFI GetRNG 早返回"
    #    — 否则 aarch64 UEFI firmware 可能通过 EFI_RNG_PROTOCOL 提供 STRONG
    #    entropy，绕过 facade seed 分支。检查日志不含 "pool seeded STRONG from UEFI GetRNG"。
    if [[ "$label" == "NONE" || "$label" == "WEAK" ]]; then
        if grep -qE "CSPRNG: pool seeded STRONG from UEFI GetRNG" "$log"; then
            echo "FAIL: $label — UEFI GetRNG bypassed facade (boot_entropy set unexpectedly)"
            tail -10 "$log"
            fail=1
            continue
        fi
    fi

    # 4. **关键（v6 新增，v8 修正）**：校验启动的 kernel variant 与期望 mode 一致
    #    - WEAK  → KERNEL_VARIANT_NAME=weak-selftest (log: "variant=weak-selftest")
    #    - NONE/STRONG (selftest variant) → KERNEL_VARIANT_NAME=selftest
    #    v8 macro 是 KERNEL_VARIANT_NAME（v6/v7 的 OS01_KERNEL_VARIANT 已废弃）
    case "$label" in
        NONE|STRONG) expect_variant="selftest" ;;
        WEAK)        expect_variant="weak-selftest" ;;
    esac
    if ! grep -qE "CSPRNG: kernel build variant=${expect_variant}\b" "$log"; then
        echo "FAIL: $label — kernel build variant mismatch (expected ${expect_variant})"
        echo "Hint: image may have been built from a different variant; check image${image_subdir}/ path"
        grep "CSPRNG: kernel build variant" "$log" || echo "  (no variant log line found)"
        fail=1
        continue
    fi

    echo "PASS: $ARCH $label"
done

if [[ $fail -eq 0 ]]; then
    echo "=== All modes PASS for $ARCH ==="
fi
exit $fail
```

> **关键 v5 修正**：
> 1. **不再覆盖 `KERNEL_BUILD_DIR`** — 完全靠 Task 3.5 的 profile variant 化决定 build dir
> 2. **不再透传未授权 `CFLAGS`** — 测试宏经 `KERNEL_TEST_FORCE_NO_RNDRRS` 白名单 env var，由 `kernel/Makefile:217 ifdef` 块注入
> 3. **aarch64 WEAK 与 STRONG 用不同 build dir** — `weak-selftest` vs `selftest` variant 完全隔离
> 4. **aarch64 WEAK 真 UEFI image + `-cpu max`** — 走 Task 3.5 weak-selftest variant build 的 UEFI image
> 5. **aarch64 WEAK / NONE 档加 UEFI GetRNG 旁路检查** — 防 firmware EFI_RNG_PROTOCOL 误设 `BOOT_CONTEXT_HAS_BOOT_ENTROPY` 绕过 facade 走 STRONG 早返回

- [ ] **Step 2: chmod +x**

```bash
chmod +x tests/scripts/qemu_entropy_modes.sh
```

- [ ] **Step 3: 跑 x86_64 三模式**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 all 2>&1 | tail -5
./tests/scripts/qemu_entropy_modes.sh x86_64 NONE   2>&1 | tail -10
./tests/scripts/qemu_entropy_modes.sh x86_64 WEAK   2>&1 | tail -10
./tests/scripts/qemu_entropy_modes.sh x86_64 STRONG 2>&1 | tail -10
./tests/scripts/qemu_entropy_modes.sh x86_64 all    2>&1 | tail -15
```

期望：四段 `PASS: x86_64 NONE/WEAK/STRONG`，最后 `=== All modes PASS for x86_64 ===`。

- [ ] **Step 4: 跑 aarch64 三模式**

```bash
make PROFILE=aarch64-clang aarch64-uefi 2>&1 | tail -5    # 首次 build regular selftest + UEFI
./tests/scripts/qemu_entropy_modes.sh aarch64 NONE   2>&1 | tail -10
# WEAK 触发 Task 3.5 变体 build：KERNEL_TEST_FORCE_NO_RNDRRS=1 + KERNEL_SELFTEST=1
# → build/aarch64-clang/kernel/weak-selftest/ + 对应 UEFI image
./tests/scripts/qemu_entropy_modes.sh aarch64 WEAK   2>&1 | tail -10
./tests/scripts/qemu_entropy_modes.sh aarch64 STRONG 2>&1 | tail -10
./tests/scripts/qemu_entropy_modes.sh aarch64 all    2>&1 | tail -15
```

期望：四段 `PASS: aarch64 NONE/WEAK/STRONG`，最后 `=== All modes PASS for aarch64 ===`。

- [ ] **Step 5: 提交**

```bash
git add tests/scripts/qemu_entropy_modes.sh
git commit -m "test(aagu-5): QEMU orchestration v5 — build system integration

- tests/scripts/qemu_entropy_modes.sh triggers builds via whitelisted
  env vars only (KERNEL_SELFTEST, KERNEL_TEST_FORCE_NO_RNDRRS);
  NO KERNEL_BUILD_DIR override (rejected by profile := override)
  NO CFLAGS passthrough (rejected by OS01_SUBMAKE_ALLOWED whitelist)
- aarch64 WEAK uses build/aarch64-clang/kernel/weak-selftest/
  (variant=weak-selftest from Task 3.5 profile change); STRONG uses
  build/aarch64-clang/kernel/selftest/ — both UEFI images produced
  from same build root via 'make aarch64-uefi' per variant
- Add UEFI GetRNG bypass check for NONE/WEAK modes: log must NOT
  contain 'CSPRNG: pool seeded STRONG from UEFI GetRNG' (otherwise
  firmware EFI_RNG_PROTOCOL would set BOOT_CONTEXT_HAS_BOOT_ENTROPY
  and bypass the facade seed branch — masking actual quality)
- Real QEMU invocation per mk/components/run.mk:153 run-aarch64-uefi
  pattern (pflash firmware + virtio-blk image + optional DTB)
- Spec compliance: §0.2.8, §4.3
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 8"

## Task 9: 写 `docs/random/entropy-quality.md`（运营商向 read-only 索引）

**Files:**
- Create: `docs/random/entropy-quality.md`

> **设计变更理由（v3 评审 item 6）**：v2 Task 9 重新列出 STRONG/WEAK/NONE 定义、来源表、路径接受度矩阵、QEMU/hardware 启用方法、调试技巧 — 这些都是 spec §2 / §3.3 / §4.3 内容的复制粘贴，违反 spec §0.2.1「STRONG/WEAK/NONE 定义集中」与 §0.2.6「plan 不重定 quality」。新版本只放指向 spec 的链接 + 一段"如何用本文件"的话，不复述任何定义。

- [ ] **Step 1: 创建文件**

```markdown
# OS01 entropy quality — operator quick index

> **这是 read-only 索引，不复述 spec 定义。** 任何 STRONG/WEAK/NONE
> 含义、来源映射、控制流规则以 [`docs/arch/entropy-source-facade.md`](../arch/entropy-source-facade.md)
> 为唯一权威。本文件只告诉读者"去看哪一节"。

## 1. 我想了解 X，去看 spec 哪一节

| 我想知道 | spec 节 |
|---|---|
| STRONG / WEAK / NONE 各是什么意思 | [§2](../arch/entropy-source-facade.md#2-quality-标签语义os01-唯一权威) |
| facade 函数签名 + 返回语义 | [§3](../arch/entropy-source-facade.md#3-facade-契约) |
| 各 caller 路径接受哪一档 | [§3.3 caller 契约](../arch/entropy-source-facade.md#33-caller-契约) |
| `random_init()` 怎么用 quality 决定 ready | [§5](../arch/entropy-source-facade.md#5-random_init-决策树aagu-2-衔接) |
| AAGU-2 既有 fail-closed 怎么衔接 | [§5.2 对照表](../arch/entropy-source-facade.md#52-与-aagu-2-既有-fail-closed-路径的对应) |
| AT_RANDOM 怎么走 STRONG-only | [§6](../arch/entropy-source-facade.md#6-at_random-路径新增-aagu-5-交付物-3) |
| reseed 在 NONE 时行为 | [§7](../arch/entropy-source-facade.md#7-reseed-策略与-aagu-2-对齐) |
| QEMU 加哪些 `-cpu` flag 能触发哪一档 | [§4.3 跨架构降级矩阵](../arch/entropy-source-facade.md#43-跨架构降级矩阵) |
| 真硬件 / KVM 上启用 RDSEED/RNDRRS | [§4.3 真硬件行](../arch/entropy-source-facade.md#43-跨架构降级矩阵) |
| 实施三档测试的 QEMU 编排 | [plan Task 8](../superpowers/plans/2026-09-23-aagu-5-entropy-facade.md#task-8-qemu-orchestration-script) |

## 2. 调试入口

- 启动 log：`make PROFILE=x86_64-clang run | grep CSPRNG` 看 pool 种子来源行
  - `CSPRNG: pool seeded STRONG from UEFI GetRNG` — UEFI 通道
  - `CSPRNG: pool seeded STRONG (RDSEED/RNDRRS)` — 硬件 raw 熵
  - `CSPRNG: pool seeded WEAK (RDRAND/RNDR DRBG); AT_RANDOM will fail-closed` — DRBG only
  - `CSPRNG: no hardware entropy source ... pool not ready` — NONE（dev 路径）

## 3. 不要做的事（与 spec §0.2 对齐）

- 不要在本文件再定义 STRONG/WEAK/NONE 或重新裁决分级 — 改 spec 唯一源
- 不要把 x86 RDRAND 路径复制到 aarch64 当作实现完成 — 各自独立评审
- 不要让 quality 沦为装饰性 facade — random_init() 必须 switch on q
```

- [ ] **Step 2: 提交**

```bash
git add docs/random/entropy-quality.md
git commit -m "docs(random): entropy quality operator index (read-only, links only)

- docs/random/entropy-quality.md reduced from a duplicate
  definition/acceptance matrix to a read-only index that points
  readers at the spec sections. No redefinition of STRONG/WEAK/
  NONE tags, no re-statement of acceptance tables, no re-citation
  of sources — those all live in docs/arch/entropy-source-facade.md
- Includes a 'want to know X? see spec §Y' table for operator
  lookup, a debug-log line catalog, and the §0.2 prohibitions
  as a 'don't do' list
- Spec compliance: §0.2.1 (definition centralized),
  §0.2.6 (no re-decision), AAGU-5 §交付物 4 (operator-facing
  summary, satisfied via index without becoming a 2nd source)
- Plan ref: docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md
  Task 9"

---

## Self-Review

### 1. Spec 覆盖

| Spec 节 | 实施 task |
|---|---|
| §0.1 范围（5 改 1 删 + 6 文件） | Task 1 (header) / 2 (x86_64) / 3 (aarch64) / 4 (random.c) / 5 (task.c) / 7 (Makefile) |
| §0.2.1（STRONG/WEAK/NONE 集中） | Task 1 header 唯一来源；Task 2/3/4/5/6 注释引用 spec §2；**Task 9 缩为 read-only 索引**（v3 修复 item 6） |
| §0.2.2（不复制 RDRAND 路径） | Task 2/3 各自独立 inline asm |
| §0.2.3（quality 真驱动控制流） | Task 4 三分支 switch；Task 5 STRONG-only helper + 错误传播 |
| §0.2.4（不重定义 AAGU-2） | Task 4 §5.2 对照表 |
| §0.2.5（不掺 cycle-counter） | Task 2/3 第 3 步 memset；Task 4 reseed NONE→drop ready |
| §0.2.6（plan 不重定 quality） | Task 3 commit message 显式声明 RNDR=WEAK 是 spec 决定；**Task 9 不再复述定义**（v3 修复 item 6） |
| §2.1 STRONG 源 | Task 2 RDSEED path；Task 3 RNDRRS path；Task 4 UEFI 早返回 |
| §2.2 WEAK 源 | Task 2 RDRAND path；Task 3 RNDR path |
| §2.3 NONE memset | Task 2/3 第 3 步；Task 4 reseed NONE |
| §3 facade 契约 | Task 1 header 接口签名 |
| §4.1 x86_64 试/退 | Task 2 |
| §4.2 aarch64 试/退 | Task 3 |
| §4.3 跨架构降级矩阵 | **Task 8 脚本直接调 QEMU 跑三模式**（v3 修复 item 5） |
| §5.1 random_init 三分支 | Task 4 |
| §5.2 AAGU-2 对照 | Task 4 commit message 引用 |
| §6 AT_RANDOM STRONG-only | **Task 5 改 setup_user_stack 签名 + 32B 缓冲 + 错误传播**（v3 修复 item 5） |
| §7.1/§7.2 reseed | Task 4 reseed 重写 |
| §7.3 reseed NONE 差异 | Task 4 commit message 显式说明 |
| AAGU-5 §交付物 4 entropy-quality.md | **Task 9 改为 read-only 索引**（v3 修复 item 6） |

**v3 修复后无覆盖缺口**。

### 2. Placeholder 扫描

搜索 task 步骤中是否有"TBD"/"implement later"/"similar to Task N"：
- 无。所有 step 含代码块或具体 shell 命令。
- Step 1/2/3/... 编号清晰；每步含运行命令与期望输出。
- Task 8 脚本 aarch64 段有"骨架仅占位"标注 + 文档化退路（spec §4.3 对称论证） — 这是合法 deferral，非 placeholder。

### 3. Type / signature 一致性

| 引用点 | 定义点 | 一致？ |
|---|---|---|
| `arch_random_get_entropy(uint8_t [32], arch_entropy_source_t *)` | Task 1 header → Task 2/3 实现 → Task 4 调用 → Task 6 测试 | ✅ |
| `arch_random_get_strong(uint8_t [32])` | Task 1 header → Task 2/3 实现 → Task 5 调用（**32B 缓冲，前 16B 拷 KSTACK**） | ✅ v3 修复越界 |
| `arch_entropy_source_t` 枚举值 | Task 1 header（NONE/WEAK/STRONG）→ Task 2/3/4/6 引用 | ✅ |
| `random_is_ready()` | 既有 AAGU-2（未改）→ Task 6 测试 | ✅ |
| `setup_user_stack()` 返回类型 | **v3 验证：`static void` at `kernel/sched/task.c:1140`，Task 5 Step 2 改 `static int`** | ✅ 已修正 |
| `spawn_user_task()` 接受 `int64_t` | `kernel/sched/task.c:1240` → Task 5 Step 5 返回 `-EAGAIN` | ✅ |
| `sys_exec()` 接受 `int64_t` | `kernel/sched/task.c:1418` → Task 5 Step 6 返回 `-EAGAIN` | ✅ |
| `task_selftest_auxv_probe()` 接受 `int` | `kernel/sched/task.c:~2296` → Task 5 Step 7 返回 `-1` | ✅ |
| Task 6 selftest 名 | `entropy_quality_selftest_current_mode`（Task 6 Step 2 注册）→ Task 8 脚本 grep 同一名 | ✅ v3 修复名错位 |

**v3 已消除 v2 的所有 ⚠️ 标记**。

### 4. v3 评审 item 5/6 修复清单

| v3 FAIL | 修复位置 |
|---|---|
| Task 5: `uint8_t at_random_buf[16]` 越界 | Task 5 Step 3 改 32B 缓冲 |
| Task 5: `setup_user_stack()` 是 `static void`，`return -1` 编译失败 | Task 5 Step 2 改 `static int`；Step 4 加 `return 0;` |
| Task 5: 三处 caller 不接返回值 | Task 5 Step 5/6/7 各加 if-check + 错误传播 |
| Task 6: 4 个互斥 selftest，runner 全跑必然 3 失败 | Task 6 Step 1-2 改为运行时 `q` 检测 + 单一 selftest |
| Task 8: `set -u` + 未定义 `$MODE` | Task 8 Step 1 `MODE="${2:-all}"` 在脚本开头赋值 |
| Task 8: 冒号分隔 + `CSPRNG:` 字段错位 | Task 8 Step 1 改 `\|` 分隔（无特殊字符） |
| Task 8: grep `*_selftest` 但注册名是 `entropy_quality_*` | Task 8 Step 1 grep `entropy_quality_selftest_current_mode`（Task 6 新名） |
| Task 8: `make run` 不接 `QEMU_EXTRA_CPU_FLAGS` | Task 8 Step 1 直接调 `qemu-system-*` 二进制 |
| aarch64 WEAK 档 "deferred" | Task 8 Step 1 用 `-cpu cortex-a76` 试 RNDR-only；Step 4 文档化退路 |
| Task 9: 复述 STRONG/WEAK/NONE 定义、来源、接受度矩阵 | Task 9 Step 1 缩为 read-only 索引 + spec 链接表 |

### 5. v4 评审 item 5 修复清单（Task 8 aarch64 仍不可执行 + WEAK deferred + 资源泄漏）

| v4 FAIL | 修复位置 |
|---|---|
| Task 8 aarch64 分支只打印 `ERROR` 不可执行 | Task 8 Step 1 重写 `run_qemu()` aarch64 分支：用真实 `aarch64-uefi` 镜像 + pflash firmware + virtio-blk + 可选 DTB（与 `mk/components/run.mk:153 run-aarch64-uefi` 同模板），不再"骨架/占位" |
| aarch64 WEAK 仍 deferred，"间接覆盖"不算验证 | Task 3 Step 1（v4 增量）加 `#ifdef AAGU5_TEST_FORCE_NO_RNDRRS` build flag；Task 8 Step 1 `run_qemu()` + `build_kernel_for_mode()` 用 flag 触发独立 build，强制 `has_rndrrs=false` 跑 `-cpu max`（确定有 RNDR），无需 QEMU 暴露"仅-RNDR 无-RNDRRS"model |
| aarch64 STRONG 依赖 "cortex-a76 是否真模拟 RNDR" 的不可重复假设 | Task 8 Step 1 改用 `-cpu max`（确定暴露 RNDR+RNDRRS），与 AAGU-5.2 调研 §2.1 "cortex-a53/cortex-a72/cortex-a76 默认不模拟 RNDR" 矛盾化解 |
| Task 5 三处 caller `-EAGAIN` 返回前未释放已分配资源 | Task 5 Step 5（spawn_user_task）加清理块：list_del + files_put + fpu_area_free + free_pages + vmm_free_user_map + kfree×3；Step 6（sys_exec）加清理块：free_pages + vmm_free_user_map + kfree。task_selftest_auxv_probe 仅用静态 buffer 无需清理。 |

### 6. v4 验收契约（Task 8 三模式 × 两架构 = 6 次启动）

| Arch | MODE | QEMU -cpu | build flag | 期望 CSPRNG log |
|---|---|---|---|---|
| x86_64 | NONE | qemu64 | (none) | `CSPRNG: no hardware entropy source` |
| x86_64 | WEAK | qemu64,+rdrand | (none) | `CSPRNG: pool seeded WEAK` |
| x86_64 | STRONG | qemu64,+rdrand,+rdseed | (none) | `CSPRNG: pool seeded STRONG` |
| aarch64 | NONE | cortex-a53 | (none) | `CSPRNG: no hardware entropy source` |
| aarch64 | WEAK | max | `KERNEL_TEST_FORCE_NO_RNDRRS=1` | `CSPRNG: pool seeded WEAK` |
| aarch64 | STRONG | max | (none) | `CSPRNG: pool seeded STRONG` |

每档同时断言 selftest `entropy_quality_selftest_current_mode... PASS`（Task 6 单一 mode-aware 测试）。NONE/WEAK 档额外断言 log 不含 `CSPRNG: pool seeded STRONG from UEFI GetRNG`（防 firmware EFI_RNG_PROTOCOL 旁路）。

### 7. v5 评审 item 5 修复清单（build system 接入 + files API + UEFI 旁路检查）

| v5 FAIL | 修复位置 |
|---|---|
| Task 8 `KERNEL_BUILD_DIR=...` 被 profile `:=` 硬编码覆盖（`mk/profiles/aarch64-clang.mk:13`） | **Task 3.5** Step 3/4 加 `KERNEL_VARIANT=weak-selftest` 分支；profile 自动基于 variant 算 `KERNEL_BUILD_DIR`，env 不需覆盖。 |
| Task 8 `CFLAGS=$CFLAGS -D...` 被 `OS01_SUBMAKE_ALLOWED` 白名单拦截（`mk/project.mk:78`） | **Task 3.5** Step 1 加 `KERNEL_TEST_FORCE_NO_RNDRRS` 到白名单；Step 2 在 `kernel/Makefile` 加 `ifdef` 块（mirrors `KERNEL_SELFTEST` line 217 模式）。 |
| aarch64 WEAK 与 STRONG 共享 build dir，可能污染 | **Task 3.5** Step 4 加 aarch64 variant 支持（之前只有 x86_64 有 variant），`build/aarch64-clang/kernel/weak-selftest/` 与 `selftest/` 隔离。 |
| aarch64 `aarch64-uefi` re-build 时未传 KERNEL_SELFTEST 与测试宏 | **Task 8** Step 1 `build_kernel_for_variant()` 在 aarch64 路径重跑 `aarch64-uefi` target，env 透传 `KERNEL_SELFTEST=1` + `KERNEL_TEST_FORCE_NO_RNDRRS=1`，确保 UEFI image 内核含 selftest + force macro。 |
| `files_put()` 不存在（`kernel/include/fs/file.h:138-148` 真实 API 是 `files_unpin`/`files_put_file`） | **Task 5** Step 5 改用 `files_unpin(tsk->files)`；并在 `spin_unlock_irqrestore(task_list_lock)` **之后**调用（遵守 file.h:140-142 持锁约束）。 |
| aarch64 UEFI firmware 可能通过 EFI_RNG_PROTOCOL 触发 STRONG 早返回，掩盖 WEAK/NONE 测试 | **Task 8** Step 1 在 run_qemu + 断言后加额外检查：NONE/WEAK 档 log 不含 `CSPRNG: pool seeded STRONG from UEFI GetRNG`。 |

### 8. v5 修订后 acceptance（强化）

每档 3 项断言（v5 新增第 3 项）：

1. selftest PASS：`[selftest] entropy_quality_selftest_current_mode... PASS`
2. CSPRNG seed log 匹配档位（NONE / WEAK / STRONG 对应不同 log）
3. NONE/WEAK 档额外：log **不**含 `CSPRNG: pool seeded STRONG from UEFI GetRNG`（firmware 不应绕过 facade）

aarch64 WEAK 档额外验证：build dir = `build/aarch64-clang/kernel/weak-selftest/`（不是 selftest/），证明 KERNEL_TEST_FORCE_NO_RNDRRS 走 variant 化路径生效。

### 9. v6 评审 item 5 修复清单（aarch64 UEFI image variant 隔离 + variant 校验）

| v6 FAIL | 修复位置 |
|---|---|
| Task 8 aarch64 image path 是 `build/${PROFILE}/image/aarch64-uefi.img` 共享路径，WEAK build 覆盖 STRONG/NONE image；STRONG 因 image "存在" 跳过 rebuild，启动错位 image | **Task 3.5 Step 4.5** `mk/targets/aarch64.mk:5` 改为 `$(BUILD_DIR)/image$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))/aarch64-uefi.img`，与 KERNEL_BUILD_DIR variant 同步 |
| Task 8 aarch64 启动 image 不一定与期望 mode 对应 | **Task 8** `run_qemu()` aarch64 分支：根据 `$subdir` 选 image 路径（weak-selftest 时 `/image/weak-selftest/`），启动 image 与 build kernel 同 variant |
| 测试启动后无法验证 image 内核 variant 与期望 mode 一致 | **Task 3.5 Step 5** `kernel/Makefile` 加 `ifdef KERNEL_VARIANT / ALL_CFLAGS += -DOS01_KERNEL_VARIANT=$(KERNEL_VARIANT)`；**Task 4** `random_init()` UEFI 早返回分支加 `printk("CSPRNG: kernel build variant=%s", OS01_KERNEL_VARIANT)`；**Task 8** PASS 检查加第 4 项断言：log 含 `CSPRNG: kernel build variant=<expected>`（NONE/STRONG→selftest；WEAK→weak-selftest） |

### 10. v6 修订后 acceptance（4 项断言）

每档 4 项断言（v6 新增第 4 项）：

1. selftest PASS：`[selftest] entropy_quality_selftest_current_mode... PASS`
2. CSPRNG seed log 匹配档位
3. NONE/WEAK 档额外：log 不含 UEFI GetRNG 早返回
4. **aarch64 三档额外**：log 含 `CSPRNG: kernel build variant=<expected>` — 证明启动的 image 内核 variant 与 build script 选择的 mode 一致（防错位 image）

aarch64 WEAK 档额外验证：build dir = `build/aarch64-clang/image/weak-selftest/`，与 STRONG/NONE 的 `image/` 完全分离。

### 11. v7 评审 item 5 修复清单（v6 三处缺陷）

| v7 FAIL | 修复位置 |
|---|---|
| v6 只改 `mk/targets/aarch64.mk` 的 `AARCH64_UEFI_DISK` 路径，但 `mk/components/image.mk:154` 的 image 生成规则 target 仍硬编码 `$(BUILD_DIR)/image/aarch64-uefi.img`，导致 WEAK build "No rule to make target" | **Task 3.5 Step 4.5**（v7 修订）：`mk/components/image.mk:154` 改为 `$(AARCH64_UEFI_DISK): ...` — recipe 用 `$@` 自动展开为 variant 路径。两处共用同一变量避免再次脱钩。 |
| v6 的 `-DOS01_KERNEL_VARIANT=$(KERNEL_VARIANT)` 展开为 `-DOS01_KERNEL_VARIANT=weak-selftest` — 编译器把 `OS01_KERNEL_VARIANT` 视作未声明标识符（或减法表达式 `weak - selftest`），`log_info("%s", OS01_KERNEL_VARIANT)` 编译失败 | **Task 3.5 Step 5**（v7 修订）：改用 `-DOS01_KERNEL_VARIANT=\"$(KERNEL_VARIANT)\""` — make 展开为 `-DOS01_KERNEL_VARIANT="weak-selftest"`，shell 视 `"` 为字面 `"`，clang 收到 `-DOS01_KERNEL_VARIANT="weak-selftest"`，定义宏为 C 字符串字面量 `"weak-selftest"`。 |
| v6 的 variant log 只在 UEFI GetRNG 早返回分支（STRONG via UEFI）打印；但 Task 8 要求 NONE/WEAK 不得进此分支，于是 NONE/WEAK 永远不打印 variant → 第 4 项断言必失败 | **Task 4**（v7 修订）：把 variant log 移到 `random_init()` 入口处（在 pool_blk/pool_bytes_since_reseed 重置之后、所有分支判断之前）。三档 STRONG/WEAK/NONE 都执行一次，selftest 也包含 NONE 路径自然覆盖。 |

---

### 12. v8 评审 item 5 修复清单（v7 残留缺陷）

| v8 FAIL | 修复位置 |
|---|---|
| v7 的 `-DOS01_KERNEL_VARIANT="..."` Makefile 引号在 shell 路径中被移除，clang 仍收 token；预处理宏也不成为符号（v7 文档说用 `nm | grep` 是错的） | **Task 3.5 Step 5**（v8 重写）：彻底放弃 Makefile 端引号注入，改用 `-DKERNEL_VARIANT_NAME=$(KERNEL_VARIANT)`（无引号 token）+ C 端 `OS01_STRINGIFY()` 双层宏 stringify。Sanity check 改用 `strings kernel.bin | grep weak-selftest` + `clang -E -dM` |
| Task 8 `build_kernel_for_variant()` 检查 image 用共享路径，与 `run_qemu()` 的 variant-aware 计算分叉 | **Task 8** `build_kernel_for_variant()`（v8 重写）：按 `$subdir` 计算 image/firmware 路径，与 `run_qemu()` 用同一套 `image_subdir` 逻辑 |
| x86_64 `all` 矩阵三个 entry `subdir` 为空，但 `KERNEL_SELFTEST=1` 产出 `kernel/selftest/kernel.bin`，启动 `kernel//kernel.bin` 找不到 | **Task 8** `x86_64:all` 段：subdir 改 `selftest`。`*:WEAK` 单模式分支：x86_64 用 selftest variant（无需 force-no-RNDRRS flag），aarch64 用 weak-selftest + flag |



### 13. v9 评审 item 5 修复清单（v8 残留缺陷）

| v9 FAIL | 修复位置 |
|---|---|


### 14. v10 评审 item 5 修复清单（v9 残留缺陷）

| v10 FAIL | 修复位置 |
|---|---|
| v9 仍残留 v7 `OS01_KERNEL_VARIANT="..."` Makefile 注入说明 + 误称"应能找到符号"的 strings sanity check（strings 找字符串字面量不是符号），与 v8 `KERNEL_VARIANT_NAME` + `OS01_STRINGIFY()` 方案矛盾 | **Task 3.5 Step 4.5**（v10 修订）：删除 v7 两段残留（OS01_KERNEL_VARIANT 引号注入说明 + 误称"应能找到符号"的 strings sanity check），保留 v8 `clang -E -dM` + `strings | grep weak-selftest`（明确"字符串字面量不是符号"） |
| Task 8 注释 `OS01_KERNEL_VARIANT=weak-selftest/selftest` 描述与 v8 后实际宏名 `KERNEL_VARIANT_NAME` 矛盾 | **Task 8** Step 1 注释：改用 `KERNEL_VARIANT_NAME` 描述 |

| v8 image.mk diff 把续行反斜杠写成行内 `-` / `+`，不是合法 Makefile 规则 — `+` 会被视作依赖目标名 | **Task 3.5 Step 4.5**（v9 修订）：diff 改为两行规则，续行 `\\` 在首行末尾，下一行以 tab 缩进 |
| Task 3.5 Step 7 的 `git add` 遗漏 `mk/targets/aarch64.mk` + `mk/components/image.mk`，导致 variant image 隔离改动不进入 commit | **Task 3.5 Step 7**（v9 修订）：`git add` 补齐两个 make 文件，多行 `\\` 续行 |
| Task 3.5 仍保留 v7 已废弃的 `OS01_KERNEL_VARIANT="..."` Makefile 注入说明 + 误称"应能找到符号"的 strings sanity check（v8 已改为 `KERNEL_VARIANT_NAME` token + `OS01_STRINGIFY()` 双层宏） | **Task 3.5 Step 4.5**（v10 修订）：删除 v7 残留的两段（OS01_KERNEL_VARIANT 引号注入说明 + 误称"应能找到符号"的 strings sanity check），保留 v8 `clang -E -dM` + `strings | grep weak-selftest`（明确"字符串字面量不是符号"）|

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

> **Note**：AAGU-5 父契约期望实施由 AAGU-5.6 (executor) agent 承担，本 plan 是 planer 产物（planer 不实施代码 — 见全局身份约束）。executor 接 plan 后按 Subagent-Driven 或 Inline 自行选择。
# OS01 arch-neutral entropy facade — spec

| 字段 | 值 |
|---|---|
| Spec ID | `entropy-source-facade` |
| Issue | AAGU-5（parent），AAGU-5.3（facade 设计） |
| 状态 | Draft v2（针对 AAGU-5.5 评审的最小修复） |
| 最后更新 | 2026-09-23 |
| 适用范围 | `kernel/include/arch/random.h`（facade） + `kernel/arch/{x86_64,aarch64}/random.c`（strong override）+ `kernel/random/random.c`（使用方） + `kernel/sched/task.c`（AT_RANDOM 调用方） |
| 输入 | AAGU-5 父 issue body §交付物 1-4；AAGU-5.1 x86_64 调研 (`docs/random/entropy-source-survey-x86_64.md`)；AAGU-5.2 aarch64 调研 (`docs/random/entropy-source-survey-aarch64.md`)；AAGU-5 合并稿 (`docs/random/entropy-source-survey-merged.md`)；AAGU-2 fail-closed (`kernel/random/random.c` §1, `kernel/include/core/bootinfo.h` BOOT_CONTEXT_HAS_BOOT_ENTROPY) |
| 后续读者 | AAGU-5.6（双架构 strong override 实现） / AAGU-5.7（三档 QEMU 测试） |

> **本文件是 designer 产物**：定义 facade 接口、quality 标签语义、控制流决策、与 AAGU-2 衔接、以及禁止条款。**不实施代码改造**；实施计划见 `docs/superpowers/plans/2026-09-23-aagu-5-entropy-facade.md`。

---

## 0. 范围与原则

### 0.1 范围

- `kernel/include/arch/random.h`：arch-neutral facade，定义 `arch_entropy_source_t { STRONG, WEAK, NONE }`、`arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality)` 与 `arch_random_get_strong(uint8_t out[32])`
- `kernel/arch/x86_64/random.c`（**新建**）：x86_64 实现 — RDSEED=STRONG / RDRAND=WEAK / 全部失败=NONE
- `kernel/arch/aarch64/random.c`（**新建**）：aarch64 实现 — RNDRRS=STRONG / RNDR=WEAK / 全部失败=NONE
- `kernel/random/random.c`（**改**）：删除 `mix_hw_entropy()` 内联 asm + cycle-counter 静态 fallback，改用 facade，三分支显式决策 `random_ready`
- `kernel/sched/task.c`（**改**）：AT_RANDOM 改用 `arch_random_get_strong()`，WEAK/NONE 返回错误
- `kernel/include/arch/random.h`（**改**）：删除 aarch64 inline stub（`rdrand64`/`rdseed64` 占位）
- `kernel/include/arch/x86_64/random.h`（**改**）：删除 inline `rdrand64`/`rdseed64`（迁到 `kernel/arch/x86_64/random.c`）
- `kernel/Makefile`（**改**）：aarch64 whitelist 加 `random/random.c`（x86_64 用 wildcard 自动收纳）

### 0.2 禁止条款（**强约束**）

1. **STRONG/WEAK/NONE 的定义集中在 `kernel/include/arch/random.h` 一处**，禁止散落到 `kernel/arch/{x86_64,aarch64}/random.c`、调用方、或文档其它地方再定义。架构实现只负责"我能产出哪个 quality + 是否填满 32B"，不重新定义标签含义。
2. **禁止把 x86_64 RDRAND/RDSEED 路径机械复制到 aarch64 当作"实现完成"**。RNDR/RNDRRS 的 inline asm（MSR/MRS encoding、`"cc"` clobber、NZCV 判定）与 RDRAND/RDSEED 的 inline asm（CALL/CFC clobber、CF 判定）语义不互通；每架构单独评审。
3. **禁止把 quality 标签沦为"装饰性 facade"**。`random_init()` 必须由 `q` 的显式三分支决定 `random_ready`；AT_RANDOM 必须拒绝 WEAK/NONE。质量标签不参与控制流，等于没有 facade。
4. **禁止重新定义 AAGU-2 fail-closed 语义**。`random_init` 的 STRONG/WEAK/NONE→ready 映射必须显式说明与 `kernel/random/random.c` 既有 `mix_hw_entropy()` 行为的对应关系；不与 AAGU-2 已落地的"无硬件即 fail-closed"路径冲突。
5. **禁止保留 cycle-counter/jitterentropy 死代码**。两份调研都判断 jitterentropy 实现成本高 + TCG 精度不够；NONE 路径必须 `memset(out, 0, 32)` 后返回，不掺 cycle-counter 字节。
6. **禁止在 plan/实施文档里重新裁决 quality 等级**。本 spec §2 是 OS01 范围内 STRONG/WEAK/NONE 定义的**唯一来源**；plan 与代码注释引用 §2 即可，不在 plan/task 内再做一次"RNDR 是 STRONG 还是 WEAK"的裁定。

### 0.3 与 AAGU-5 父契约的关系

| 父契约条目 | 本 spec 节 | 一致性 |
|---|---|---|
| `kernel/include/arch/random.h` facade + `arch_entropy_source_t` | §1, §3 | 一致（新增 `arch_random_get_strong`） |
| `arch_random_get_entropy()` 一次性 32B | §3 | 一致 |
| x86_64 实现位于 `kernel/arch/x86_64/random.c` | §4.1 | 一致（新建） |
| aarch64 实现位于 `kernel/arch/aarch64/random.c` | §4.2 | 一致（新建） |
| 删除 `mix_hw_entropy()` 内联 asm + 静态 fallback | §5 | 一致（替换为 facade 调用） |
| AT_RANDOM 仅 STRONG，否则 fail-closed | §6 | 一致 |
| 三档验收：QEMU 默认 / 加 flag / 真硬件 | §4.3 | 一致（spec 不规定测试，由 AAGU-5.7 plan 实施） |

### 0.4 与 AAGU-2 的边界（**再次强调**）

AAGU-2 已落地（commit `8931cab`，`kernel/random/random.c`）：

- `mix_hw_entropy()` 在 4 次 `rdrand64`/`rdseed64` 调用里，只要有 ≥1 次成功（`had_hw > 0`）就 `return true`；全部失败（4 次都退到 `arch_cycle_counter() ^ jiffies`）就 `return false`
- `random_init()` 据此设 `random_ready=true/false`
- `get_random_bytes()` 在 `!random_ready` 时 memset 请求 buffer 为 0（fail-closed）

本 spec 在 AAGU-2 之上做的**唯一结构性改动**：

1. 把"≥1 次 RDRAND/RDSEED 命中"细化为**STRONG**（4 次都是 RDSEED/RNDRRS）与 **WEAK**（4 次都是 RDRAND/RNDR）两档 — 各自与 AAGU-2 的对应见 §5.1
2. **新增** AT_RANDOM 专用 STRONG-only 路径（`arch_random_get_strong`）— 这是 AAGU-5 §交付物 3 的父契约要求，AAGU-2 不曾有

**本 spec 不修改**：`pool_key` 状态、`reseed()` 的 ChaCha20 nonlinear 混合、`get_random_bytes()` 的 fail-closed 语义、UEFI GetRNG 路径。

---

## 1. 设计目标 / 非目标

### 1.1 设计目标

- **质量分级可被控制流读取**：`arch_random_get_entropy()` 返回的 `quality` 必须真影响 `random_init()` 的 ready 决策与 AT_RANDOM 调用方的接受/拒绝；不是装饰
- **跨架构对称**：x86_64 与 aarch64 的 STRONG/WEAK/NONE 映射遵循同一套定义（不各自发明）
- **向后兼容 AAGU-2**：`mix_hw_entropy()` 的"≥1 次命中 → ready=true"映射到新 spec 的"STRONG 或 WEAK → ready=true"；不修改既有控制流
- **可测试**：每个 quality 档在 QEMU + 真硬件上都有可重复触发 + 可断言路径

### 1.2 非目标

- 不引入 jitterentropy 软件熵源（两份调研一致"先不做"）
- 不引入 `/dev/hwrng` / 用户态 entropy 守护 / 网络熵源
- 不修改 CSPRNG 算法（ChaCha20 状态机）、pool 锁、reseed interval、`/dev/random` 写入 mix — 这些都属 AAGU-2
- 不实现 RNDR → STRONG 的捷径（即便 `RNDR` 在 ARM ARM 文档里描述为"真随机数"，本 spec 仍按 NIST 800-90B 严格分级划为 WEAK — 见 §2.2）

---

## 2. Quality 标签语义（**OS01 唯一权威**）

> 本节是 §0.2 禁止条款 1 所指的"STRONG/WEAK/NONE 定义集中处"。任何架构实现、调用方、plan、文档引用此处，不重新定义。

### 2.1 STRONG（**raw entropy source**）

**OS01 定义**：输出 32 字节全部来自**可被密码学验证为 raw entropy** 的硬件源。

**OS01 当前承认的 STRONG 源**：

| 架构 | 指令/通道 | 验证机制 |
|---|---|---|
| x86_64 | `RDSEED` | CPUID L7.S0.EBX[18]=1；调用 `rdseed %0` 直到 CF=1 |
| aarch64 | `RNDRRS` | `ID_AA64ISAR0_EL1.RNDR` bits[63:60]=0b0001；调用 `mrs RNDRRS` 直到 NZCV=eq |
| **任意** | UEFI `EFI_RNG_PROTOCOL` (`GetRNG`) | 由 bootloader (`boot/uefi/main.c:119`) 拉取 32B 后写入 `boot_entropy`，kernel 启动时已就绪 |

**失败语义**：所选指令未实现或 10 次重试未成功 → 切下一档（详见 §4）。

### 2.2 WEAK（**DRBG output**）

**OS01 定义**：输出 32 字节全部来自**已上电的硬件 DRBG**，但**不是**可被密码学验证的 raw entropy — 即 NIST SP 800-90A 定义的 DRBG 输出。

**OS01 当前承认的 WEAK 源**：

| 架构 | 指令/通道 |
|---|---|
| x86_64 | `RDRAND`（CPUID L1.ECX[30]=1；`rdrand %0` 直到 CF=1）|
| aarch64 | `RNDR`（`ID_AA64ISAR0_EL1.RNDR` bits[63:60]≥0b0001；`mrs RNDR` 直到 NZCV=eq）|

**与 NIST 800-90B 的对齐**：RNDR 与 RDRAND 均为硬件实现的 SP 800-90A DRBG（前者 AES-128 后者未公开），其输出**不直接**可被攻击者预测但**不可独立验证**为 raw entropy；故 STRONG-only 路径必须拒绝它们。

**失败语义**：所选指令未实现或 10 次重试未成功 → 切下一档或返回 NONE（详见 §4）。

**注**：RNDR/RDRAND 在父 issue body §交付物 1 的速记中曾标 STRONG — 那是 AAGU-5 父契约初稿，本 spec 在 AAGU-5.3 阶段按合并稿 `docs/random/entropy-source-survey-merged.md` §5.1 **Option A（严格分级）** 重裁定为 WEAK。父契约交付物 1 在本 spec 落地后视为已被本节解释覆盖。

### 2.3 NONE（**no hardware entropy**）

**OS01 定义**：32 字节 buffer **未**填入任何硬件熵；`memset(out, 0, 32)` 后返回。

**当前状态**：x86_64 默认 `-cpu qemu64`（无 RDRAND/RDSEED）+ aarch64 默认 `-cpu cortex-a53`（无 RNDR/RNDRRS）— 详见 `docs/random/entropy-source-survey-merged.md` §6.1。

**禁止**：NONE 路径**不得**掺入 cycle-counter 字节充当"伪熵"。AAGU-2 的 `mix_hw_entropy()` 在 4 次 RDRAND/RDSEED 都失败时退到 `arch_cycle_counter() ^ jiffies`，那是 AAGU-2 fail-closed 落地前的过渡行为；本 spec 起明确剔除。

---

## 3. Facade 契约

### 3.1 接口签名

```c
// kernel/include/arch/random.h

typedef enum {
    ARCH_ENTROPY_NONE  = 0,
    ARCH_ENTROPY_WEAK  = 1,
    ARCH_ENTROPY_STRONG = 2,
} arch_entropy_source_t;

/* 一次性产出 32B entropy + 质量标签。
 *
 *   out[32]   成功时填满 32B（STRONG 或 WEAK），失败时 memset(out, 0, 32)。
 *   *quality  成功时设 ARCH_ENTROPY_STRONG / ARCH_ENTROPY_WEAK；
 *             失败时设 ARCH_ENTROPY_NONE。
 *   return    true iff *quality != ARCH_ENTROPY_NONE（即填满了 32B）。
 *
 * 失败语义：调用方收到 false 时 out 已被 memset 0，可直接用。
 *
 * IRQ 安全：是的（atomic block 内调用；不持锁、不分配、不记录到 log）。
 *
 * Boot 时序：在 random_init() 之前调用合法（arch 实现各自测一次）。
 */
bool arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality);

/* AT_RANDOM 专用 helper：拒绝 WEAK/NONE。
 *
 *   out[32]   成功时填满 32B（STRONG）。
 *   return    true iff STRONG entropy 已填入。
 *
 * 语义：失败时 out 状态未定义 — 调用方（task.c）须把 out 当作不可用，
 *       不读 out 内容。
 *
 * 实现：在内部调 arch_random_get_entropy()；若 q != STRONG 则 return false
 *       （out 内容由 facade 保证为 memset 0，但调用方契约不依赖此保证）。
 */
bool arch_random_get_strong(uint8_t out[32]);
```

### 3.2 返回语义对照表

| 内部状态 | `out[32]` 内容 | `*quality` | 返回值 |
|---|---|---|---|
| STRONG 命中（RDSEED / RNDRRS / UEFI GetRNG 已就绪） | 32B 全部来自 raw entropy | `ARCH_ENTROPY_STRONG` | `true` |
| WEAK 命中（RDSEED/RNDRRS 失败但 RDRAND/RNDR 成功） | 32B 全部来自 DRBG output | `ARCH_ENTROPY_WEAK` | `true` |
| 全部失败 | `memset(out, 0, 32)` | `ARCH_ENTROPY_NONE` | `false` |

### 3.3 caller 契约

| caller | 接受 STRONG | 接受 WEAK | 接受 NONE |
|---|---|---|---|
| `random_init()`（pool 初始 seed） | ✅ → `random_ready=true` | ✅ → `random_ready=true` | ❌ → `random_ready=false` |
| `reseed()`（周期性 rekey） | ✅ | ✅ → log warn，不 drop ready | ❌ → drop ready=false，log error（见 §7） |
| `get_random_bytes()` / `dev/urandom` 读 | ✅ ready 时正常输出 | ✅ ready 时正常输出 | ❌ → memset 请求 buffer 0（AAGU-2 既有） |
| AT_RANDOM（`setup_user_stack`） | ✅ via `arch_random_get_strong` | ❌ | ❌ |
| `/dev/random` 非阻塞读 | ✅ | ❌ → `EAGAIN` | ❌ → `EAGAIN` |

---

## 4. 架构实现（**不重复定义 quality**）

> 本节按 §0.2 禁止条款 1：架构实现只列"我能产出哪个 quality + 试/退顺序"，不重述 §2 的 STRONG/WEAK/NONE 定义。

### 4.1 x86_64 实现 (`kernel/arch/x86_64/random.c`)

```
// 试/退顺序（每次循环 4 次取 64-bit，10 次重试 per Intel SDM）
1. RDSEED × 4 → 全成功 → ARCH_ENTROPY_STRONG
2. RDRAND × 4 → 全成功 → ARCH_ENTROPY_WEAK
3. memset(out, 0, 32); *q = NONE; return false
```

**STRONG-WEAK 边界**：第 1 步中 4 次 RDSEED 只要有 1 次失败（CF=0）即不再尝试后续 RDSEED，**全部退到第 2 步**（禁止"3 RDSEED + 1 RDRAND"拼凑 STRONG）。

**CPUID 探测**：实现内首次调用前必须 CPUID 一次，结果缓存在 percpu `cpu_features.has_rdseed`/`has_rdrand`（具体字段名由 arch 实现自行决定；本 spec 不规定）。

**失败语义**：RDSEED 不支持 → 直接跳第 2 步；RDRAND 不支持 → 直接跳第 3 步。

### 4.2 aarch64 实现 (`kernel/arch/aarch64/random.c`)

```
// 试/退顺序（每次循环 4 次取 64-bit，10 次重试 per ARM ARM D17.1.5）
1. RNDRRS × 4 → 全成功 → ARCH_ENTROPY_STRONG
2. RNDR × 4 → 全成功 → ARCH_ENTROPY_WEAK
3. memset(out, 0, 32); *q = NONE; return false
```

**STRONG-WEAK 边界**：与 §4.1 同 — 第 1 步 4 次 RNDRRS 中 1 次失败即**全部退到第 2 步**（禁止"3 RNDRRS + 1 RNDR"拼凑 STRONG）。

**`ID_AA64ISAR0_EL1.RNDR` 探测**：在 `kernel/include/arch/aarch64/regs.h`（实施时由 AAGU-5.6 plan 补 `ISAR0_RNDR_RNDRRS` 宏）读 bits[63:60]，值 ≥ 0b0001 即支持 RNDR，值 = 0b0001 额外支持 RNDRRS。

**UEFI GetRNG**：是 bootloader 拉取、kernel 启动时已就绪的独立入口；aarch64 实现不重复拉取；由 `random_init()` 在调 facade 前先查 `bootctx->flags & BOOT_CONTEXT_HAS_BOOT_ENTROPY`（见 §5.1）。

**失败语义**：RNDRRS 不支持 → 直接跳第 2 步；RNDR 不支持 → 直接跳第 3 步。

### 4.3 跨架构降级矩阵

> 行 = 当前硬件实际暴露的源；列 = facade 调用结果。
> 本表是 AAGU-5.7 三档验收的对照表，本 spec 不规定具体测试代码（由 AAGU-5.7 plan 出）。

| 场景 | RDSEED | RDRAND | RNDRRS | RNDR | UEFI GetRNG | facade 结果 |
|---|---|---|---|---|---|---|
| QEMU 默认 x86_64 (`-cpu qemu64`) | ❌ | ❌ | — | — | 默认未挂 | NONE |
| QEMU 加 `+rdrand`（无 `+rdseed`） | ❌ | ✅ | — | — | 默认未挂 | WEAK |
| QEMU 加 `+rdrand,+rdseed` | ✅ | ✅ | — | — | 默认未挂 | STRONG |
| QEMU 加 `-device virtio-rng` | ❌ | ❌ | — | — | ✅ | STRONG（经 boot_entropy） |
| QEMU 默认 aarch64 (`-cpu cortex-a53`) | — | — | ❌ | ❌ | 默认未挂 | NONE |
| QEMU 加 aarch64 `-cpu max` | — | — | ✅ | ✅ | 默认未挂 | STRONG |
| QEMU 加 aarch64 `-cpu neoverse-n2` | — | — | ✅ | ✅ | 默认未挂 | STRONG |
| 真硬件 x86_64 Haswell+ / Zen 2+ | ✅ | ✅ | — | — | 视 firmware | STRONG |
| 真硬件 aarch64 Cortex-A77+ / Neoverse V1+ / Apple M1+ | — | — | ✅ | ✅ | 视 firmware | STRONG |
| 真硬件 aarch64 Cortex-A53/A55/A72/A76 | — | — | ❌ | ❌ | 视 firmware | NONE（无 firmware 时） |

**注**：aarch64 QEMU 加 `-cpu max` 等于同时启用 RNDR + RNDRRS（ARM ARM FEAT_RNG），与"仅 RNDR 无 RNDRRS"的场景在 QEMU 默认 CPU 列表里**没有**现成档位 — 详见 `docs/random/entropy-source-survey-merged.md` §6.2。如需测"仅 RNDR 无 RNDRRS"，需用 QEMU 9.2.0 之后的私有 `-cpu` 变体；AAGU-5.7 plan 决定是否补此测试档。

---

## 5. `random_init()` 决策树（AAGU-2 衔接）

> 本节是 §0.2 禁止条款 3 所要求的"由 q 的显式三分支决定 ready"。**不**用 facade 的 bool 返回值决定 ready；**必须**switch on `q`。

### 5.1 决策流程

```
random_init(bootctx):
    pool_blk = 0
    pool_bytes_since_reseed = 0

    // UEFI GetRNG 路径（独立于 facade；AAGU-2 已落地）
    if (bootctx && (bootctx->flags & BOOT_CONTEXT_HAS_BOOT_ENTROPY)):
        // 父契约 §交付物 2 标 STRONG；与 §2.1 UEFI 行对齐
        memcpy(pool_key, bootctx->boot_entropy, 32)
        random_ready = true
        log_info("CSPRNG: pool seeded STRONG from UEFI GetRNG (boot_entropy)\n")
        return

    // 通用 facade 路径
    arch_entropy_source_t q
    uint8_t buf[32]
    if (arch_random_get_entropy(buf, &q)):
        switch (q):
            case ARCH_ENTROPY_STRONG:
                memcpy(pool_key, buf, 32)
                random_ready = true
                log_info("CSPRNG: pool seeded STRONG\n")
                break
            case ARCH_ENTROPY_WEAK:
                memcpy(pool_key, buf, 32)
                random_ready = true   // AAGU-5 父契约 §交付物 2 授权
                log_warn("CSPRNG: pool seeded WEAK (DRBG); "
                         "AT_RANDOM will fail-closed via arch_random_get_strong\n")
                break
            case ARCH_ENTROPY_NONE:
                // 不应进此分支（facade false 时已 memset buf=0）
                // 但防御性处理：仍走 fail-closed
                memset(buf, 0, 32)
                random_ready = false
                log_warn("CSPRNG: NONE quality reached switch\n")
                break
        memset(buf, 0, 32)   // wipe stack
    else:
        // facade return false ⇒ q == NONE，buf 已 memset 0
        random_ready = false
        log_warn("CSPRNG: no hardware entropy source "
                 "(no UEFI GetRNG, no STRONG/WEAK via facade); "
                 "pool not ready, get_random_bytes fail-closed\n")
```

### 5.2 与 AAGU-2 既有 fail-closed 路径的对应

> 回应 AAGU-5.5 评审 item 3 的"未证明与 AAGU-2 一致"。下表逐档对照。

| 当前 spec 状态 | AAGU-2 对应状态 (`kernel/random/random.c`) | 一致性 |
|---|---|---|
| STRONG → `random_ready=true` | AAGU-2 `mix_hw_entropy()` 全 4 次 RDSEED 命中 → `return true` → `random_ready=true` | 一致（更严格：明确排除 RDRAND 混入） |
| WEAK → `random_ready=true` | AAGU-2 `mix_hw_entropy()` 仅 RDRAND 命中、无 RDSEED → `return true` → `random_ready=true` | 一致（同 ready=true 决策；新增"AT_RANDOM STRONG-only"由 §6 实施） |
| NONE → `random_ready=false` | AAGU-2 `mix_hw_entropy()` 4 次全失败（退到 cycle-counter ^ jiffies） → `return false` → `random_ready=false` | 一致（更严格：明确剔除 cycle-counter 伪熵，NONE 是真零而非低熵） |
| UEFI GetRNG 优先 → `random_ready=true`（STRONG） | AAGU-2 完全相同的 `bootctx->flags & BOOT_CONTEXT_HAS_BOOT_ENTROPY` 早返回分支 | 一致（完全相同的代码路径，仅注释加 STRONG 字样） |

### 5.3 行为差异（一句话总结）

- **`random_ready` 决策本身不变**：STRONG/WEAK/NONE → true/true/false 与 AAGU-2 的 `mix_hw_entropy()` true/false 决策**逐档一致**
- **新增** AT_RANDOM 专用 STRONG-only 路径（§6）：这是 AAGU-5 §交付物 3 的父契约要求，AAGU-2 不曾有；task.c 调用方改造即可，不修改通用 ready 决策

---

## 6. AT_RANDOM 路径（**新增**，AAGU-5 §交付物 3）

### 6.1 现有调用

`kernel/sched/task.c:1181`：
```c
get_random_bytes(KSTACK(rsp), 16);
```
即把 16B AT_RANDOM payload 委托给通用 CSPRNG。该路径在 WEAK-only pool 下也会成功（AAGU-2 已落地），但 STRONG-only 需求不满足。

### 6.2 新调用

```c
uint8_t at_random_buf[16];
if (!arch_random_get_strong(at_random_buf)):
    // spawn/exec 路径决策：重试 / 阻塞 / fail
    // AAGU-5 §交付物 3 留白，由 task.c 决策（plan Task 5 实施）
    return -1;   // 暂定：返回错误让 caller 决策
memcpy(KSTACK(rsp), at_random_buf, 16);
memset(at_random_buf, 0, 16);
```

### 6.3 决策点

- **`arch_random_get_strong()` 返回 false**：调用方负责决策 — 当前 OS01 没人在 spawn/exec 阻塞，故先 `return -1` 触发 spawn 失败。后续若引入"阻塞等 STRONG 出现"，由新 issue 跟踪。
- **不需要额外 wait/timeout**：本 spec 不阻塞 spawn 路径；WEAK-only 环境下用户进程暂时无法启动（与 AAGU-2 fail-closed 行为一致）。
- **不修改** `setup_user_stack()` 的栈布局计算：AT_RANDOM 16B 的位置、对齐、auxv 顺序不变。

---

## 7. Reseed 策略（与 AAGU-2 对齐）

### 7.1 AAGU-2 既有行为

`kernel/random/random.c:128` `reseed()`：
- 调 `mix_hw_entropy(hw)` 拿 32B 新熵
- ChaCha20 block(seed = old_key || hw) → 取前 32B 作新 pool_key
- 若 `hw_ok=false`（无 RDRAND/RDSEED），**log warn 但保持 random_ready=true**，因为 ChaCha20 nonlinear + initial seed 强度足够

### 7.2 新策略

```
reseed():
    arch_entropy_source_t q
    uint8_t hw[32]
    if (!arch_random_get_entropy(hw, &q)):
        // NONE: 无新熵
        // 与 AAGU-2 差异：从"cycle-counter 混入"改为"显式 NONE → drop ready=false"
        // 理由：AAGU-2 的 cycle-counter 字节是某种"伪熵"，新 spec NONE 是真零，语义不同
        memset(pool_key, 0, 32)   // 让下一个 get_random_bytes 走 fail-closed
        random_ready = false
        log_error("CSPRNG: reseed got NONE; pool dropped to not-ready\n")
        return

    uint8_t seed[64], block[64], nonce[12] = {0}
    memcpy(seed,      pool_key, 32)
    memcpy(seed + 32, hw,       32)
    chacha20_block(seed, 0, nonce, block)
    memcpy(pool_key, block, 32)

    memset(block, 0, 64)
    memset(seed,  0, 64)
    memset(hw,    0, 32)

    pool_bytes_since_reseed = 0

    if (q == ARCH_ENTROPY_WEAK):
        log_warn("CSPRNG: reseed mixed WEAK entropy; pool stays ready\n")
    // STRONG 不打日志（常规路径）
```

### 7.3 与 AAGU-2 的差异（一处）

**NONE reseed 时 drop ready=false** — 这是 AAGU-2 后的新行为。理由见 §7.2 注释：AAGU-2 的 cycle-counter 混入有"低熵"，本 spec NONE 是真零，安全状态不同。

其它（STRONG/WEAK reseed 不 drop ready、ChaCha20 块构造、wipe 顺序）逐行对齐 AAGU-2。

---

## 8. 禁止条款回顾（与 §0.2 呼应）

| §0.2 条款 | spec 落地节 |
|---|---|
| 1. STRONG/WEAK/NONE 定义集中 | §2（本 spec 唯一权威） |
| 2. 不复制 RDRAND 路径到 aarch64 | §4.1 vs §4.2 分别独立列出 |
| 3. quality 真驱动控制流 | §5 三分支 switch；§6 AT_RANDOM STRONG-only |
| 4. 不重定义 AAGU-2 fail-closed | §5.2 逐档对照表；§7.3 reseed 差异说明 |
| 5. 不留 cycle-counter 死代码 | §2.3、§5.1 NONE 分支 memset |
| 6. plan 不重定 quality | 实施 plan 引用 §2，自身不再裁决 |

---

## 9. 引用

| 引用 | 位置 |
|---|---|
| AAGU-5 父 issue | platform issue 01a0b568-fcde-7570-9c02-6cce956d0535 |
| AAGU-2 fail-closed 实现 | commit `8931cab`；`kernel/random/random.c` §1, `kernel/include/core/bootinfo.h` BOOT_CONTEXT_HAS_BOOT_ENTROPY |
| AAGU-5.1 x86_64 调研 | `docs/random/entropy-source-survey-x86_64.md` |
| AAGU-5.2 aarch64 调研 | `docs/random/entropy-source-survey-aarch64.md` |
| AAGU-5 合并稿（§5 跨架构冲突） | `docs/random/entropy-source-survey-merged.md` |
| Intel SDM Vol. 2 RDRAND/RDSEED | Intel 文献 |
| Arm ARM DDI 0601 RNDR/RNDRRS | arm.com |
| NIST SP 800-90A（DRBG）/ SP 800-90B（Entropy Source） | NIST |
| UEFI 2.4 §37.5 EFI_RNG_PROTOCOL | UEFI Forum |
| OS01 weak-default + strong-override 模式 | `docs/arch.md` §相关 |
| AAGU-5.5 评审 item 3（fail-closed 衔接） | platform issue 01a0cd78-... 评论 01a0cdf5-... |

---

## 10. 变更记录

- **v2（2026-09-23, AAGU-5.5 评审最小修复）**：
  - §2.2 RNDR 改判 WEAK（按合并稿 §5.1 Option A 严格分级；AAGU-5.5 item 4 解决）
  - §2.3 NONE 显式排除 cycle-counter 伪熵（item 4 + item 6 解决）
  - §5.1 random_init 改为 q 的显式三分支 switch（item 1 解决）
  - §5.2 新增 AAGU-2 逐档对照表（item 3 解决）
  - §6 新增 AT_RANDOM 专用 STRONG-only helper 契约（item 3 + 父契约 §交付物 3）
  - §7 reseed 策略重写；NONE 重置 pool_key + drop ready=false（item 3 一致性论证）
  - §0.2 禁止条款 6 显式禁止 plan/task 重定 quality（item 6 解决）
  - §4.3 跨架构降级矩阵更新（item 4 解决）
- **v1（初版，已被评审）**：评审 item 1/3/4/5/6 FAIL；本 v2 覆盖
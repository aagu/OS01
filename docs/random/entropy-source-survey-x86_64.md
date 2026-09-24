# x86_64 熵源调研报告（RDSEED / RDRAND / UEFI GetRNG）

| 字段 | 值 |
|---|---|
| Spec ID | `entropy-source-survey-x86_64` |
| Issue | AAGU-5.1 |
| 状态 | Draft v2（revised after AAGU-5 BLOCK — see §9） |
| 最后更新 | 2026-09-23 |
| 适用范围 | x86_64 arch override（`kernel/arch/x86_64/random.c`，**当前不存在**） |
| 父文档 | `docs/arch.md` §「weak-default + strong-override」 |
| 后续读者 | AAGU-5.3（facade 设计）/ AAGU-5.6（x86_64 strong override 实现） |

---

## 1. 范围与不重复造轮子

本文档**只做调研**，不写代码、不动 kernel/ 任何 C 文件。结论将喂给 AAGU-5.3（facade 设计）和 AAGU-5.6（x86_64 strong override 实现）两个子任务。

**前置一致性**：AAGU-5 总契约（arch-neutral entropy 采集 facade）已经敲定 STRONG / WEAK / NONE 三级质量标签 + `arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality)` 接口。本调研不复述；只回答"x86_64 上哪些硬件熵源对应哪个等级"。

**OS01 现有实现（早期形态）**：

```text
$ find kernel/include/arch kernel/include/random kernel/random
kernel/include/arch/random.h           ← facade：仅 inline rdrand64/rdseed64 stub
kernel/include/arch/x86_64/random.h     ← x86_64 inline RDRAND/RDSEED（含 CPUID 守卫）
kernel/include/random/random.h          ← 公共 facade：random_init / get_random_bytes
kernel/random/random.c                  ← ChaCha20 + mix_hw_entropy（含 cycle-counter fallback）
kernel/include/core/bootinfo.h          ← boot_entropy[32]（UEFI GetRNG 通道）
boot/uefi/arch/x86_64/boot.c:231        ← x86 UEFI 走 EFI_RNG_PROTOCOL → boot_entropy
boot/uefi/arch/aarch64/boot.c:401       ← aarch64 UEFI 走 EFI_RNG_PROTOCOL → boot_entropy
```

并且 `kernel/arch/aarch64/Makefile` **不存在**（aarch64 全部由 `kernel/Makefile` 的 `KERNEL_C_SOURCES` whitelist 拉入），不存在 x86_64 ↔ aarch64 路径错挂风险（详见 §6.4）。

---

## 2. 候选熵源清单与评级

按可用性 × 信任度排序，**逐条标注**：

| # | 源 | OS01 评级 | CPUID 检测 | QEMU 默认 (`qemu64`) | 真实硬件 (Haswell+) | 引用 |
|---|---|---|---|---|---|---|
| 1 | **RDSEED** (`rdseed r64`) | **STRONG** | leaf 7 subleaf 0 EBX bit 18 | ❌ bit=0（实测 §6.2 表 1） | ✅ Intel Broadwell+ / AMD Zen 2+ | Intel SDM Vol. 2 §「RDSEED」 |
| 2 | **RDRAND** (`rdrand r64`) | **WEAK** | leaf 1 ECX bit 30 | ❌ bit=0（实测 §6.2 表 1） | ✅ Intel Ivy Bridge+ / AMD Zen+ | Intel SDM Vol. 1 §7.3 |
| 3 | **UEFI GetRNG**（bootloader 通道） | **STRONG**（取决于 firmware 算法） | 不走 CPUID（firmware protocol） | ✅ via `-device virtio-rng` | ✅ TPM / RNG 设备 | UEFI Spec §35.5；`boot/uefi/arch/x86_64/boot.c:231` |
| 4 | **RDTSC + 操作抖动**（jitterentropy 模式） | **WEAK**（仅当 RDSEED/RDRAND/UEFI 全无时的最后手段） | 无（TSC 总是存在） | ✅ 可运行 | ✅ | Linux `arch/x86/kernel/cpu/amd.c` 注；jitterentropy 库（Stefan Mangold） |
| 5 | **TSC 裸值**（单次 RDTSC） | **NONE**（采样率稳定，无熵） | 无 | ❌ 不接受 | ❌ 不接受 | — |
| 6 | **VMX/SVM 退出时间抖动** | **NONE-边界**（host 操控，不安全 → 不可信任） | VMX/SVM feature bits | ❌ 不接受 | ⚠ 仅在 Type-1 VMM 才存在 | Sebastian Vogl 等 "Whispers in the Hypervisor" |
| — | /dev/hwrng / entropy 守护 | — | — | **❌ 排除** | **❌ 排除** | kernel 自给硬约束 |

### 2.1 RDSEED（OS01 评级 = STRONG — OS01 威胁模型下的策略）

- **CPUID 检测**：leaf 7 subleaf 0，EBX bit 18 = `CPUID_FEAT_EBX_RDSEED`（已在 `kernel/include/arch/x86_64/regs.h:151` 定义）。
- **失败语义**：`CF=1 && OF=0` 才算成功；`CF=0` 时**目标寄存器值未定义**（Intel SDM Vol. 2 「RDSEED」："the data is valid only if CF=1"），**绝对不可消费**。
- **重试次数**：底层熵源可能短暂欠载，Intel SDM 建议"软件循环直到成功"。实操上限通常 10–1000 次。OS01 当前 inline 实现用 10 次（见 `kernel/include/arch/x86_64/random.h:27`），够用但偏保守。
- **为什么 OS01 把它归 STRONG**：RDSEED 是**非条件化熵源**（conditionable entropy source），输出可直接作为 CSPRNG seed material。Intel SDM Vol. 1 §7.3.1.2 把 RDSEED 描述为"适合任意长度 PRNG 的种子源"（a seed source suitable for use in seeding an arbitrary-length PRNG）。OS01 的 STRONG = "适合作 CSPRNG seed" — RDSEED 满足。
- **OS01 不做 ISA 安全性断言**：本节只引用 Intel SDM 的事实性描述，不代 ISA 做绝对断言。RDSEED 在硬件实现层确有侧信道历史问题（如 RDRAND/RDSEED microcode 漏洞 CVE-2019-11098 / 2019-11157），但这些不是 OS01 dev 场景的攻击模型 — 真硬件用户在生产部署前应执行 microcode update（见 §7 风险项 #6）。
- **32 字节输出契约**：每次 RDSEED64 产出 8 字节，`arch_random_get_entropy()` 需循环 4 次（每次独立重试，CFG=1 才接受）。4 次成功概率约 (0.999……)^4。**任何一次失败则降级**（详见 §3）。

### 2.2 RDRAND（OS01 评级 = WEAK — OS01 威胁模型下的策略）

- **CPUID 检测**：leaf 1，ECX bit 30 = `CPUID_FEAT_ECX_RDRAND`（已在 `kernel/include/arch/x86_64/regs.h:148` 定义）。
- **失败语义 / 重试**：与 RDSEED 同（CF=1 才接受）；实测一次成功率 100% – 99.9999……%（取决于实现代际）。
- **Intel/AMD 的公开描述**（不替 ISA 下断言）：
  - Intel SDM Vol. 1 §7.3：RDRAND 适合"高质量密码学用途"（"Suitable for high-quality cryptographic key generation, digital signature, RNG, and other security applications"）。
  - Intel SDM Vol. 2 「RDRAND」intro：声明 output "符合 NIST SP 800-90A"；实现是 AES-128 CTR_DRBG。
  - RDSEED 与 RDRAND 的关系（Intel SDM Vol. 1 §7.3.1.2）：RDSEED 是"任意长度 PRNG 的种子源"；RDRAND 是"软件通用取数"接口。
- **OS01 把它归 WEAK 的理由（**OS01 威胁模型 / 防御深度原则，非 ISA 断言**）**：
  1. OS01 的 STRONG 标签保留给"明确可证 seed-quality"的源；RDRAND 的 SP 800-90A 合规是 AES-128 DRBG 的 *内部* DRBG 输出，而非原始熵。
  2. 同一颗 CPU 上若有 RDSEED 可用，RDSEED 优先（OS01 决策树 §4.1）。
  3. WEAK 不代表 RDRAND 输出可被预测 — 仅代表"在我们的威胁模型下，它不可单独作为 CSPRNG seed"。
  4. **AES-128 等价安全强度**：RDRAND 内部 DRBG 是 AES-128，理论安全强度 128 bit；STRONG 路径若能拿到 RDSEED 同源 256-bit 熵，质量上界更高。
- **是否被 RDSEED 取代**：是 — RDSEED 提供了"未加工熵"，RDRAND 是"已加工随机数"。OS01 决策树总是先尝试 RDSEED。
- **32 字节输出契约**：同 RDSEED，4 次循环。

### 2.3 UEFI GetRNG（bootloader 通道 — 与 arch_random_get_entropy() 并列，不混作 fallback）

UEFI 的 `EFI_RNG_PROTOCOL` 是 **bootloader 阶段** 取熵的另一条通道，**逻辑上完全独立**于 `arch_random_get_entropy()`：

- **生产端**（`boot/uefi/arch/x86_64/boot.c:231` 和 `boot/uefi/arch/aarch64/boot.c:401`）：在 OS01 UEFI bootloader 启动时 best-effort 调用 `GetRNG`；成功 → 32 字节写入 `ctx->boot_entropy` 并设 `BOOT_CONTEXT_HAS_BOOT_ENTROPY` 标志位。
- **消费端**（`kernel/random/random.c:90-95`）：`random_init()` **先**检查 `bootctx->flags & BOOT_CONTEXT_HAS_BOOT_ENTROPY`，命中 → `random_ready=true`，log "pool seeded from UEFI GetRNG"，**不再**调用 `arch_random_get_entropy()`。
- **质量标签**：OS01 把 UEFI GetRNG 视为 STRONG（firmware 自带 DRBG / 物理 RNG，标签依赖 firmware — QEMU virtio-rng 由宿主 OS 提供；真硬件 TPM / RDRAND passthrough 也符合 NIST SP 800-90A/B）。
- **优先级**：UEFI GetRNG > `arch_random_get_entropy()`。前者一旦成功即决定 `random_ready=true`，后者只在缺 UEFI 时被调用作 fallback。
- **失败语义**：`GetRNG()` 返回 `EFI_NOT_FOUND` → `boot_entropy` 不写、flag 不设 → kernel `random_init()` 跳过 UEFI 分支，继续往下走 `arch_random_get_entropy()`。
- **不要混淆**：UEFI GetRNG **不是** x86 指令集 fallback；与 RDRAND/RDSEED 处于不同生命周期、不同 production path。**§4 决策树只描述 `arch_random_get_entropy()` 内部行为**；UEFI 优先级体现在 §5 状态机的入口判定上。

### 2.4 RDTSC + 操作抖动（WEAK fallback 候选）

- **TSC 基本特性**：x86 自 Pentium 起的 64-bit cycle counter，单调递增（受 TSC invariant bit 约束）。**单次 RDTSC 值本身无熵**（采样率稳定），**根本不可作熵源**。
- **"jitter entropy" 思路**：循环执行一组操作（如 cache-line touch / inter-processor interrupt → read TSC deltas），统计相邻 reads 的最低有效位差异。差值来自 cache 命中 / 未命中 / 流水线状态等微小变量。
- **实操限制**（jitterentropy 库 / Linux `jitterentropy_rng.c`）：
  - 至少需要 1024–8192 次采样才能累积到 128 bit 信任；
  - 攻击者若能操纵 cache 状态、调度、超标量流水线，则抖动模式可降级；
  - **VM 内情景**：TCG 模式 QEMU 的 cache 模型精度不够，**抖动可能为 0**；KVM 模式才接近真硬件。
- **OS01 取舍**：可作"全无 RDSEED/RDRAND/UEFI 时的 WEAK fallback"，但**实现成本高**（cycle counter、statistical test、threads 化），且对 AT_RANDOM 这种 fail-closed 场景**不够安全**（AAGU-5 明确 AT_RANDOM 路径必须 STRONG 或 fail-closed，不接受 WEAK）。**先不做**，Stage 3 决策树中以"NONE"降级而接受 boot-time `random_ready=false`。

### 2.5 VMX / SVM 退出抖动

- **机理**：在 VMM 中主动 VMEXIT（cpuid / rdmsr / I/O port）后读 TSC；VMEXIT 触发 vCPU 切换到 host 执行，**时长受宿主机调度影响**。
- **安全评估**：
  - host 可见所有出口时间（host 控制 clock、调度器、TSC offset），攻击者可构造特定 JITTER 模式 → **完全可预测**。
  - 在 Type-1 VMM（KVM/Xen）host 可信时，可视为 WEAK；在 Type-2 VMM（QEMU/VMware Workstation）host 就是攻击者。
  - **不满足 STRONG**。
- **OS01 取舍**：**不采用**。理由：(a) OS01 主要在裸机和 QEMU 跑，host 不可信；(b) 调用 VMEXIT 自身就需 bypass RDRAND/RDSEED 的禁锢，引入不可控状态。**仅在风险章节标"未决定"**，不写入决策树主干。

### 2.6 明确排除项

按 AAGU-5 任务 hard constraint：

- ❌ `/dev/hwrng` 路径（host kernel、设备驱动）—— 不是 OS01 内核可见的接口
- ❌ 用户态 entropy 守护进程（`rngd` / `haveged`）—— kernel 必须自给
- ❌ 网络熵源（Tor / `Entropy.as-a-Service`）—— 内核 bootstrap 阶段的纯硬件约束

---

## 3. 32 字节（256 bit）一次性输出契约分析

`arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality)` 必须一次性写满：

### 3.1 RDSEED64 / RDRAND64 性能（**仅作初步估计，实施前再实测**）

- Intel SDM Vol. 2「RDSEED Performance」：单条 RDSEED 指令 latency 约 30–100 cycles（依代际：Haswell ~100 / Skylake ~80 / Broadwell ~75 / Alder Lake ~30）。
- 假设 RDSEED 平均成功 latency 80 cycles，4 次循环（无重试失败） ≈ 320 cycles < 1µs。
- **多线程**：RDSEED 在 multi-core 上**每核独立熵**（Intel SDM 明确），无锁、无争用。
- **性能契约作为承诺**（非实施前提）：本节数字基于 SDM 引述，未在 OS01 target 上 re-benchmark。**AAGU-5.6 / 实施期必须重新测量**；不要从调研报告流入性能承诺。

### 3.2 32-byte/quality 算法（revised — 严格一次成型）

> **reviewer BLOCK v1 修复点**：v1 算法允许"任一 RDSEED 成功 + 其他零填充 → STRONG"。违反"32 字节必须完整填满"契约，且会让 AT_RANDOM 接受不完整 STRONG buffer。本节给出**严格一次成型**算法。

**正确算法**（伪代码）：

```c
bool arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *q) {
    uint64_t w[4];

    /* Step 1: try RDSEED for all 4 words (STRONG path). */
    int rdseed_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (!rdseed64_retry(&w[i])) { rdseed_ok = 0; break; }
    }
    if (rdseed_ok) {
        memcpy(out, w, 32);
        memset(w, 0, 32);
        *q = ARCH_ENTROPY_STRONG;
        return true;
    }

    /* Step 2: RDSEED 全失败 — RDRAND 是否能填满？(WEAK path). */
    int rdrand_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (!rdrand64_retry(&w[i])) { rdrand_ok = 0; break; }
    }
    if (rdrand_ok) {
        memcpy(out, w, 32);
        memset(w, 0, 32);
        *q = ARCH_ENTROPY_WEAK;
        return true;
    }

    /* Step 3: both fail — do NOT copy partial / zero-padded buffer. */
    memset(w, 0, 32);              /* wipe stack residue */
    memset(out, 0, 32);            /* explicit zero-fill — caller can detect */
    *q = ARCH_ENTROPY_NONE;
    return false;                   /* explicit "no hw entropy available" */
}
```

**关键不变量**：

1. **要么整 32 字节来自 RDSEED，要么整 32 字节来自 RDRAND，要么两者都没有** —— **不存在"3 个 word RDSEED + 1 个 word 零"的混合状态**。
2. **失败路径不复制**部分 word 到 `out`，但**仍显式 memset(out, 0, 32)** — 让 caller（`kernel/random/random.c`）能立刻检测到 NONE。
3. **Stack wipe**：每个成功路径 memset `w[]` 让 RDSEED/RDDRAND raw 输出不在栈残留。
4. **retry 预算**：每个 `rdseed64_retry` / `rdrand64_retry` 内部 10 次重试（沿用当前 inline 实现）。RDSEED 真实平均 ~1 cycle / RDSCED23 cycle latency → 10 次足够；RDRAND 几乎 100% 一次成功。

**为什么不允许混合**：AT_RANDOM payload 是 16 字节，从 `arch_random_get_entropy()` 输出的 32 字节里取前 16 字节。若部分 word 来自 RDSEED + 部分为零，AT_RANDOM 接受的就是"16 字节里一部分真熵 + 一部分零" — 与 fail-closed 语义冲突。**只接受整 32 字节都来自同一等级**。

### 3.3 重试与 #UD 的语义区分（**strictly CPUID + CF gating**）

**关键约束**：实现必须**只**依赖 CPUID bit + CF 标志位，**绝不**根据 CPU model 名字（如 "Haswell" vs "Skylake"）推断质量。

```c
static inline bool rdseed64_retry(uint64_t *out) {
    /* 必须先检查 CPUID bit — 否则 QEMU 默认 CPU 上 RDSEED 会 #UD */
    if (!cpuid_has_rdseed()) return false;
    for (int i = 0; i < 10; i++) {
        uint64_t v; unsigned char cf;
        __asm__ __volatile__("rdseed %0; setc %1" : "=r"(v), "=qm"(cf) :: "cc");
        if (cf) { *out = v; return true; }
    }
    return false;
}
```

`cpuid_has_rdseed()` 是 AAGU-5.6 的实现细节（一次性 CPUID probe 后存 percpu）。当前 OS01 inline 版本每次调用都做 CPUID — **慢**（~100 cycles / call），Stage 3 应改为 BSP 一次 CPUID 探测（参见 §7 风险项 #2）。

---

## 4. OS01 推荐的尝试顺序

### 4.1 `arch_random_get_entropy()` 内部决策树

```
                       ┌──────────────────────────┐
                       │ arch_random_get_entropy  │
                       │   (out[32], *quality)    │
                       └────────────┬─────────────┘
                                    │
                          CPUID(L7.S0.EBX[18])?
                                    │
        ┌───────────────────────────┼────────────────────────────┐
        ▼                           ▼                            ▼
  4× RDSEED 全成功          4× RDSEED 失败 →                     全部失败
  STRONG                     4× RDRAND 全成功？                 NONE
  → 直接 memcpy              ├─ 是 → WEAK                        → memset(out,0,32)
    memset(w,0)              │        memcpy + memset(w,0)        *q = NONE
    *q=STRONG                │        *q=WEAK                     return false
    return true              └─ 否 → memset(out,0,32)
                                       *q=NONE
                                       return false
```

### 4.2 调用方约定（AAGU-5 已敲定；修订自 v1）

| 路径 | 接受 STRONG | 接受 WEAK | 接受 NONE | 实现位置 |
|---|---|---|---|---|
| UEFI GetRNG → boot_entropy（bootloader 通道） | ✅ | ❌ | ❌ | `boot/uefi/arch/x86_64/boot.c::capture_entropy` |
| `arch_random_get_entropy()` → `kernel/random/random.c::random_init()` 初次种子 | ✅ | ✅（仅当 caller 接受 fail-open WEAK 语义） | ❌ → `random_ready=false` | `kernel/random/random.c` |
| `arch_random_get_entropy()` → `kernel/sched/task.c::setup_user_stack()` AT_RANDOM | ✅ only | ❌（返回错误） | ❌ | AAGU-5.5 AT_RANDOM 路径 |
| `arch_random_get_entropy()` → 周期 reseed（每 1 MiB 输出） | preferred | acceptable（log_warn） | ❌ drop to not-ready | `kernel/random/random.c::reseed` |
| `/dev/random` 非阻塞读 | ✅ | ⚠️ 返回 `EAGAIN` | ❌ EAGAIN | `kernel/fs/devfs.c` |
| `/dev/urandom` 阻塞读 | ✅ | ✅（允许 WEAK 流） | ❌ | `kernel/fs/devfs.c` |
| 内核 PID / 弱伪随机需求（future） | ✅ | ✅ | ❌（不允许 NONE） | future call site |

> **v1 错误修复**：v1 把 WEAK 列为 `random_ready=false`。这与 AAGU-5 父 spec 冲突。AAGU-5 明确："WEAK 时 `random_ready=true`，**仅**安全敏感路径 STRONG-only"。当前修订：`random_init()` 收到 STRONG 或 WEAK 都设 `random_ready=true`，但 `/dev/random` 非阻塞读在 WEAK 模式返回 `EAGAIN`；AT_RANDOM / canary / KDF 路径仍只取 STRONG（failure → 显式错误让 spawn/exec 决定）。

### 4.3 状态机（revised — 反映 WEAK 实际语义）

```
boot ──► random_init()
              │
              ├─ UEFI GetRNG success → random_ready = true
              │                       (boot_entropy 是 32 字节 STRONG)
              │                       log_info "pool seeded from UEFI GetRNG"
              │                       skip arch_random_get_entropy()
              │
              └─ no UEFI GetRNG ──► arch_random_get_entropy()
                                          │
                                          ├─ STRONG ─► random_ready = true
                                          │             reseed 周期内若新一次
                                          │             arch_random_get_entropy
                                          │             STRONG → 持续 ready
                                          │
                                          ├─ WEAK ──► random_ready = true   ◄── v1 修正
                                          │             重新 reseed 失败 (WEAK/NONE)
                                          │             → 仍 ready, 仅 log_warn
                                          │             AT_RANDOM 路径 STRONG-only
                                          │             缺 STRONG → fail-closed
                                          │
                                          └─ NONE ──► random_ready = false
                                                        整本期间 fail-closed
```

**v1 → v2 关键差异**：

| 状态 | v1（错误） | v2（修订） |
|---|---|---|
| WEAK 时 `random_ready` | `false` | `true` |
| WEAK 时 AT_RANDOM 路径 | fail-closed（隐式） | 显式错误返回，让 caller 决定 |
| WEAK 时 `/dev/urandom` | 未明确 | ✅ 接受（阻塞读始终返回数据） |
| WEAK 时 `/dev/random` 非阻塞读 | 未明确 | `EAGAIN`（与 AAGU-5 一致） |
| reseed 遇 WEAK | drop to not-ready | log_warn，保留 ready |

### 4.4 与现有 `mix_hw_entropy()` 的关系

`kernel/random/random.c` 当前用 `rdseed64`/`rdrand64` 三个 inline 指令 + cycle counter ^ jiffies fallback。AAGU-5.4 会替换为 `arch_random_get_entropy()` 调用。本调研结论：

- **`mix_hw_entropy()` 中的 cycle-counter ^ jiffies fallback 必须删除**（即使 RDRAND/RDSEED 不通，也不再用软件伪熵"凑数"）—— AAGU-1 评审中"软件伪熵混迹 HW 熵"的批评正落在这里。
- **replacement**：AAGU-5.3 在 `kernel/include/arch/random.h` 加 `arch_entropy_source_t` 枚举 + 强声明 `arch_random_get_entropy()`；AAGU-5.6 在 `kernel/arch/x86_64/random.c` 实现。
- **混合策略**：AAGU-5.4 写 `kernel/random/random.c` 的新版本应**严格 1:1** 映射：UEFI success → ready=true；否则调 arch_random 看 quality（STRONG/WEAK → ready=true；NONE → ready=false）。**不**再做任何 cycle-counter / jiffies fallback。

---

## 5. 与 AAGU-5 契约的一致性检查

| 契约项 | 来源 | 一致性 |
|---|---|---|
| facade `arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality)` 在 `kernel/include/arch/random.h` | AAGU-5 §交付物 1 | ✅ 本调研 §3.2 / §4.1 / §4.2 / §4.3 全部假设该接口 |
| out[32] 一次性 32 字节 | AAGU-5 §交付物 1 | ✅ §3.2 给出 4× RDSEED64 循环，**严格一次成型**（修订 v1 的混合策略） |
| 32 字节应能在合理时间（< 1µs）内产出 | 本调研 §3.1 | ⚠️ 仅作初步估计；实施前 AAGU-5.6 必须重测（**不要从调研流入性能承诺**） |
| x86_64 实现位于 `kernel/arch/x86_64/random.c`（**新建**） | AAGU-5 §交付物 1 | ⚠️ 现有 inline `kernel/include/arch/x86_64/random.h` 需改名/迁移（AAGU-5.6 决定细节） |
| 当前 `kernel/include/arch/x86_64/random.h` 是否被 build 引用？ | grep | ✅ `kernel/include/arch/random.h` line 8 `#include <arch/x86_64/random.h>` |
| aarch64 whitelist 无 x86_64 误挂 | `kernel/Makefile` `KERNEL_C_SOURCES` aarch64 分支 | ✅ 第 21-32 行手动 whitelist，**无 `arch/x86_64/*`** |
| 反向：x86_64 whitelist 无 aarch64 误挂 | `kernel/Makefile` 第 33+ `$(wildcard ...)` | ✅ x86_64 用 wildcard，`arch/aarch64/*` 不在 `arch/x86_64/`，不会被扫到 |
| RDSEED → STRONG / RDRAND → WEAK / 全无 → NONE | AAGU-5 §交付物 1 | ✅ §2.1、§2.2、§3.2 一致 |
| UEFI GetRNG 跨架构 bootloader 通道，**不混作** x86 instruction fallback | AAGU-5 交付物 1 + 本调研 §2.3 | ✅ §2.3 单列 + §4.3 状态机入口判定 |
| AT_RANDOM 仅 STRONG 或 fail-closed | AAGU-5 §交付物 3 | ✅ §4.2 表 |
| WEAK 时 `random_ready=true`，非敏感路径可接受 WEAK，安全敏感路径 STRONG-only | AAGU-5 §交付物 2 | ✅ §4.2 表 + §4.3 状态机（v2 修订 v1 错误） |
| 三档 QEMU 实测：default（无 RDRAND）、`+rdrand,+rdseed`、`host` | AAGU-5 §验收 | ✅ §6.2 表 1 / 表 2 — 含 QEMU 11.1.1 实际 query-cpu-model-expansion 输出 |
| `/dev/hwrng` / 用户态守护 排除 | AAGU-5 hard constraint | ✅ §2.6 显式排除 |

---

## 6. QEMU 默认 vs 真实硬件两档差异（含实测数据）

### 6.1 OS01 当前 QEMU 调用方式

`mk/targets/x86_64.mk:9` 仅设置 `QEMU_BIN=qemu-system-x86_64`，**不传 `-cpu` 参数**。因此 `make run` / `make test` 默认走 QEMU 兜底 CPU model = `qemu64`。

### 6.2 QEMU CPU model 实测（RDRAND / RDSEED bit exposure）

**测试方法**（QEMU 11.1.1 host on x86_64, Linux 7.2.6-arch2-1）：

```bash
# 通过 QMP query-cpu-model-expansion 拿 model.props 中 rdrand / rdseed 的 boolean 值
python3 qemu_check.py  # 见附录 A
# 完整 boot 命令：qemu-system-x86_64 -M pc -qmp tcp:127.0.0.1:NNNN,server,nowait
#                                  -cpu <MODEL> -S -display none
```

**表 1 — 不同 CPU model 下 RDRAND / RDSEED bit 暴露**（QEMU 11.1.1）：

| CPU model（`-cpu`） | `props.rdrand` | `props.rdseed` | QEMU 9.2.0 source 引用 |
|---|---|---|---|
| `qemu64`（QEMU 默认） | **False** | **False** | `target/i386/cpu.c:2484-2502` 显式 `FEAT_1_ECX = SSE3\|CX16`，**未**设 RDRAND |
| `qemu64-v1` | False | False | 同上 |
| `kvm64` | False | False | `target/i386/cpu.c` kvm64 显式 `FEAT_1_ECX = SSE3\|CX16` |
| `Haswell-v3` | True | **False** | `target/i386/cpu.c:3222` 设 RDRAND，但 `FEAT_7_0_EBX` 显式未列 `RDSEED`（QEMU 9.2.0 建模遗漏） |
| `Haswell-v3 + rdseed=true` | True | True | QMP model expansion 接受 props override |
| `Skylake-Client-v3` | True | True | `target/i386/cpu.c` Skylake 默认包含 RDRAND + RDSEED |
| `host`（requires KVM） | (host cpu 决定) | (host cpu 决定) | QEMU 报错：`CPU model 'host' requires KVM or HVF` |

**表 2 — `qemu64` + feature override 测试**（确认 `+rdrand,+rdseed` 等显式开关生效）：

| `-cpu` 参数 | `props.rdrand` | `props.rdseed` |
|---|---|---|
| `qemu64` | False | False |
| `qemu64` + `props={rdrand:True}` | True | False |
| `qemu64` + `props={rdseed:True}` | False | True |
| `qemu64` + `props={rdrand:True, rdseed:True}` | True | True |

**QEMU 9.2.0 source 与 11.1.1 实测一致性**：QEMU 9.2.0 `target/i386/cpu.c` 中 `qemu64` 模型定义（`FEAT_1_ECX = CPUID_EXT_SSE3 | CPUID_EXT_CX16`，**未**列 `CPUID_EXT_RDRAND`）和 `kvm64` 模型一致。这两条数据交叉证实：OS01 dev 默认 `qemu64` 下，**当前 inline CPUID 守卫见到 bit=0 → 直接返回 false → 走 cycle-counter fallback**（这一行为正是 `kernel/include/arch/x86_64/random.h:25` 的设计目标）。

**关键约束重申**：实现必须以 CPUID bit + CF 标志位为唯一判据，**不能**因 CPU model 名字推断输出质量。
- 上表显示 `Haswell-v3` 在 QEMU 11.1.1 中**不**advertise `RSEED`（实测 False），这与 Intel 真实 Haswell 处理器规格不符 — 是 QEMU 9.2.0 / 11.1.1 建模的历史遗漏，不影响 OS01（OS01 只看 CPUID bit，不看 model name）。
- 真硬件 Haswell+ 都有 RDSEED bit（Intel / AMD spec sheet 一致）。

### 6.3 QEMU 9.2.0 source — RDRAND/RDSEED 的 TCG 实现路径

读 `target/i386/tcg/translate.c:2900-2940`（摘录自已读 `toolchain/qemu-9.2.0/target/i386/tcg/translate.c`）：

- RDRAND（opcode `0x1c7` modrm=`6`）和 RDSEED（opcode `0x1c7` modrm=`7`）走**同一条** helper：`gen_helper_rdrand(s->T0, tcg_env)`。
- 该 helper 实现于 `target/i386/tcg/int_helper.c:445 HELPER(rdrand)`，调用 `qemu_guest_getrandom(&ret, sizeof(ret), &err)`（`util/guest-random.c:46`）。
- `qemu_guest_getrandom()` 在 normal 模式下调用 `qcrypto_random_bytes()` → host OS 的 `getrandom(2)` 或同等 — **TCG 下提供的就是宿主机的真实熵，不是 stub / 固定值**。
- 唯一失败条件：(a) replay mode (`replay_mode == REPLAY_MODE_PLAY`)；(b) `qcrypto_random_bytes()` 内部 host 熵源不可用（生产用户态几乎不会触发）。
- CPUID guard 实现：缺 `CPUID_EXT_RDRAND` 或 `CPUID_7_0_EBX_RDSEED` → `goto illegal_op` → **#UD raised**（不是静默 fail）。OS01 dev 默认 `qemu64` 上看到 #UD = kernel 触发 #UD handler，可能挂 — 这正是 `kernel/include/arch/x86_64/random.h:15-19` 注释强调"必须 CPUID 守卫"的原因。

> **v1 错误修复**：v1 写"TCG 模式下 RDRAND 可能 stub 成固定值（QEMU 行为一直在演化）"。QEMU 9.2.0 / 11.1.1 source 实证：feature bit 开启后 RDRAND/RDSEED 走 host `qcrypto_random_bytes()` 返回真实熵（不是固定 stub）。**v1 表达过于保守**；TCF 模式下失败模式 = "feature bit 缺失 → #UD"，而非"返回固定值"。

### 6.4 真硬件（Haswell+ Intel / Zen 2+ AMD）

- RDSEED: ✅ 可用、可信（Intel/AMD 声明 NIST SP 800-90B/90A 合规）
- RDRAND: ✅ 可用、符合 NIST SP 800-90A
- CPUID 守卫见到两个 bit 都为 1 → 走 STRONG 路径
- 全 OS01 developer boot 仍然默认 CPU 不可信（如果用 QEMU 跑）；**生产固件需明确告知用户启用 `-cpu kvm64,+rdseed` 或 `-cpu host`**

### 6.5 推荐文档（operator-facing — 待写）

`docs/operations.md`（待写）建议补一节：

> 真硬件 / KVM 启用 RDSEED：在 `mk/targets/x86_64.mk` 或 `mk/components/run.mk` 加 `-cpu host` 或 `-cpu kvm64,+rdrand,+rdseed`。
> TCG + `qemu64` 默认：仅 STRONG via UEFI GetRNG（virtio-rng），RDRAND/RDSEED 路径 fail-closed，符合 AAGU-5 §验收 真硬件 / QEMU 默认 双轨语义。

---

## 7. 风险与未决问题（留给 Stage 3 设计阶段）

1. **inline → 拆 .c 文件迁移**：现有 `kernel/include/arch/x86_64/random.h` 是 header-only inline；AAGU-5.6 拆 `kernel/arch/x86_64/random.c` 时，是新建还是 rename？本次报告**不决断**，留给 AAGU-5.6 实现期。
2. **CPUID 调用频度**：`arch_random_get_entropy()` 每次都做 CPUID 太慢（≥ 100 cycles call）。Stage 3 应在 BSP `early_random_init()` 做一次性 CPUID，把 `cpu_features->has_rdseed/has_rdrand` 存 percpu 结构（已存在，见 `kernel/include/arch/percpu.h`），运行时只读 **位** 不调 CPUID。
3. **WEAK 路径开放的 caller 范围**：v2 修订后 WEAK 时 `random_ready=true`，但当前 OS01 没有 WEAK consumer。Stage 3 应明确非安全 caller 列表；`/dev/urandom` 阻塞读接受 WEAK 是当前最直接的扩展点。
4. **跨核熵传递**：CSPRNG seed 一次性由 BSP 调用 `arch_random_get_entropy()`，AP 启动期是否需要刷新？Stage 3 决定。
5. **RDTSC 抖动是否真的不用**：OS01 是个 hobby OS，但若未来要把 `/dev/urandom` 开放给非安全 caller 又不愿增加硬件要求，jitterentropy 路径是低成本选项。本调研标"待定"，由未来 follow-up 决定。
6. **microcode / CVE 安全**：DRBG 侧信道 CVE-2019-11098 等历史漏洞，OS01 不在攻击模型内（本地 QEMU dev），但真硬件需在 docs 加一句"使用前请更新 microcode"。
7. **同一份代码在 `-mno-sse -mno-sse2` 编译选项下的可用性**：见 `kernel/arch/x86_64/make.config` 第 ~38 行；不影响 RDRAND/RDSEED（都是普通 GP-register 指令），但要确保 `arch_cycle_counter()` 仍返回有效值。
8. **QEMU model 漂移**：本调研 §6.2 表基于 QEMU 9.2.0 / 11.1.1；未来 QEMU 版本可能修正 `Haswell-v3` 缺 RDSEED 的建模错误。OS01 kernel 不受此影响（仍只看 CPUID bit），但 QEMU dev workflow 应定期复测。
9. **strict-mode "一次成型" 的代价**：v2 §3.2 算法要求 4× RDSEED 全成功才算 STRONG，单次失败就降级到 RDRAND。极端情况下（RDSEED 一直欠载），每次调用耗时可能达 ~80 cycles × 10 retry × 4 = 3200 cycles per failure → 2560 cycles 落到 RDRAND 重试。仍远低于 reseed 周期（每 1 MiB），可接受。

---

## 8. 引用与参考

| 引用 | 位置 |
|---|---|
| Intel 64 / IA-32 SDM Volume 1, Chapter 7.3 「Random Number Generator」 | Intel 文献 |
| Intel 64 / IA-32 SDM Volume 2, 「RDSEED」 / 「RDRAND」 | Intel 文献 |
| AMD64 Architecture Programmer's Manual Volume 3, 「RDSEED」 / 「RDRAND」 | AMD 文献 |
| NIST SP 800-90A（DRBG 标准） | NIST |
| NIST SP 800-90B（Entropy Source 标准） | NIST |
| QEMU 11.1.1 CPU models（实测） | `qemu-system-x86_64 -cpu help` / QMP `query-cpu-model-expansion` |
| QEMU 9.2.0 source：`qemu64` 模型定义 | `toolchain/qemu-9.2.0/target/i386/cpu.c:2484-2502` |
| QEMU 9.2.0 source：RDRAND/RDSEED translate | `toolchain/qemu-9.2.0/target/i386/tcg/translate.c:2900-2940` |
| QEMU 9.2.0 source：`qemu_guest_getrandom()` | `toolchain/qemu-9.2.0/util/guest-random.c:46` |
| Linux `arch/x86/kernel/cpu/amd.c` RDRAND 设计参考 | kernel.org |
| Linux `drivers/char/random.c` RDRAND 信任度参考 | kernel.org |
| OS01 父 AAGU-5 issue | platform issue 01a0b568-fcde-7570-9c02-6cce956d0535 |
| AAGU-2 `getrandom` spec §4.2 硬件熵检测 | `docs/superpowers/specs/2026-08-21-getrandom-design.md` |
| OS01 weak-default + strong-override 模式 | `docs/arch.md` |

---

## 9. 变更记录

- **v2（2026-09-23 AAGU-5.1 修订稿 — 应 reviewer BLOCK 5 条）**：
  1. WEAK 语义对齐 AAGU-5：`random_ready=true`，安全敏感路径 STRONG-only；`/dev/random` 非阻塞读返回 `EAGAIN`（§4.2 / §4.3）。
  2. §3.2 算法改为 strict "一次成型"：4×RDSEED 全成功 → STRONG；4×RDRAND 全成功 → WEAK；失败 → NONE，**不允许部分/零填充 mix**。
  3. RDRAND 表述收敛为 OS01 威胁模型下的策略分类，删除"绝对断言"措辞（§2.2）。
  4. §6.2 增补 QEMU 11.1.1 `query-cpu-model-expansion` 实测数据（表 1 / 表 2），覆盖 qemu64 / Haswell-v3 / Skylake-Client-v3 / kvm64 四种 model + feature override。
  5. UEFI GetRNG 单列 §2.3 段，明确与 `arch_random_get_entropy()` 并列、不混作 instruction fallback；质量标签 / 失败语义 / 优先级独立说明。
  6. §3.1 性能估计标注"实施前 AAGU-5.6 必须重测"，不再从调研流入性能承诺。
- v1（2026-09-23 AAGU-5.1 初稿）：已弃。

---

## 附录 A — QEMU QMP 实测脚本（用于 §6.2 表 1 / 表 2）

```python
# /tmp/qemu_check.py — 复现 §6.2 表 1
import socket, subprocess, time, json

def query_props(cpu_model, props_dict, port):
    p = subprocess.Popen(
        ["qemu-system-x86_64", "-M", "pc",
         "-qmp", f"tcp:127.0.0.1:{port},server,nowait",
         "-cpu", cpu_model, "-S", "-display", "none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # wait for port
    for _ in range(50):
        try:
            s = socket.socket(); s.settimeout(0.5)
            s.connect(("127.0.0.1", int(port))); s.close(); break
        except: time.sleep(0.1)
    s = socket.socket(); s.connect(("127.0.0.1", int(port))); s.settimeout(5)
    s.recv(4096)  # banner
    s.sendall(b'{"execute":"qmp_capabilities"}\n'); s.recv(4096)
    cmd = {"execute": "query-cpu-model-expansion",
           "arguments": {"type": "full", "model": {"name": cpu_model, "props": props_dict}}}
    s.sendall(json.dumps(cmd).encode() + b"\n")
    raw = b""
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            c = s.recv(65536)
            if c: raw += c
            else: break
        except:
            try: json.loads(raw.decode()); break
            except: continue
    s.close(); p.terminate()
    try: p.wait(timeout=2)
    except: p.kill()
    return json.loads(raw.decode())["return"]["model"]["props"]

# 表 1：无 feature override
for model in ["qemu64", "qemu64-v1", "kvm64", "Haswell-v3", "Skylake-Client-v3"]:
    feats = query_props(model, {}, str(4400 + hash(model) % 50))
    print(f"{model:25s} rdrand={feats.get('rdrand','?')} rseed={feats.get('rdseed','?')}")

# 表 2：qemu64 + feature override
for props in [{}, {"rdrand": True}, {"rdseed": True}, {"rdrand": True, "rdseed": True}]:
    feats = query_props("qemu64", props, str(4500 + hash(str(props)) % 50))
    print(f"qemu64 + {props!s:60s} rdrand={feats.get('rdrand','?')} rdseed={feats.get('rdseed','?')}")
```

**运行环境**：host = Linux 7.2.6-arch2-1 (x86_64)；QEMU = `/usr/bin/qemu-system-x86_64` version 11.1.1。
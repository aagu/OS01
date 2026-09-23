# OS01 熵质量治理（STRONG / WEAK / NONE）

> **For agentic workers:** 本规范是 OS01 内「熵质量分级 + 路径 → 质量要求」的**单一治理来源**。所有安全敏感路径（AT_RANDOM / canary / KDF / cookie / nonce）必须按本规范的判定表选择 STRONG / WEAK / NONE 等级；任何变更（新增 STRONG 源、改变 WEAK 接受范围、放宽 NONE fail-closed）必须先改本规范再改代码。本规范**不重复造轮子**：CSPRNG 算法、pool 锁、reseed policy 见 `docs/superpowers/specs/2026-08-21-getrandom-design.md`（AAGU-2）；arch override 实现细节见 AAGU-5.6 实施 PR；x86_64 硬件熵源调研见 `docs/random/entropy-source-survey-x86_64.md`（AAGU-5.1）；多 arch weak-default + strong-override 总论见 `docs/arch.md`。

| 字段 | 值 |
|---|---|
| Spec ID | `entropy-quality` |
| Issue | AAGU-5.4 |
| 状态 | Draft v1（待 AAGU-5.6 实施期对齐） |
| 最后更新 | 2026-09-23 |
| 适用范围 | kernel + libc + userspace startup（任何消费 `get_random_bytes()` / `getrandom(2)` / `/dev/random` / `/dev/urandom` 的站点） |
| 父 issue | [AAGU-5 (arch-neutral 熵采集)](https://multica.ai) |
| 子 issue | AAGU-5.6（x86_64 / aarch64 strong override 实施）/ AAGU-5.7（AT_RANDOM 联调） |

---

## 1. 适用范围与不重复造轮子

本规范回答**「哪些路径必须 STRONG、哪些允许 WEAK、NONE 出现时谁必须停」**，不回答以下问题：

| 问题 | 去向 |
|---|---|
| ChaCha20 block function 算法本身、ChaCha20 keystream counter / nonce 拆分 | `docs/superpowers/specs/2026-08-21-getrandom-design.md` §3、§4.3 |
| pool spinlock 取放顺序、`pool_lock` 与 `mm->lock` 嵌套、`get_random_bytes` 分块放锁 | `docs/superpowers/specs/2026-08-21-getrandom-design.md` §4.4、§5.1 |
| 周期 reseed 触发条件（每 1 MiB 输出）、ChaCha20 generate-then-rekey 派生新 key | `docs/superpowers/specs/2026-08-21-getrandom-design.md` §4.3；本规范不重新审视 reseed 触发时机 |
| `/dev/random` 写入 fold（XOR accumulator） | `docs/superpowers/specs/2026-08-21-getrandom-design.md` §4.3 后段；本规范只回答「写入能否提升 quality」——见 §4 |
| x86_64 上 RDSEED / RDRAND / UEFI GetRNG / VMX jitter 的具体评级与 QEMU 实测 | `docs/random/entropy-source-survey-x86_64.md` §2、§6 |
| aarch64 上 RNDR / RNDRRS / UEFI GetRNG / cycle counter 的具体评级 | AAGU-5.6 实施期补（独立 spec，遵循本规范的 §2 等级语义） |
| arch-neutral facade + strong override 总论（weak-default 模式、SUBSYS_INITCALL） | `docs/arch.md` §「weak-default + strong-override」 |
| `__stack_chk_guard` / `__stack_chk_fail` 唯一定义点选择（kernel vs libc 二选一） | `docs/arch/cross-boundary-symbols.md` §2.1、§3.1 |

**本文档**约束路径选择与失败语义；不约束实现细节。

---

## 2. 三级质量标签定义

`kernel/include/arch/random.h`（AAGU-5.6 落地）声明枚举：

```c
typedef enum {
    ARCH_ENTROPY_NONE  = 0,
    ARCH_ENTROPY_WEAK  = 1,
    ARCH_ENTROPY_STRONG = 2,
} arch_entropy_source_t;
```

每级的 OS01 语境定义如下。**这些定义是治理契约**——任何 `arch_random_get_entropy()` 的实现都必须严格映射到这一组标签，不能引入第四级，也不能把已有等级再切分（如 "STRONG-with-audit-trail" 不存在）。

### 2.1 `ARCH_ENTROPY_STRONG` — 可作 CSPRNG seed 的 raw entropy

**定义**：来自 ISA 公开声明的「非条件化熵源」（conditionable entropy source），输出可直接作为 CSPRNG seed material，无需任何软件后处理。OS01 把「可作 CSPRNG seed」作为 STRONG 的判定标准——因为 CSPRNG（ChaCha20）的种子一旦熵不足，后续 keystream 即整体可预测，**这是唯一的不可逆失败路径**。

**满足 STRONG 的源**（实施期须以本表为准，不在调研报告中重新评估）：

| 源 | 平台 | 备注 |
|---|---|---|
| `RDSEED` (`rdseed r64`) | x86_64（Intel Broadwell+ / AMD Zen 2+） | Intel SDM Vol. 1 §7.3.1.2 明文："a seed source suitable for use in seeding an arbitrary-length PRNG"。OS01 决策树先尝试 RDSEED（详见 `entropy-source-survey-x86_64.md` §4.1） |
| `RNDR` / `RNDRRS`（AArch64 RND/RNG 寄存器） | aarch64 | AAGU-5.6 实施期评估并写入本表 |
| UEFI `EFI_RNG_PROTOCOL` raw output | 双 arch bootloader 通道 | 写入 `boot_entropy[32]`，经 `BOOT_CONTEXT_HAS_BOOT_ENTROPY` 标志位传给 kernel；标签取决于 firmware——QEMU virtio-rng 由宿主机熵源提供、真硬件 TPM/RNG passthrough 符合 NIST SP 800-90A/B |

**不满足 STRONG 的源（即使看起来是"硬件熵"）**：

- `RDRAND`：Intel 公开声明 "Suitable for high-quality cryptographic key generation"，但其内部实现是 AES-128 CTR_DRBG（AES-128 安全强度 128 bit）；OS01 把 STRONG 留给 "明确可证 seed-quality" 的源——RDRAND 是 "已加工随机数" 而非 "raw entropy"。**归 WEAK**（详见 §2.2）。
- 任何 cycle counter 派生（`rdtsc` / aarch64 `cntvct`）、jitter accumulator、TSC delta 统计：单次读无熵；多次采样的统计估计在 VM 内可信度进一步退化。**归 WEAK 或 NONE**（详见 §2.2 / §2.3）。

### 2.2 `ARCH_ENTROPY_WEAK` — 满足 NIST SP 800-90A 但不可单独作 CSPRNG seed

**定义**：来源本身满足某种 NIST/ISA 标准（如 NIST SP 800-90A DRBG / NIST SP 800-22 统计测试），但不是 "raw entropy"——其输出已经经过硬件 DRBG 的内部条件化处理。在 OS01 威胁模型下，WEAK 输出的不可预测性**只对非密码学用途（PID、hash table perturbation、伪随机 seed）成立**；**安全敏感路径（密钥派生、AT_RANDOM、canary）拒绝接受 WEAK**——理由见 AAGU-5 立项理由明文："WEAK 用于 canary 时 SSP 在 QEMU dev/test 环境实际失效"。

**满足 WEAK 的源**：

| 源 | 平台 | 备注 |
|---|---|---|
| `RDRAND` | x86_64（Intel Ivy Bridge+ / AMD Zen+） | NIST SP 800-90A 合规；内部 AES-128 DRBG；OS01 决策树 RDSEED 失败后取此；详见 `entropy-source-survey-x86_64.md` §2.2 |
| RNDR（aarch64，待评估） | aarch64 | AAGU-5.6 实施期评估；同 RDRAND 思路 |
| `arch_cycle_counter() ^ jiffies` 混合 | 双 arch | **仅作最后手段**；AAGU-5 §3 要求"删除软件伪熵 fallback"，AAGU-5.6 落地后此源可能直接消失——本规范保留为"未来抖动熵若实现则归 WEAK"的位置 |

**为什么 WEAK 不"够用"**：WEAK 是 "已加工"，意味着同一颗 CPU 实现的 DRBG 内部状态受硬件设计支配——存在 microcode 漏洞历史（CVE-2019-11098 / 2019-11157 等）、存在硬件实现的私有性（验证依赖 ISA 公开声明而非逆向）。OS01 dev 场景的攻击模型不涵盖这些，但生产固件应在部署前更新 microcode。**STRONG ≠ 不可能出错；STRONG = 我们的威胁模型下不可被预测**。WEAK 路径在 OS01 严格保留给非密码学用途。

### 2.3 `ARCH_ENTROPY_NONE` — 无可信熵；fail-closed 总触发

**定义**：`arch_random_get_entropy()` 调用**完全失败**——CPUID bit 不存在（QEMU 默认 `qemu64` 模型无 RDRAND/RDSEED）+ UEFI GetRNG 未提供 + bootloader 没拿到 `boot_entropy` + cycle counter fallback 也被 AAGU-5.6 删除。NONE 出现意味着**整个 boot 没有任何 hardware-trusted entropy**。

**后果（fail-closed 总触发器）**：以下**全部**子系统在 NONE 期间必须停止推进，**不接受"低优先级路径继续"的折衷**：

1. `kernel/random/random.c::random_ready = false`——`get_random_bytes()` 对所有 caller **memset 返回 buffer 为 0**（参见 `kernel/random/random.c:160-170` 已存在的 fail-closed 实现）。
2. **安全敏感路径**（STRONG-only 路径，见 §3.1）**全部 fail-closed**——AT_RANDOM 路径返回错误让 spawn/exec 决定重试或阻塞；canary 派生走 fail-closed → `__stack_chk_fail` → 进程 abort（`waitpid status = 6`，systest case 43 观察）；KDF / cookie / nonce 派生返回错误。
3. **非安全路径**（WEAK-allowed 路径，见 §3.2）在 NONE 期间**也**全部 fail-closed——理由：NONE 状态下 `get_random_bytes()` 已 memset 0，所有 caller 拿到的 buffer 全零，**让 caller 误以为"成功"是更糟的失败**。让 caller 显式失败比拿到看似合法但全零的 PID 更安全。
4. **boot 阻塞**：`kernel_main` 在 `random_init()` 返回 `random_ready=false` 后**继续**走完（不阻塞 boot），但**必须在 boot log 中显式打印 NONE 状态**（见 §4）——失败可见性是 OS01 dev / QEMU 默认环境的常规可观测性诉求。

**关键不变量（NONE 期间的所有 caller 必须满足）**：

- **没有 caller 接受 NONE**——`get_random_bytes()` 的 memset(0) 是 caller 与 `__stack_chk_guard == 0` / `__stack_chk_fail()` 检测的契约；调用方检查全零 buffer 并显式 abort。
- **没有 caller 把 NONE 当 "正常 fallback"**——任何 `if (entropy == NONE) { use_deterministic_value(); }` 形式代码是违例（详见 §5 判定标准）。
- **没有 caller 跨 quality 边界**——A 路径拿到 STRONG 不代表 B 路径也能拿到 STRONG；每个 call site 独立调用 `arch_random_get_entropy()` 或独立检查 `random_ready`。

---

## 3. 路径 → 质量要求表

> **使用方式**：每个 call site 在写代码前查本表，确定"STRONG only / WEAK allowed / NONE fail-closed"，再选实现路径（直接 `arch_random_get_entropy()` / 走 `kernel/random` pool / `getrandom(2)` syscall）。

### 3.1 STRONG only（拒绝 WEAK / NONE）

| 路径 | 位置 | 接受等级 | 失败语义 |
|---|---|---|---|
| AT_RANDOM payload（`setup_user_stack()`） | `kernel/sched/task.c:1178-1182`（调用 `get_random_bytes(KSTACK(rsp), 16)`） | **STRONG only** | WEAK/NONE → `get_random_bytes()` 返回全零 buffer → spawn/exec 检测 AT_RANDOM 16B 全零 → spawn 失败返回 -EIO；exec 路径由 `spawn_user_task` 决定是阻塞 / 重试 / 失败（**AAGU-5.7 联调落地**） |
| libc 端 `__stack_chk_guard` 派生 | `libc/csu/csu.c:49-59`（exec/spawn 单站点；fork 不经过这里） | **STRONG only** | WEAK/NONE → `getrandom(&guard, 8)` 返回全零 → `__stack_chk_guard == 0` 检测命中 → `__stack_chk_fail()`（noreturn, SIGABRT）；`waitpid status = 6`，systest case 43 可观察 |
| kernel 端 `__stack_chk_guard` 派生 | `kernel/core/main.c:81`（`arch_cycle_counter() ^ 0xDEADBEEFCAFEBABE`，`kernel_main` 第一条语句） | **STRONG only** | 当前用 cycle counter —— 已知**弱点**（可预测），**待 AAGU-5.6 落地后改为 STRONG 源**（redmine: 把 kernel canary 也走 `get_random_bytes()` / `arch_random_get_entropy()`，要求 `random_ready=true`） |
| 任何 KDF / 密钥派生（HMAC 输入、PBKDF2、HKDF） | **当前 OS01 不存在**（grep `crypto_*` / `kdf_*` / `*_secret` / `*_cookie` 在 kernel / libc / user 零命中）—— 本规范作 forward-looking | **STRONG only** | 不接受 WEAK：密钥派生输出若可预测即完全失效 |
| 任何 nonce / IV 生成 | 同上 forward-looking | **STRONG only** | nonce 重用即丧失语义安全；WEAK 不可接受 |
| cookie / token / CSRF / session ID 生成 | 同上 forward-looking | **STRONG only** | 同 nonce |
| mmap ASLR seed（OS01 当前**未实现**） | `kernel/memory/vma.c:127` `mm->mmap_base = 0x40000000`（**固定值，零 ASLR**） | STRONG only（**未来实现时**） | 待 OS01 落地 ASLR 后由本规范约束种子源 |

**判定标准**（违例 = 满足任一条）：

- 上述 STRONG-站点**显式接受** WEAK / NONE 等级（如 `if (quality >= WEAK) accept;` 而非 `== STRONG`）。
- STRONG-站点用 cycle counter / jiffies / 任何软件派生 fallback（如 kernel canary 当前的 `arch_cycle_counter()`）。
- 上述 STRONG-站点在 NONE 状态下**不检测、不 abort**——继续走完、返回看似合法但实际全零的 buffer 给用户态。
- 新增"密码学用途"路径（HMAC / 签名 / 加密 / session token）未走 `arch_random_get_entropy()` STRONG 路径或未在 §3.1 表中显式登记。

### 3.2 WEAK allowed（允许 STRONG / WEAK，拒绝 NONE）

| 路径 | 位置 | 接受等级 | 失败语义 |
|---|---|---|---|
| PID 生成 | `kernel/sched/task.c:1284` / `:1937`：`atomic_fetch_add((volatile uint64_t *)&pid_counter, 1)` | **当前使用确定性 counter**（**不接受任何等级**）—— 待未来 AID 评估 | 当前 PID 是 1, 2, 3... 顺序递增，**不**消费熵——本规范**不**要求改造 PID，但若未来改为熵派生，则必须**至少 WEAK**；STRONG 也接受 |
| 伪随机 seed（非密码学用途） | **当前 OS01 不存在**—— future 任意 "种子化" 路径（如 cache eviction 模拟、模拟器种子） | WEAK+ | NONE → caller 显式失败 |
| hash table perturbation | **当前 OS01 不存在**（grep `hash` 仅有 hash 函数定义本身，无独立 hash table 实现的 perturbation 需求） | WEAK+ | NONE → caller 显式失败 |
| `/dev/urandom` 阻塞读 | `kernel/fs/devfs.c::random_read` 共享 `random_ops` | **WEAK+** | 阻塞读永远可读（Linux 语义）；WEAK 模式也返回数据；NONE 模式 → `get_random_bytes()` memset(0) → 用户态拿全零 buffer（**用户态需自行检测**，OS01 不主动抛错） |

**判定标准**：

- WEAK-allowed 站点**不得**在 NONE 状态下继续走"确定性 fallback"（如"entropy 不够就用 `pid_counter` 凑数"）。
- WEAK-allowed 站点不得升级为密码学用途——若用途变化（如 PID 改为权限判断依据），必须先迁出本表到 §3.1 并改为 STRONG only。

### 3.3 NONE 应该 fail-closed 的总触发（不可绕过）

NONE 状态下，**§3.1 全部站点 + §3.2 全部站点** 走同一 fail-closed 路径：

| 触发器 | 行为 |
|---|---|
| `arch_random_get_entropy()` 返回 `ARCH_ENTROPY_NONE` | `kernel/random/random.c::random_init()` 不设 `random_ready=true`；`get_random_bytes()` 在所有 caller 入口 memset(0)；`/dev/random` 与 `/dev/urandom` 都返回全零 buffer |
| `kernel/random/random.c::random_is_ready() == false` | 同上——所有 caller 走 fail-closed 路径 |
| `getrandom(2)` 返回全零 buffer（用户态） | libc 端与 kernel 端调用方按各自检测逻辑处理（canary 检测、AT_RANDOM 检测） |
| `/dev/random` 非阻塞读 | 详见 §5.2 |

**NONE 状态下禁止的行为**（**判定标准 — 违反即拒绝合并**）：

- 任何 caller 把 NONE 当成 "继续走默认值即可" 的输入。
- 任何 caller 用确定性 fallback（jiffies、cycle counter、atomic counter、hardcoded constant、boot-time RNG seed）替代缺失的 entropy——这是 AAGU-5 立项明文禁止的"软件伪熵 fallback"。
- 任何 caller 跨越 quality 边界——"我看到 A 路径有 STRONG 就假设 B 路径也有"。

---

## 4. 启动期 → STRONG 路径变 WEAK 的处理

### 4.1 降级（启动期）

OS01 启动期 → 运行时可能的降级场景：

| 场景 | 触发条件 | 处理 |
|---|---|---|
| 启动期 UEFI GetRNG 失败 → 走 RDSEED/RDRAND → RDSEED 失败但 RDRAND 成功 | 真硬件 CPU 无 RDSEED 支持（如旧 Haswell 之前） | `random_init()` 走 WEAK 路径；`random_ready=true`；boot log `log_warn "CSPRNG: pool seeded from RDRAND only (no RDSEED/RDRAND both) ..."`；**§3.1 STRONG-only 路径**（AT_RANDOM / canary）必须显式检测并 fail-closed（**AAGU-5.7 落地**） |
| 启动期所有 entropy 源失败 | QEMU 默认 `qemu64`、aarch64 RNDR 未实现、UEFI GetRNG 无 | `random_init()` 走 NONE 路径；`random_ready=false`；boot log `log_warn "CSPRNG: no hardware entropy source ... pool not ready, get_random_bytes will fail-closed"` |

**降级的语义不变性**：

- **不可逆**——一旦 `random_ready=true`，降级到 `random_ready=false` 不可能（除非 `arch_random_get_entropy()` 在运行时也返回 NONE，但 OS01 现有实现不在运行时重新探测；future 状态机若加入运行时探测，必须先改 §4.1 与 §2.3 契约）。
- **可观测**——每次降级必须 log_warn；同一原因不重复刷屏（"log once"），但每次状态变迁必须 log。
- **可验证**——`kernel/random/random.c::random_is_ready()` 是公开的 SELFTEST 入口；KERNEL_SELFTEST 模式下应至少有 1 条 SELFTEST 验证 boot 时 quality 等级与 `random_ready` 一致。

### 4.2 升级（运行时）

**运行时不存在从 WEAK/NONE 升级到 STRONG 的路径**（不可逆的同向要求）。理由：

- 运行时 entropy 等级由启动期 hardware capability 决定——CPUID bit、UEFI GetRNG 是否提供、boot_entropy 是否就绪，这些在 kernel_main 后**基本冻结**（OS01 不支持 hotplug CPU，不支持运行时动态加载 entropy 源）。
- `/dev/random` 写入（`kernel/fs/devfs.c::random_write` → `kernel/random/random.c::random_add_entropy`）**只 XOR-fold 进 pool_key，不触发 quality 升级**——这是 §4.3 的明文契约。
- 周期 reseed（每 1 MiB 输出）混合 `arch_random_get_entropy()` 当前等级：若当前等级 WEAK，reseed 仍是 WEAK；若当前等级 NONE，`random_ready=false`，reseed 不执行（详见 AAGU-2 §4.3）。

### 4.3 `/dev/random` 写入与 quality 升级（明确：否）

**判定 / 契约**：

- `random_add_entropy(buffer, len)`（`kernel/random/random.c:229-237`）把 caller buffer XOR-fold 进 32B `pool_key`（`pool_key[i & 31] ^= p[i]`）。**不**修改 `random_ready`，**不**提升 quality 等级。
- 写入 /dev/random 之前 caller **必须已持有 STRONG 或 WEAK entropy**——"我想让内核变 strong 所以写入更多 entropy" 是不成立的语义。
- `random_ready=false` 时 `random_add_entropy` 直接 no-op 返回（已有实现，`kernel/random/random.c:231`）；用户态写入的 bytes 被静默丢弃，**不**抛错（避免被未授权用户态 DoS）。**待观察**——是否需要 log_warn "discarded /dev/random write because pool not ready"？本期不决断，留给后续 follow-up。

### 4.4 启动日志契约（审计要求）

**所有 quality 等级必须在 boot log 显式打印**，不静默：

| quality | 必须打印的 log 行 |
|---|---|
| STRONG（UEFI GetRNG） | `log_info "CSPRNG: pool seeded from UEFI GetRNG (boot_entropy) — STRONG"` |
| STRONG（RDRAND + RDSEED） | `log_info "CSPRNG: pool seeded from RDRAND/RDSEED — STRONG"` |
| WEAK（RDRAND only / 无 RDSEED） | `log_warn "CSPRNG: pool seeded from RDRAND only — WEAK (no RDSEED); STRONG-only paths will fail-closed"` |
| WEAK（aarch64 RNDR only / 无 RDSEED-equivalent） | 同上模式，源标签替换为 RNDR |
| NONE | `log_warn "CSPRNG: no hardware entropy source — pool not ready, all paths fail-closed"` |

**判定标准**：

- 任何 quality 等级的 boot path **不打印** quality 标签 = 违例（"silent quality"）。
- log 标签的**单源**在 `kernel/random/random.c::random_init()`——**禁止**在 caller 端再单独打印 quality（避免日志发散、不一致）。
- `RANDOM_READY` 日志标签字符串是契约的一部分——AAGU-5.6 / AAGU-5.7 / KERNEL_SELFTEST 共享同一组字符串字面量。

---

## 5. `/dev/random` 非阻塞语义

> **对齐 AAGU-2 既有实现**：本节**不**重新定义 `/dev/random` vs `/dev/urandom` 阻塞语义——仅就 quality 等级变化时的非阻塞语义作明确。

### 5.1 `/dev/random` 与 `/dev/urandom` 共池（OS01 决策）

当前 `kernel/fs/devfs.c:120-148` 的 `random_read` 把两个设备接同一 `random_ops`——**不区分 random/urandom 质量**（参见 AAGU-2 §6："不区分 random/urandom 质量，与'同源'决策一致"）。本规范**不**重新审视该决策。

### 5.2 非阻塞语义表

| quality | `/dev/random` 读 | `/dev/urandom` 读 |
|---|---|---|
| STRONG | 阻塞读返回数据；非阻塞读立即返回 | 阻塞读返回数据；非阻塞读立即返回（同 Linux） |
| WEAK | 阻塞读返回数据；**非阻塞读返回 -EAGAIN** | 阻塞读返回数据；非阻塞读立即返回 |
| NONE | `random_ready=false` → `get_random_bytes()` memset(0)；read 返回 -EAGAIN（devfs 包装层），用户态拿到全零 buffer | 同左——但**用户态拿到全零 buffer**（不主动抛错，让用户**自行检测全零**） |

**为什么 WEAK 时 `/dev/random` 非阻塞读抛 EAGAIN**：AAGU-5 立项明文——"WEAK 用于 canary 时 SSP 在 QEMU dev/test 环境实际失效"。`/dev/random` 是"密码学级"接口的语义承诺：用户态期望拿到 STRONG-quality 数据；WEAK 状态下不让用户态误以为拿到 STRONG（fail-open）而改抛 -EAGAIN 显式拒绝（fail-closed）。

**为什么 WEAK 时 `/dev/urandom` 不抛 EAGAIN**：`/dev/urandom` 在 Linux 语义下是"非密码学用途"接口——jiffies-grade randomness 也算数据；WEAK 满足该语义；阻塞读不阻塞。

### 5.3 落地分工

`kernel/fs/devfs.c` 当前 `random_read` 不区分 quality 等级——`AAGU-5.6` 实施期需在 `random_read` 入口增加 `random_is_ready()` + 当前 quality 判定（**quality 标签需从 `kernel/random/random.c::random_init()` 的本地变量传递**，或新增 `random_current_quality()` getter），按上表返回 -EAGAIN 或继续。本规范**不**约束 devfs 实现细节——只规定语义契约。

---

## 6. 与其他文档 / issue 的边界声明

| 文档 / issue | 关系 |
|---|---|
| `docs/superpowers/specs/2026-08-21-getrandom-design.md`（AAGU-2） | **CSPRNG 算法、pool 锁、reseed policy、/dev/random write fold 契约、用户 buffer mm->lock 保护**——本规范不重复；本规范只回答"quality 等级变化时的语义" |
| `docs/random/entropy-source-survey-x86_64.md`（AAGU-5.1） | **x86_64 硬件熵源调研**——具体源的 QEMU 11.1.1 实测、TCG 路径、microcode 风险等；本规范不重复；本规范只回答"STRONG / WEAK / NONE 各接受哪些源" |
| AAGU-5.6 实施 PR | **arch_random_get_entropy() 强覆盖实现**——x86_64 / aarch64 的 `kernel/arch/<arch>/random.c` 新建；本规范不约束实现细节 |
| AAGU-5.7 AT_RANDOM 联调 | **§3.1 AT_RANDOM 行的具体 fail-closed 处理**——spawn/exec 路径的 -EIO / 阻塞 / 重试决策；本规范只规定 "STRONG-only, 失败显式返回错误" |
| `docs/arch.md` | **多 arch weak-default + strong-override 总论**——`arch_random_get_entropy()` 是该模式的一个新实例；本规范不重新讲 weak-default |
| `docs/arch/cross-boundary-symbols.md` | **`__stack_chk_guard` / `__stack_chk_fail` 唯一定义点选择**——kernel `kernel/compiler_rt/` vs libc `libc/ssp/ssp.c` 二选一；本规范不重复 |
| `docs/superpowers/specs/2026-09-11-x86_64-kernel-ssp-design.md` | **kernel 端 canary 实现历史**——`rdtsc()` 派生 guard 的做法；本规范要求 AAGU-5.6 落地后改为 STRONG 源 |
| `docs/arch/multi-arch-weak-default.md` | **历史 weak-default 总论**——本规范不复述该模式；本规范的 §2.3 NONE fail-closed 是该模式的"全无 arch 实现"分支 |
| AAGU-1（OS01 代码质量评审） | **P0-1 fail-closed 语义钩子**——本规范建立在该钩子之上 |

---

## 7. 验收对照（AAGU-5.4 验收条目）

- [x] `docs/random/entropy-quality.md` 已落地（本文件）
- [x] 含三级质量标签定义（§2.1 STRONG / §2.2 WEAK / §2.3 NONE，每级 OS01 语境精确）
- [x] 含路径 → 质量要求表（§3.1 STRONG-only 覆盖 AT_RANDOM / kernel+libc canary / KDF / cookie / nonce / future ASLR；§3.2 WEAK-allowed 覆盖 PID / pseudo-RNG / hash perturbation；§3.3 NONE 是 fail-closed 总触发的明确声明）
- [x] 含 `/dev/random` 非阻塞语义（§5.2，对齐 AAGU-2 既有 §4.2 决策）
- [x] 含质量等级在启动日志必须打印的审计要求（§4.4 + §5 判定标准）
- [x] 含与 AAGU-2 / AAGU-5 / 既有 spec 的边界声明（§1 / §6 两层声明）
- [x] 含降级 / 升级路径契约（§4.1 / §4.2 不可逆；§4.3 `/dev/random` 写入不提升 quality）
- [x] NONE 状态 fail-closed 触发器明文（§2.3、§3.3，含禁止的 fallback 形式）
- [x] 不写代码（无 `kernel/` 下任何改动；仅 `docs/random/entropy-quality.md` 一份新文件）
- [x] 不重复造轮子（§1 链接 AAGU-2 / AAGU-5.1 / arch.md / cross-boundary-symbols.md，不复述）

**AAGU-5 帖 comment 任务**（验收条目最后一项）：本规范落地后于 AAGU-5 帖一条 comment，附 `docs/random/entropy-quality.md` 路径 + 一句话总结。
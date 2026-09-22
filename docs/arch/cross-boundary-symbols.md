# OS01 跨边界符号 / ABI 边界规范

> **For agentic workers:** 本规范是 OS01 内「跨边界符号 + ABI 边界」的**单一来源**。所有未来的边界治理 follow-up issue 必须引用本规范，并在落地后回头更新 §3 现状对照表。本规范**不重复造轮子**：多 arch 抽象的「weak-default + strong-override」总论见 `docs/arch.md`，本 spec 只补 cross-boundary 部分。

| 字段 | 值 |
|---|---|
| Spec ID | `cross-boundary-symbols` |
| Issue | AAGU-4 |
| 状态 | Draft（待 review） |
| 最后更新 | 2026-09-22 |
| 适用范围 | kernel + libc + runtime + 用户态 startup |

---

## 1. 适用范围与不重复造轮子

OS01 现有 `docs/arch.md` 已经定义了**多 arch 抽象层**的总体模式（weak-default + strong-override），本规范不复述。本规范把同一架构理念应用到**跨边界符号**这一更窄的类别：

| 类别 | 已有规范 | 本规范补什么 |
|---|---|---|
| 跨 arch 抽象（facade + override） | `docs/arch.md` | 不复述，仅在 §3.3 引用 |
| 跨**符号边界**（kernel ↔ libc ↔ userspace ↔ 编译器 runtime） | **本规范** | 4 类规则 + 现状对照 |
| 跨 arch **entropy 采集**质量分级 | （AAGU-5 backlog） | 不复述；§4 引用并提示由 AAGU-5 落地 |

**前提**：本文档不引入新的镜像层（"那我们就搞个 `libc/include-os01/` 镜像 kernel" 是把问题外包，**禁止**）。

---

## 2. 边界规则（4 类）

### 2.1 builtin / compiler runtime 符号（`__udivti3`、`__stack_chk_guard`、`__divti3`、`__clzsi2` 等）

**规则**：
- 唯一定义点选择：`kernel/compiler_rt/<name>.c` 或 `libc/<libname>/<name>.c`，**二选一**。
- 落点决策依据：
  - kernel freestanding 路径 → `kernel/compiler_rt/`（**单一 TU，由 `KERNEL_C_SOURCES` 编进 `kernel.bin`**）
  - selfhosted runtime archive 路径 → `runtime/builtins/<name>.c`（编进 `runtime/udivti3.o` 之类 archive，与 kernel 链接时作为外部输入）
  - libc 用户态 → `libc/<libname>/<name>.c`
- 禁止：
  - 散落到 `kernel/arch/<arch>/` 下当作 arch 差异
  - 散落到 kernel 通用层（`kernel/<subsys>/*.c`）
  - 在 `libc/` 与 `kernel/compiler_rt/` 同时存在同一符号定义（即便 ABI 一致，也是违例）

**判定标准**（违反 = 满足任一条）：
- `kernel/compiler_rt/` 与 `libc/` 同时存在同名的 builtin 实现
- `kernel/arch/<arch>/` 下出现 builtin 类符号定义（编译器隐式调用、不是 arch API）
- `kernel/<subsys>/*.c` 出现 builtin 符号定义

### 2.2 跨用户 ABI（UAPI：auxv 常量、syscall 号、共享结构体）

**规则**：
- **单一源**：`kernel/include/uapi/<topic>.h`
- 安装机制：kernel UAPI 头由 build 步骤安装到 libc sysroot（Makefile 步骤，参见 `kernel/Makefile:106` x86_64 sysroot 路径）
- libc 头**不再**通过 include-path 优先级伪造 libc ABI
- 任何 libc 端「我也要一份」的镜像视为违例（无论 ABI 是否一致）

**判定标准**（违反 = 满足任一条）：
- `libc/include/` 下的头包含 `AT_*` 常量 / syscall 号 / 共享结构体定义，且**未通过 sysroot 安装的 uapi 头**取得（典型违例：`libc/include/sys/auxv.h`）
- `kernel/include/uapi/` 与 `libc/include/` 下同名 ABI 头**常量集不一致**
- 在 kernel 通用层或 libc 用户态源文件出现 `#ifdef __KERNEL__` / `#ifdef BUILDING_LIBC` 之类的 include-guard 分支来「选」头

### 2.3 arch value（AT_PLATFORM、HWCAP bits、x86_64-specific auxv entries）

**规则**：
- 模式：**arch-neutral facade + per-arch strong override**（详见 `docs/arch.md`）
- facade：`kernel/include/arch/<topic>.h`（仅声明）
- strong override：`kernel/arch/<arch>/<topic>.c`（实现）
- arch-neutral builder（`setup_user_stack`、`arch_register_subsys`、`pmm_init` 之类）**只排版，不出现 arch 字符串**
- **禁止**：
  - 在 arch-neutral 源文件用 `#ifdef __x86_64__` / `#ifdef __aarch64__` 分支塞 arch-specific 值（典型反模式，参见 `kernel/sched/task.c` 老版本 `AT_PLATFORM` 硬编码）
  - arch-neutral builder 内含 `"x86_64"` / `"aarch64"` 字面量字符串

**判定标准**（违反 = 满足任一条）：
- arch-neutral 源文件（`kernel/sched/`、`kernel/memory/`、`kernel/intr/`、`kernel/core/`、`kernel/sync/` 等）出现 `#ifdef __x86_64__` 守护 arch-specific 值
- arch-neutral builder 内出现 `arch_auxv_platform` 以外的 arch-specific payload 字符串
- 新增 HWCAP bit、auxv entry 而**不**走 `kernel/include/arch/auxv.h` facade

### 2.4 libc API 镜像（`compat/list.h`、`compat/rbtree.h` 等）

**规则**：
- 真正共享的 freestanding primitive → **单一 kernel-owned 头，不要镜像**
- 绝对不能镜像的（如 `vprintf` 系、有 libc 状态机依赖的）：kernel 不该用，**直接拒绝**（编译器报错或 API 撤回）
- 镜像必须有的（kernel freestanding 路径必须用）：禁止在两个目录同时存在
  - **build 步骤 install 模式**：单一来源在 kernel 一侧，通过 build 步骤把头 install 到 libc sysroot；`#include` 一处生效
  - **合并模式**：kernel 直接 include libc 的头（前提：kernel 已声明 freestanding 并由 libc 提供该头）
- 不允许「include-path 优先级伪造 libc ABI」——这是 aarch64 `-I libc/include` 兼容镜像层留下的隐患（参见 `docs/aarch64-libc-include-policy-closure-2026-09-18.md`）

**判定标准**（违反 = 满足任一条）：
- `kernel/include/compat/<name>.h` 与 `libc/include/<name>.h` 同时存在，且头注释没有「由 build 步骤 install」「自动生成」「指向单一来源」标识
- 头注释里出现 "FROZEN, do not modify without updating both" 之类**人工同步约定**（这是违例的活证据）
- 新增镜像时既不写「单一来源」也未写「安装机制」

---

## 3. 现状对照表（截至 2026-09-22，commit `c5e730b`）

> 每一行格式：**类别 / 位置 / 现状 / 说明**。`现状` 取值：
> - `✅ 修` — 已符合本规范
> - `🟡 部分` — 主线已合规，但有边缘遗留
> - `❌ 违例` — 不符合本规范，需要后续 issue 落地

### 3.1 builtin / compiler runtime 符号

| 类别 | 位置 | 现状 | 说明 |
|---|---|---|---|
| `__stack_chk_guard` | `libc/ssp/ssp.c:12`（唯一定义）；`kernel/core/main.c:63/77`（kernel 端独立定义） | 🟡 部分 | libc 端唯一定义点合规；但 kernel 在 `libk.a` 构建时**故意排除**（`ssp.c:10 #if !defined(__is_libk)`），靠人工 `#define __is_libk` 区分。kernel + libc 各一个定义，靠 build flag 避免双定义——**靠约定不靠强约束**。建议落地方向：把 kernel 端的 `__stack_chk_guard` 移到 `kernel/compiler_rt/`（与 `__stack_chk_fail` 配套），定义一次；libc 端继续按 `__is_libk` 排除（参见 `docs/superpowers/specs/2026-09-11-x86_64-kernel-ssp-design.md`）。 |
| `__udivti3` | `kernel/compiler_rt/udivti3.c`（KERNEL_C_SOURCES 一员）+ `runtime/builtins/udivti3.c`（selfhosted runtime archive） | ✅ 修 | 两份实现**服务于不同 build 路径**：freestanding 内联 vs selfhosted archive。两者均单一来源（kernel path 单一 TU；runtime path 单一 archive），无镜像违规。属于「同一符号两份实现但落点二选一」的合规情形。 |
| `__divti3`、`__modti3`、`__clzsi2` 等 | （未在 OS01 内出现） | ✅ 修 | OS01 当前不在 kernel / libc 引入其他 builtin 符号；一旦引入，必须走 §2.1 落点。 |

### 3.2 跨用户 ABI（UAPI）

| 类别 | 位置 | 现状 | 说明 |
|---|---|---|---|
| auxv 常量 | `kernel/include/uapi/auxv.h:11-25`（15 个）+ `libc/include/sys/auxv.h:9-31`（23 个） | ❌ 违例 | **常量集已不一致**：libc 多出 **8 个**（`AT_NOTELF/UID/EUID/GID/EGID/SECURE/HWCAP2/EXECFN`），kernel UAPI 缺失。`kernel/include/uapi/auxv.h` 自身注释明确写「Kernel-side mirror of `libc/include/sys/auxv.h`. Both headers list the same AT_* constants」，但实际已不是同一份。违反 §2.2（常量集不一致）。建议落地：删除 `libc/include/sys/auxv.h`（或改为 `#include <uapi/auxv.h>` 转发），把全部 AT_* 常量集中在 `kernel/include/uapi/auxv.h`，由 build 步骤 install 到 libc sysroot。 |
| syscall 号 | `kernel/include/uapi/syscall.h`（74 号）+ `libc/include/sys/syscall.h`（用户态通过 `__NR_*` 宏） | 🟡 部分 | kernel 端单一来源；libc 端通过 sysroot 安装（参见 `kernel/Makefile:106` x86_64 sysroot 路径）；不构成镜像违规。但 `__NR_*` 宏列表需定期与 `syscall.h` 同步，靠人工——建议把 syscall 号表生成由 build 步骤完成（参见 AAGU-6 cleanup batch）。 |
| 共享结构体（`struct boot_context`、`sigaction`、`timespec` 等） | 主要在 `kernel/include/uapi/`；用户态通过 sysroot 包含 | ✅ 修 | 不存在镜像违规。 |
| 其他 UAPI 头（`futex.h`、`sockaddr.h`、`time.h`） | 全部位于 `kernel/include/uapi/` | ✅ 修 | 未在 `libc/include/` 发现同名镜像。 |
| `stat.h` 中的 `AT_FDCWD` / `AT_SYMLINK_NOFOLLOW` / `DT_*` 常量 | `kernel/include/uapi/stat.h:102-111`（6 个 `DT_*`）+ `libc/include/sys/stat.h:100-115`（9 个 `DT_*`） | ❌ 违例 | **第二对 UAPI 镜像**，且**常量集已不一致**。<br>实测 diff：<br>kernel 有 `DT_UNKNOWN/REG/DIR/CHR/BLK/LNK`（6 个）<br>libc 有 `DT_UNKNOWN/FIFO/CHR/DIR/BLK/REG/LNK/SOCK/WHT`（9 个）<br>libc 多 `DT_FIFO (1)`、`DT_SOCK (12)`、`DT_WHT (14)` —— 是 POSIX 完整集合。<br>kernel 没追上 = 镜像 + 不一致双重违例 §2.2。<br>`AT_FDCWD=-100` / `AT_SYMLINK_NOFOLLOW=0x100` 两处定义一致，但镜像本身违规。<br>建议落地：合并到 `kernel/include/uapi/stat.h`，补全 9 个 `DT_*`（POSIX 完整集合），libc 转发；install owner = `mk/components/sysroot.mk` 单一 writer（见 cleanup plan Task 2 修正）。 |

### 3.3 arch value（facade + strong override）

| 类别 | 位置 | 现状 | 说明 |
|---|---|---|---|
| `AT_PLATFORM` payload | facade `kernel/include/arch/auxv.h:6-17` + `kernel/arch/aarch64/auxv.c` + `kernel/arch/x86_64/auxv.c`；消费方 `kernel/sched/task.c:1173-1210` | ✅ 修 | AAGU-2 已落地（commit `8931cab`）。`setup_user_stack()` 走 `arch_auxv_platform()` facade，无 `#ifdef __x86_64__` 分支、无硬编码字符串。 |
| `arch_cpu_pause()` | `kernel/include/arch/regs.h` + `kernel/arch/<arch>/regs.h` | ✅ 修 | weak-default + strong-override 模式合规。 |
| `arch_irq_*` | `kernel/include/arch/irq.h` + `kernel/intr/arch_irq_hooks.c`（弱默认）+ `kernel/arch/x86_64/irq_hooks.c` | ✅ 修 | AAGU-2 已合规；详见 `docs/arch.md`「中断 hook 三段式」段。 |
| `arch_kernel_thread_entry` | facade + `kernel/arch/x86_64/thread_entry.S` | ✅ 修 | arch-neutral builder (`sched/task.c`) 无 arch 字符串。 |
| `arch_register_subsys` | facade + `kernel/arch/<arch>/linker.ld` | ✅ 修 | driver 自注册走 initcall，arch-neutral。 |
| `#ifdef __x86_64__` 在 arch-neutral 通用层 | `kernel/intr/softirq.c:11, 48`（**真 arch-neutral TU**，在 aarch64 whitelist `kernel/Makefile:47` 内） | ❌ 违例 | ifdef 内容是 `__asm__ __volatile__("lock orq %0, softirq_status(%%rip)" ...)`（行 12-13）和 `"lock andq ..."`（行 49-50），**即 x86 原子位 set/clear 内联汇编**；aarch64 走纯 C 写（行 19、52）。实际违例是「x86 原子操作硬塞进通用层」—— 应该抽象成 `kernel/include/arch/atomic.h` facade 的 `arch_atomic_or_u64` / `arch_atomic_and_u64`，x86 strong override 用 `lock orq/andq`，aarch64 strong override 用 `ldset`/`stclr`（或带 LR/SC 重试）。`softirq.c` 调用 facade，**完全不出现 ifdef**。 |
| `#ifdef __x86_64__` 在 `kernel/intr/` 下 x86-only 驱动 | `kernel/intr/pic/8259A.c:104` + `kernel/intr/apic/lapic_timer.c:218` + `kernel/intr/apic/lapic.c:180` | 🟡 部分 | 这些是 x86-only 驱动被放在 arch-neutral 的 `kernel/intr/` 目录下，靠 `#ifdef __x86_64__` 跳过。**严格来说不是 §2.3 违例**（不在 arch-neutral builder 内），但**目录位置错**：x86-only 驱动应该放 `kernel/arch/x86_64/intr/` 或 `kernel/intr/pic/`（仅 x86）并由 Makefile whitelist 控制。建议落地：重定位 + 移除 ifdef。 |

### 3.4 libc API 镜像（`kernel/include/compat/`）

| 类别 | 位置 | 现状 | 说明 |
|---|---|---|---|
| `kernel/include/compat/list.h` | 与 `libc/include/list.h` 同时存在；头注释（行 1-14）自承 "FROZEN, do not modify without updating both" | ❌ 违例 | **典型人工同步镜像**。aarch64 freestanding 路径必须用它，但同步靠人工。违反 §2.4（头注释明文写出 "FROZEN, do not modify without updating both" = 人工约定 = 违例）。建议落地：合并模式（kernel 直接 include libc 的 `list.h`），前提是 aarch64 whitelist 不再 exclude libc——这又依赖 §2.2 sysroot 安装机制到位。 |
| `kernel/include/compat/rbtree.h` | 与 `libc/include/rbtree.h` 同时存在；同上 "FROZEN" | ❌ 违例 | 同上。 |
| `kernel/include/compat/string.h` | 与 `libc/include/string.h` 同时存在 | ❌ 违例 | 同上；头注释明确写 "Phase 2 #5 R2 + scope expansion: aarch64 kernel no longer pulls `<string.h>` from `libc/include/`"。 |
| `kernel/include/compat/stdlib.h` | 与 `libc/include/stdlib.h` 同时存在 | ❌ 违例 | 同上；含 `calloc/free/strtol` 等扩展集，差异化已发生（见 `compat/stdlib.h:69-79` 末尾重复声明）。 |
| `kernel/include/compat/sys/cdefs.h` | 与 `libc/include/sys/cdefs.h` 同时存在 | ❌ 违例 | 内容基本一致（仅注释差异），但镜像存在本身违反 §2.4。 |
| `kernel/include/compat/sys/types.h` | 与 `libc/include/sys/types.h` 同时存在 | ❌ 违例 | 同上。 |

**说明（context）**：上述 6 个镜像文件是 `2026-09-18` commit `23f916d`「aarch64 `-I libc/include` policy cleanup」Phase 2 #5 的产物（参见 `docs/superpowers/specs/2026-09-18-libc-include-policy-design.md`）。当时为了让 aarch64 kernel 摆脱 `-I libc/include`，选择**手抄镜像**作为过渡方案。该方案解决了「aarch64 不该 include libc 头」的**架构诉求**，但引入了「双份维护」的**镜像违例**。本规范的立场：手抄镜像**只是过渡态**；终极目标是§2.4 的「合并模式 / install 模式」。后续 issue（参见 §4）应把这 6 个文件收口。

### 3.5 libc 用户态 FILE 注册

| 类别 | 位置 | 现状 | 说明 |
|---|---|---|---|
| `open_files[]` 注册表 | `libc/stdio/stdio_file.c:26-60`（`register_file` / `unregister_file` / `is_open_file`）+ 消费方：`fopen/fdopen` 注册、`fclose` 注销、`fflush` 验证（行 132-152） | ❌ 假象安全 | 仅 `fopen`/`fdopen` 注册、`fclose` 注销、`fflush` 验证。**`fread`（行 108-115）、`fwrite`（行 117-130）不查表**：传入垃圾指针既不被 `is_open_file` 拦截，也不产生任何错误。`fflush` 注释自我承认这是「POSIX says return EOF」的局部处理——但 `fread/fwrite` 路径**完全未受注册表保护**。一个 typo'd stream 名会让 `fread(buf, 1, 100, garbage_ptr)` 拿到 `garbage_ptr->fd` 然后调 `read(garbage_fd, ...)`，静默落入任意 fd。这不是「假象安全」是「完全无安全」。**建议落地**：把 `fread/fwrite` 路径都加上 `is_open_file` 检查（与 `fflush` 一致），或收窄注释（承认 libc 不做 stream 验证，只保证 fd 有效）。 |
| atexit 处理链从未被调用 | `libc/stdlib/atexit.c:20` 定义 `__call_atexit_handlers`；`libc/csu/csu.c:35, 51` `__libc_start_main` 直接 `return main(...)`，无 `exit()` / `fflush(NULL)` / fini 触发；全 repo grep `__call_atexit_handlers` 只命中定义行 | 🟡 部分 | 出口路径**完全死链**：`atexit()` 注册的清理函数永远不会被调用；进程退出靠 `_exit` syscall 直通内核。这是 stdio FILE 注册外**第二处假象**：注册机制存在但出口路径不存在。**reviewer round 4 拆出**：本行不属 §3.5 stdio FILE 注册范畴，由 **AAGU-4.6 follow-up** 接手。AAGU-4.6 验收必须用 OS01 QEMU user-program E2E：注册 handler → `exit(0)` → handler 写 marker 到 file/pipe → 父进程断言 marker 存在；不得用 hosttest（atexit 死链在 csu 层，hosttest 触达不到）也不得用 destructor（OS01 libc 无 `.init_array`）。 |

---

## 4. 后续 issue 切分建议

> 本规范**不强制**按本节切，但提供一个「先规范后落地」的可执行路径。每条建议对应 3.现状对照表的违例项。

### 4.1 P1 / 高优先 — 假象安全整改

**Issue: AAGU-4.1 — libc stdio FILE 注册扩面**（依 3.5）
- 在 `fread/fwrite` 入口加 `is_open_file` 检查
- OR：明确「libc 不做 stream 验证」的语义边界，删 `is_open_file` 表
- 推荐前者；后者需要把 fopen/fclose 全部放弃 FILE 概念，工作量大

### 4.2 P1 — UAPI auxv 单一源收口

**Issue: AAGU-4.2 — UAPI auxv 收口（kernel UAPI 为唯一源）**（依 3.2）
- 把 libc 端 `AT_*` 常量全部集中到 `kernel/include/uapi/auxv.h`
- 删除 `libc/include/sys/auxv.h`（或改为转发）
- 复用 `kernel/Makefile` 的现有 `install-headers` target（kernel 发布 `<uapi/...>`）；libc 唯一发布 `<sys/...>` wrapper

### 4.3 P2 — 镜像层收口

**Issue: AAGU-4.3 — `kernel/include/compat/*` 6 文件收口**（依 3.4）
- 路径选择（实施时定）：
  - (a) **合并模式**：kernel 直接 `#include <list.h>`，由 aarch64 whitelist 调整为不 exclude libc
  - (b) **install 模式**：kernel 拥有单一 `kernel/include/freestanding/<name>.h`，由 build 步骤 install 到 libc sysroot
- 删除 `kernel/include/compat/` 整个目录
- 更新 aarch64 Makefile 不再 `-I include/compat`

### 4.4 P3 — builtin 统一

**Issue: AAGU-4.4 — kernel 端 `__stack_chk_guard` 移到 `kernel/compiler_rt/`**（依 3.1）
- 当前 `kernel/core/main.c:63/77` 独立定义 kernel 端 canary
- 移到 `kernel/compiler_rt/` 与 `__stack_chk_fail` 配套
- libc 端 `libc/ssp/ssp.c` 继续按 `__is_libk` 排除
- 与 §2.1 「二选一」规则对齐：kernel path 单一 TU

### 4.5 P2 — arch atomic bit op facade

**Issue: AAGU-4.5 — `arch_atomic_*_u64` facade + softirq.c 移除 ifdef**（依 3.3）
- 当前 `kernel/intr/softirq.c:11, 48` 有 `#if defined(__x86_64__)` 守护 `lock orq/andq` 内联汇编；aarch64 走纯 C 写（行 19、52）
- 实际违例：x86 原子操作硬塞进通用层
- 修法：
  - 新增 `kernel/include/arch/atomic.h` facade：`void arch_atomic_or_u64(uint64_t *addr, uint64_t mask);` / `void arch_atomic_and_u64(uint64_t *addr, uint64_t mask);`
  - x86_64 strong override `kernel/arch/x86_64/atomic.c`：`lock orq/andq` 内联汇编
  - aarch64 strong override `kernel/arch/aarch64/atomic.c`：`ldset`/`stclr`（AArch64 Large System Extensions）或 LR/SC 重试
  - `kernel/intr/softirq.c` 改调 facade，**不出现 ifdef**
- 顺带：`kernel/intr/{pic/, apic/}` 下 x86-only 驱动（8259A、lapic、lapic_timer、ioapic）从 `kernel/intr/` 挪到 `kernel/arch/x86_64/intr/` —— **目录错位**问题，标 🟡 部分
- 同步排查 `kernel/` 下其它 arch-neutral TU 的 `#if defined(__x86_64__)` / `#if defined(__aarch64__)` 守护位，按同样模式收口

---

## 5. 与其他文档的关系

| 文档 | 关系 |
|---|---|
| `docs/arch.md` | **总论**（facade + override 模式）；本 spec §2.3 引用其模式 |
| `AGENTS.md` §Directory organization 第 3 条 | 本 spec §2.3 的 AGENTS 入口（已写入，本 PR 同步） |
| `docs/aarch64-libc-include-policy-closure-2026-09-18.md` | 镜像层的历史背景（commit `23f916d` 的动机） |
| `docs/superpowers/specs/2026-09-18-libc-include-policy-design.md` | 同上设计稿 |
| `docs/superpowers/specs/2026-09-11-x86_64-kernel-ssp-design.md` | kernel 端 canary 设计稿（本 spec §3.1 引用） |
| AAGU-5（entropy quality） | 未来 follow-up，由本 spec §3.3 「arch value」精神扩展 |

---

## 6. 验收对照（issue AAGU-4 验收条目）

- [x] 规范文档草稿已写完，4 类边界规则清晰无歧义（§2）
- [x] 现状对照清单覆盖 AAGU-1 两轮评审找到的所有违例 + Explore agent 抓出的额外违例（stat.h 镜像、softirq.c ifdef、atexit 死链）+ reviewer round 3 抓出的精确数（kernel 6 vs libc 9 DT_*，auxv delta 实为 8）；合计：**10 ❌ 违例 + 4 🟡 部分 + 9 ✅ 修 = 23 行状态表**（reviewer round 3 抓出 §6 之前写「8 ❌ + 3 🟡 + 4 ✅」是错的，已 recount 修正）
- [x] 规范文档 review 过 `AGENTS.md`「Directory organization」段 + `docs/arch.md` 现有概览，不重复造轮子（§1）
- [x] 文档放在 `docs/arch/` 下，纳入 AGENTS.md「Documentation」索引（本 PR 同步）

**recount 来源**（spec §3 表逐行核算）：
- §3.1（compiler runtime）：`__stack_chk_guard` 🟡 1；`__udivti3` ✅ + `__divti3`等 ✅ 2
- §3.2（UAPI）：auxv 常量 ❌ + stat.h DT_* ❌ 2；syscall 号 🟡 1；共享结构体 ✅ + 其他 UAPI 头 ✅ 2
- §3.3（arch value）：AT_PLATFORM/`arch_cpu_pause`/`arch_irq_*`/`arch_kernel_thread_entry`/`arch_register_subsys` ✅ 5；softirq.c ifdef ❌ 1；x86-only 驱动 ifdef 🟡 1
- §3.4（compat 镜像）：`compat/{list,rbtree,string,stdlib,sys/cdefs,sys/types}.h` ❌ 6
- §3.5（stdio）：open_files[] ❌ + atexit 死链 🟡 2

**recount 合计**：10 ❌ + 4 🟡 + 9 ✅ = 23

**说明（Explore agent findings, 2026-09-22 补）**：
- auxv 常量 delta 实测为 8 个（`AT_NOTELF/UID/EUID/GID/EGID/SECURE/HWCAP2/EXECFN`），AAGU-4 issue body 写 7 个；本 spec §3.2 已修正为 8 个。
- `kernel/include/uapi/stat.h` vs `libc/include/sys/stat.h` 是**第二对 UAPI 镜像**（`AT_FDCWD` + `AT_SYMLINK_NOFOLLOW` + 9 个 `DT_*`），AAGU-4 issue body 未提及；§3.2 已加行。**修正**：kernel 有 6 个 `DT_*`，libc 有 9 个 `DT_*`（差 3 个：`DT_FIFO (1)`、`DT_SOCK (12)`、`DT_WHT (14)`），不是「差 8 个」也不是「8 个常量集」。
- `kernel/intr/softirq.c`（**真 arch-neutral TU**，在 aarch64 whitelist）有 `#ifdef __x86_64__`，违反 §2.3；§3.3 已加行。`kernel/intr/pic/8259A.c` 等 x86-only 驱动也有 ifdef，但严格说不是 §2.3 违例（目录错位问题），标 🟡。
- `__call_atexit_handlers` 定义但全 repo 0 caller；与 §3.5 同一 issue 链一并修。

**reviewer round 3 修正记录**：
- plan Task 2 Step 3 sysroot.mk 装配方案：kernel 只 stage `<uapi/>`，libc 提供 `<sys/>` forwarding，避免 duplicate destination
- plan Task 2 各处数字「22」「7 个」「DT_* 8」全部修正为「23」「8 个」「DT_* 9」
- plan Task 3 string.h/stdlib.h 分类修正（pure function kernel-owned vs libc-stateful allocator）；`libc_stub.h` 不存在改用 `kernel/arch/aarch64/libc_stub.c` 直接引用；include typo 修正；Step 4/5 重复删除
- plan Task 5 AArch64 LR/SC 实现修正：status 寄存器独立（`uint32_t status` 单独约束），不再误用 new_val 寄存器；附 memory-order 语义说明（`ldaxr` acquire + `stxr` release = seq_cst RMW）
- plan Task 1 hosttest 改用 OS01 hosttests/ 框架（host clang + LIBC_OBJS），删 `--target=x86_64-elf --sysroot=$SYSROOT -nostdlib` 模式；0xDEADBEEF 用 SIGSEGV catcher 处理；atexit test 改用显式 oracle + `ASSERT_EQ(teardown_flag, 1)`，**不**依赖 destructor（OS01 libc 无 `.init_array`）

**reviewer round 4 修正记录**（已**全部否定**，仅留为历史快照，下文为**当前实际方案**）：
- ~~SIGSEGV catcher / 0xDEADBEEF 野指针 + ``__attribute__((destructor))` atexit oracle~~ → 改 pipe-based fake `mini_file_t`（reviewer round 5 进一步简化到 2 个 TEST_FUNC）
- ~~kernel/Makefile 新加 install-headers recipe + DESTDIR~~ → 不动 `kernel/Makefile:412-425`，kernel 唯一 stage `<uapi/>`，libc 唯一 stage `<sys/>` forwarding
- ~~kernel/include/freestanding/allocator.h + libc include/string.h|stdlib.h 转发~~ → Task 3 仅边界规则 4 列表，parent plan **不**预写 header 落地路径
- ~~`stxr` + seq_cst memory order~~ → 改 `stlxr` + acq_rel（`ldaxr + stlxr + memory clobber` = acq_rel RMW）
- ~~plan 4-task / spec 4-task 不一致~~ → 全部统一 5 task + AAGU-4.6 follow-up

**当前实际方案（reviewer round 5 后）**：
- §3.5 atexit 行归属 **AAGU-4.6**（不在 AAGU-4.1 范围）
- §3.5 stdio FILE 注册表只登记 `open_files[]` ❌ 一行（fix 由 AAGU-4.1 落地）
- §3.3 ifdef 行描述已是当前态（`lock orq`/`lock andq` 原子位 set/clear + Task 5 的 `arch_atomic_or/and_u64` facade）
- §3.2 stat.h 行描述已是 6 vs 9 实测 diff
- §3.1 / §3.4 现状描述与 cleanup plan 一致
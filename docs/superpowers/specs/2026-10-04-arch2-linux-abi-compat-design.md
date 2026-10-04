---
title: OS01 ARCH-2 Linux ABI 兼容层独立设计
created: 2026-10-04
updated: 2026-10-04
type: spec
status: draft-for-review
tags: [osdev, syscall, linux-abi, compat, architecture]
related: [docs/roadmap.md ARCH-2, docs/syscall/syscall.md, docs/arch/cross-boundary-symbols.md]
---

# OS01 ARCH-2 Linux ABI 兼容层独立设计

## 1. 目标与范围

本设计落实 `docs/roadmap.md` 中的 **ARCH-2（Linux ABI 兼容层独立）** 架构治理任务。

### 核心目标
1. **抽离架构热路径**：将现存于 `kernel/arch/x86_64/intr/trap.c::do_system_call` 中的 70 余行 Linux 翻译表与分发前逻辑彻底移出，建立独立的兼容模块 `kernel/syscall/compat_linux.c` 与公开接口头 `kernel/include/syscall/compat_linux.h`。
2. **彻底消除整数溢出隐患**：现有的 `static const int8_t linux_to_os01[320]` 使用有符号 8 位整数存储 OS01 编号，其上限仅为 127。一旦未来 OS01 系统调用编号扩充超过 127（当前已达 74），将引发**静默符号翻转/溢出**。本次重构将表项类型升级为 `int16_t`，并引入编译期断言 `_Static_assert` 确保强类型与范围安全。
3. **纠正未映射与不支持系统调用的返回语义**：
   - 现存实现中，若 Linux 编号未映射（值为 0）或显式不支持（值为 -1），会跳过翻译直接透传给原生分发器；这会导致偶然与 OS01 原生编号重合的 Linux 调用被**意外执行**，其余则被 OS01 原生分发器返回 `-EINVAL`。
   - 依 POSIX 及 Linux ABI 规范，在 Linux ABI 模式下，未映射、越界或不支持的系统调用必须统一返回 **`-ENOSYS`**（38），决不能泄漏原生内部错误码或误调内核原生调用。
4. **确立 OS01 ABI 唯一标准**：严格遵守 2026-10-04 架构决策——**OS01 自有 syscall ABI（`kernel/include/uapi/syscall.h`）为全内核唯一核心标准**；Linux ABI 仅作为外围兼容层（服务于带有 `PF_LINUX_ABI` 标记的二进制与程序），不得反向主导内核编号或核心系统调用语义。
5. **提供语义与参数适配机制**：为存在参数个数、标志位或结构体差异的调用（如 `rt_sigaction`、`wait4` 等）提供适配通道，确保核心系统调用处理函数（`sys_fs.c`、`sys_proc.c` 等）只面向 OS01 原生语义。

### 核心约束与长期边界
1. **Linux ABI 激活严禁解析 `PT_INTERP`**：
   - **长期架构考量**：OS01 长期规划必须支持自有的动态链接；在标准 ELF 规范中，`PT_INTERP` 是通用可执行文件声明动态链接器的标准段。未来原生 OS01 动态二进制同样会包含 `PT_INTERP`（如指向 `/lib/ld-os01.so`），绝不能将 `PT_INTERP` 与 Linux ABI 混淆或等同。
   - **分层解耦**：动态链接属于通用 ELF 加载器和用户态动态链接器的能力演进，属于后续独立里程碑。本次 ARCH-2 严禁在 Linux ABI 激活判定中去解析或绑定 `PT_INTERP`。
2. **源码级移植（如 BusyBox）与二进制兼容的严格边界**：
   - 当前在 OS01 sysroot 下构建的所有二进制（包括通过源码移植构建的 `busybox.elf`，以及 `init.elf`、`terminal.elf`、`systest.elf`）均通过 OS01 自研 `libc.a` 发出 OS01 原生系统调用，它们是纯粹的原生程序，运行时必须维持 `flags == 0`，严禁打上 `PF_LINUX_ABI`；
   - Linux ABI（`PF_LINUX_ABI`）的激活机制严格限定为以下两条边界：
     - **外部静态 Linux 二进制识别**：通过 `PT_NOTE` 段（检查 `.note.ABI-tag` 中是否明确声明了 Linux/GNU ABI）进行安全识别，不会误伤由 `user/linker.ld` 抛弃了所有 Note 段的 OS01 原生二进制；
     - **显式测试/受控接口（Personality）**：为系统调用测试套件（`systest`）提供受控的标志设置途径，使回归套件能够显式验证 Linux ABI 分发与 `-ENOSYS` 返回语义。

### 非本项范围
- 本项不改变 OS01 原生系统调用编号（0..74）或调用约定；
- 本项不引入庞大的 Linux 系统调用完整模拟（如 300+ 个系统调用全量实现），仅重构并规范现有已支持的兼容调用映射；
- 本项不实现动态链接（`PT_INTERP` 解析与共享库加载）；
- aarch64 的 Linux 兼容（基于 generic unistd）留待 aarch64 用户态落地后扩展，本设计预留清晰的架构解耦接口。

---

## 2. 现状审计与痛点分析

### 2.1 现状实现（`kernel/arch/x86_64/intr/trap.c:1046-1119`）
在 ARCH-1 完成后，OS01 原生系统调用的分发已抽离至 `kernel/syscall/`，但 Linux ABI 翻译仍留在 `trap.c::do_system_call` 入口处：

```c
    // Linux x86_64 ABI translation (for busybox etc.)
    if ((current->flags & PF_LINUX_ABI) && regs->rax < 320) {
        static const int8_t linux_to_os01[320] = {
            [0] = 6,   // read -> SYS_read
            [1] = 1,   // write -> SYS_write (same)
            ...
            [318] = 66, // getrandom -> SYS_getrandom
        };
        int8_t os = linux_to_os01[regs->rax];
        if (os > 0)
            regs->rax = os;
    }
```

### 2.2 关键痛点与缺陷

| 缺陷点 | 当前现状 | 潜在危害与架构冲突 |
|---|---|---|
| **类型溢出风险** | 表项类型为 `int8_t` | `int8_t` 最大表示 127。随着内核功能扩充，OS01 编号一旦超过 127，`[nr] = SYS_xxx` 会产生有符号溢出截断，静默调用错位系统调用。 |
| **魔数与越界检查脆弱** | 硬编码 `regs->rax < 320` | Linux x86_64 系统调用号已超过 450。若用户程序发起 ≥ 320 的调用，判断为假直接透传给原生分发器。 |
| **未映射号错误透传** | `if (os > 0) regs->rax = os;` | 若 Linux 进程调用未映射号（`os == 0`）或不支持号（`os == -1`），`regs->rax` 保持原值透传至 `syscall_dispatch()`。若该原值恰好落在 OS01 原生编号范围内，会**误执行毫不相关的原生系统调用**！若落在范围外，原生分发器返回 `-EINVAL` 而非 Linux 规范的 `-ENOSYS`。 |
| **架构与兼容层纠缠** | 业务映射表位于 `arch/x86_64/intr/trap.c` | 违背架构分层规范（`kernel/arch/` 只负责寄存器保存恢复与架构陷阱，不包含兼容层业务）。 |
| **参数与语义差异无处安放** | 仅做裸编号替换 | Linux 的 `rt_sigaction`（4 参数，含 sigsetsize）、`wait4`（4 参数，含 rusage）、`clone`（不同于普通 fork 的参数顺序）等，裸编号替换无法进行语义校验或结构适配。 |
| **未启用状态下的黑盒隐患** | `PF_LINUX_ABI` 未受持续回归保护 | 缺乏显式的测试注入手段与自动化单测，容易在重构中出现静默退化。 |

---

## 3. 架构设计与接口契约

### 3.1 模块定位与目录分布

按照源目录与头文件目录对称规范：
- **头文件**：`kernel/include/syscall/compat_linux.h`
- **通用兼容分发与适配实现**：`kernel/syscall/compat_linux.c`
- **测试套件**：
  - 宿主单元测试：`hosttests/cases/test_compat_linux.c`
  - 集成回归测试：`user/systest.c` 补充 `PF_LINUX_ABI` 专用测试用例

```
kernel/
├── arch/x86_64/intr/
│   └── trap.c              # 纯架构入口：解码 pt_regs，若 PF_LINUX_ABI 则调用 compat_linux_dispatch()
├── include/syscall/
│   ├── dispatch.h          # OS01 原生分发接口
│   └── compat_linux.h      # Linux ABI 兼容层公共接口与常量
└── syscall/
    ├── dispatch.c          # OS01 原生分发表（0..74）
    ├── sys_*.c             # 原生模块处理函数
    └── compat_linux.c      # Linux x86_64 翻译表、断言、语义适配器与独立分发
```

### 3.2 兼容层核心接口定义 (`kernel/include/syscall/compat_linux.h`)

```c
#ifndef _SYSCALL_COMPAT_LINUX_H
#define _SYSCALL_COMPAT_LINUX_H

#include <stdint.h>
#include <stdbool.h>
#include <syscall/dispatch.h>

/* 兼容表表项类型：必须使用 int16_t 杜绝 127 溢出 */
typedef int16_t compat_syscall_nr_t;

/* 映射状态哨兵 */
#define COMPAT_UNMAPPED      ((compat_syscall_nr_t) 0)
#define COMPAT_UNSUPPORTED   ((compat_syscall_nr_t)-1)

/* Linux x86_64 系统调用编号支持上限（当前最高使用 318 getrandom，预留安全裕量） */
#define LINUX_X86_64_NR_MAX  384

/**
 * 校验指定 Linux 系统调用编号是否有对应支持（直接原生映射或兼容适配器）
 */
bool compat_linux_has_syscall(uint64_t linux_nr);

/**
 * Linux ABI 系统调用分发入口
 *
 * 契约：
 * 1. 若 linux_nr 处于支持表中且有原生对应，转换为对应 SYS_xxx 编号并处理参数适配，
 *    转调 syscall_dispatch(ctx)；
 * 2. 若 linux_nr 具有兼容适配函数（如 rt_sigaction 参数检查），调用适配函数；
 * 3. 若 linux_nr 未映射、越界或显式标记为 COMPAT_UNSUPPORTED，严禁调用原生分发器，
 *    直接返回 -ENOSYS（-38）；
 * 4. 绝不改变非 PF_LINUX_ABI 任务的分发路径。
 */
int64_t compat_linux_dispatch(syscall_ctx_t *ctx);

/**
 * 仅查询 Linux 编号对应的 OS01 原生编号（供测试与审计使用）
 * 返回原生 SYS_xxx，或 COMPAT_UNMAPPED / COMPAT_UNSUPPORTED
 */
compat_syscall_nr_t compat_linux_lookup_nr(uint64_t linux_nr);

#endif /* _SYSCALL_COMPAT_LINUX_H */
```

### 3.3 强类型映射表与编译期断言安全网

在 `kernel/syscall/compat_linux.c` 中：
1. **统一使用符号常量**：全部映射目标使用 `<uapi/syscall.h>` 定义的 `SYS_xxx` 宏。
2. **显式类型与编译期断言**：
   ```c
   _Static_assert(sizeof(compat_syscall_nr_t) == 2, "compat_syscall_nr_t must be 16-bit");
   _Static_assert(SYS_fstatat <= INT16_MAX, "OS01 syscall numbers must fit within int16_t");
   _Static_assert(LINUX_X86_64_NR_MAX <= 1024, "Linux nr table size sanity check");
   ```
3. **映射表定义（Designated Initializers）**：
   ```c
   static const compat_syscall_nr_t linux_x86_64_table[LINUX_X86_64_NR_MAX] = {
       [0]   = SYS_read,
       [1]   = SYS_write,
       [2]   = SYS_open,
       [3]   = SYS_close,
       [4]   = SYS_stat,
       [5]   = SYS_fstat,
       [6]   = SYS_lstat,
       [8]   = SYS_lseek,
       [9]   = SYS_mmap,
       [10]  = SYS_mprotect,
       [11]  = SYS_munmap,
       [12]  = SYS_brk,
       [13]  = SYS_signal,        // rt_sigaction
       [14]  = SYS_sigprocmask,   // rt_sigprocmask
       [15]  = SYS_sigreturn,     // rt_sigreturn
       [16]  = SYS_ioctl,
       [21]  = SYS_access,
       [25]  = COMPAT_UNSUPPORTED,// mremap -> 显式不支持
       [32]  = SYS_dup,
       [33]  = SYS_dup2,
       [35]  = SYS_nanosleep,
       [39]  = SYS_getpid,
       [41]  = SYS_socket,
       [42]  = SYS_connect,
       [43]  = SYS_accept,
       [44]  = SYS_sendto,
       [45]  = SYS_recvfrom,
       [48]  = SYS_shutdown,
       [49]  = SYS_bind,
       [50]  = SYS_listen,
       [51]  = SYS_getsockname,
       [54]  = SYS_setsockopt,
       [55]  = SYS_getsockopt,
       [56]  = SYS_fork,          // clone
       [57]  = SYS_fork,          // fork
       [59]  = SYS_exec,          // execve
       [60]  = SYS_exit,          // _exit
       [61]  = SYS_waitpid,       // wait4
       [62]  = SYS_kill,
       [63]  = SYS_uname,
       [79]  = SYS_getcwd,
       [80]  = SYS_chdir,
       [83]  = SYS_mkdir,
       [84]  = SYS_rmdir,
       [87]  = SYS_unlink,
       [88]  = SYS_symlink,
       [89]  = SYS_readlink,
       [102] = SYS_getppid,       // 历史兼容别名
       [110] = SYS_getppid,
       [162] = SYS_nanosleep,
       [164] = SYS_getifaddr,
       [201] = SYS_times,
       [217] = SYS_getdents64,
       [228] = SYS_clock_gettime,
       [231] = SYS_exit,          // exit_group
       [262] = SYS_fstatat,
       [318] = SYS_getrandom,
   };
   ```

### 3.4 语义差异与特殊适配器设计

在 `compat_linux.c` 中，针对具有 Linux 专属语义要求的调用设置轻量适配函数：

1. **`rt_sigaction` (Linux nr 13)**：
   - 原型差异：Linux `sys_rt_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact, size_t sigsetsize)`。
   - 规则：Linux 内核要求第 4 参数 `sigsetsize == sizeof(sigset_t)`（x86_64 上为 8 字节），否则返回 `-EINVAL`。
   - 适配：`compat_sys_rt_sigaction` 先检查 `ctx->args[3] == 8`，满足后将 `ctx->nr = SYS_signal` 转发至 `syscall_dispatch()`。
2. **`wait4` (Linux nr 61)**：
   - 原型差异：Linux `wait4(pid, status, options, rusage)`；OS01 `SYS_waitpid` 仅有 3 个参数。
   - 适配：直接忽略 `rusage`（或若非空但无需填充返回 0），把 `args[0..2]` 透传给 `SYS_waitpid`。
3. **未映射与不支持号的拦截**：
   ```c
   int64_t compat_linux_dispatch(syscall_ctx_t *ctx)
   {
       uint64_t nr = ctx->nr;
       if (nr >= LINUX_X86_64_NR_MAX) {
           debug_syscall("[compat_linux] nr=%lu out of range -> -ENOSYS\n", nr);
           return -ENOSYS;
       }
       compat_syscall_nr_t os_nr = linux_x86_64_table[nr];
       if (os_nr == COMPAT_UNMAPPED || os_nr == COMPAT_UNSUPPORTED) {
           debug_syscall("[compat_linux] nr=%lu (%s) -> -ENOSYS\n",
                         nr, os_nr == COMPAT_UNSUPPORTED ? "unsupported" : "unmapped");
           return -ENOSYS;
       }
       // 特殊调用语义适配
       if (nr == 13) {
           return compat_sys_rt_sigaction(ctx);
       }
       // 标准 1:1 映射
       ctx->nr = (uint64_t)os_nr;
       return syscall_dispatch(ctx);
   }
   ```

### 3.5 架构入口与调用链解耦 (`kernel/arch/x86_64/intr/trap.c`)

在 `trap.c::do_system_call` 中，代码大幅精简并解除兼容表耦合：

```c
void do_system_call(pt_regs_t *regs, uint64_t error_code __attribute__((unused)))
{
#ifndef NDEBUG
    // Stack overflow guard 保留
    task_t *cur = get_current_task();
    if (cur) {
        uint64_t stack_bottom = ((uint64_t)cur) & ~(STACK_SIZE - 1);
        if ((uint64_t)__builtin_frame_address(0) - stack_bottom < 2048)
            log_err("WARNING: RSP within 2KB of stack bottom! pid=%d\n", (int)cur->pid);
    }
#endif

    syscall_ctx_t syscall_ctx = {
        .nr = regs->rax,
        .args = { regs->rdi, regs->rsi, regs->rdx,
                  regs->r10, regs->r8, regs->r9 },
        .arch_frame = regs,
        .suppress_writeback = false,
    };

    int64_t result;
    if (current->flags & PF_LINUX_ABI) {
        result = compat_linux_dispatch(&syscall_ctx);
    } else {
        const char *sname = syscall_name(syscall_ctx.nr);
        debug_syscall("[strace] pid=%d syscall(%s, arg1=%#lx, arg2=%#lx, arg3=%#lx)\n",
                      (int)current->pid, sname ? sname : "?",
                      (unsigned long)regs->rdi,
                      (unsigned long)regs->rsi,
                      (unsigned long)regs->rdx);
        result = syscall_dispatch(&syscall_ctx);
    }

    if (!syscall_ctx.suppress_writeback)
        regs->rax = (uint64_t)result;

    // ── Signal delivery ──────────────────────────────────────
    if (regs->cs & 3)
        arch_do_signal_delivery(regs);
}
```

---

## 4. 实施文件职责与增量变更清单

| 文件 | 变更类型 | 核心职责 |
|---|---|---|
| `kernel/include/syscall/compat_linux.h` | **新增** | 定义 `compat_syscall_nr_t`、`LINUX_X86_64_NR_MAX`、`compat_linux_dispatch`、`compat_linux_lookup_nr` |
| `kernel/syscall/compat_linux.c` | **新增** | 16 位映射表、`_Static_assert` 断言、特殊语义适配器、`-ENOSYS` 拦截与原生分发转调 |
| `kernel/arch/x86_64/intr/trap.c` | **修改** | 移除旧 `linux_to_os01` 数组与硬编码逻辑；若 `PF_LINUX_ABI` 则调 `compat_linux_dispatch()` |
| `kernel/Makefile` | **验证** | `$(wildcard syscall/*.c)` 自动收录新增的 `compat_linux.c` |
| `hosttests/cases/test_compat_linux.c` | **新增** | 宿主侧单元测试：测试表项完整性、符号范围、未映射/不支持返回 `-ENOSYS`、`rt_sigaction` 校验 |
| `hosttests/Makefile` | **修改** | 加入 `test_compat_linux` 测试构建与运行 |
| `user/systest.c` | **修改** | 增加 `PF_LINUX_ABI` 测试用例（验证 Linux syscall 实际调用及 unmapped 返回 `-ENOSYS`） |
| `docs/roadmap.md` | **修改** | 实施完成后将 ARCH-2 标记为已完成 |

---

## 5. 迁移与验证方案（五阶段）

### 阶段 1：构建独立兼容层与宿主测试 (Host Test First)
1. 创建 `kernel/include/syscall/compat_linux.h` 与 `kernel/syscall/compat_linux.c`。
2. 移植并标准化 Linux x86_64 映射表，设置 `_Static_assert`。
3. 创建 `hosttests/cases/test_compat_linux.c`，编写测试：
   - 遍历 `linux_x86_64_table`，确保所有已映射编号对应的 OS01 编号在原生分发表中均有处理函数；
   - 验证越界编号（如 385、999）返回 `-ENOSYS`；
   - 验证未映射编号（如 7、17、99）返回 `-ENOSYS`；
   - 验证不支持编号（如 25 `mremap`）返回 `-ENOSYS`；
   - 验证 `rt_sigaction` 4 参数校验；
   - 运行 `make test-host` 确保 100% 通过。

### 阶段 2：重构 `trap.c::do_system_call`
1. 包含 `<syscall/compat_linux.h>`。
2. 彻底删除 `trap.c` 内部的 `static const int8_t linux_to_os01[320]` 和临时翻译分支。
3. 接入 `compat_linux_dispatch(&syscall_ctx)`。
4. 运行 `make clean && make test-static`，确保编译无告警、无重复定义，符号布局合规。

### 阶段 3：内核态与系统级测试验证 (QEMU & Systest)
1. 在 `user/systest.c` 中加入 Linux ABI 专项用例：
   - 通过系统调用或测试入口为当前子进程设置 `PF_LINUX_ABI`；
   - 发起 Linux 系统调用：
     - Linux `__NR_getpid` (39) 验证返回值与 native `getpid` 一致；
     - Linux `__NR_getrandom` (318) 验证随机数生成；
     - Linux 未映射编号（如 999）验证严格返回 `-ENOSYS`（`errno == 38`）；
     - Linux 显式不支持编号（如 25 `mremap`）验证严格返回 `-ENOSYS`；
2. 运行 `make OS01_SYSTEST=1 test-qemu SUITE=systest`，验证 340+ 测试全绿通过。

### 阶段 4：交互式 Shell 与 BusyBox 回归
1. 启动完整系统验证：`make run` 或 headless 测试脚本。
2. 验证 BusyBox ash 命令解析、管道、脚本执行正常无退化。
3. 验证网络通信与其它工具正常工作。

### 阶段 5：清理与文档闭环
1. 检查各单文件行数，确保仍严格遵守 `< 800 行` 规则。
2. 更新 `docs/roadmap.md`、`docs/syscall/syscall.md` 与 `docs/changelog.md`。

---

## 6. 风险评估与防御策略

| 风险项 | 严重程度 | 防御与应对措施 |
|---|---|---|
| **BusyBox 依赖的未登记系统调用** | 中 | 现存 BusyBox 主要走 OS01 libc 编译，极少数直接发起 Linux 系统调用。严格保留现有表中的所有条目；若发现新调用，在 `compat_linux.c` 中集中增补并记录日志。 |
| **未映射返回 `-ENOSYS` 对旧行为的影响** | 低 | 过去未映射号若透传可能偶然执行编号相同的原生调用或返回 `-EINVAL`。返回 `-ENOSYS` 是唯一符合 POSIX 的安全行为，能防止未知内存破坏。 |
| **架构解耦时的头文件循环引用** | 低 | `compat_linux.h` 仅依赖 `<stdint.h>` 与 `<syscall/dispatch.h>`，不依赖 `sched/task.h` 或任何 arch 内部头。 |

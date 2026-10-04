---
title: OS01 ARCH-1 syscall 层脱离 arch 设计
created: 2026-10-04
updated: 2026-10-04
type: spec
status: draft-for-review
tags: [osdev, syscall, architecture]
related: [docs/roadmap.md ARCH-1, docs/syscall/syscall.md]
---

# OS01 ARCH-1 syscall 层脱离 arch 设计

## 1. 目标与范围

把 OS01 自有 syscall 的编号分发、用户参数检查和业务处理从 `kernel/arch/x86_64/intr/trap.c::do_system_call` 移到 `kernel/syscall/`，并在 `kernel/include/syscall/` 定义对应内部接口。x86_64 的 `int $0x80` 入口仍保存、恢复寄存器；它把编号与六个参数交给通用分发器，再把结果写入返回现场。未来 aarch64 EL0 `svc` 入口可复用该分发器，但本项不交付 aarch64 用户态。

成功标准：`trap.c` 不再包含 OS01 syscall 的业务 `switch`；`kernel/syscall/*.c` 不直接读取 x86_64/aarch64 寄存器字段或包含 per-arch 头；OS01 号 0..74 的已实现调用保持原有行为；x86_64 systest、正常 BusyBox 启动及静态/宿主测试通过。

本项仅是 roadmap ARCH-1。OS01 syscall 编号、libc 调用约定、Linux ABI 兼容语义、用户态共享结构体、调度器内部架构依赖以及 aarch64 EL0 入口均不在范围内。`SYS_getpeername` 目前只有编号而无处理实现；本项继续把它视作未实现项。

## 2. 已核实的现状与方案选择

- x86_64 `entry.S::system_call` 经共同异常入口调用 `do_system_call(pt_regs_t *, 0)`。编号取 `rax`，参数依次为 `rdi/rsi/rdx/r10/r8/r9`，结果写回 `rax`。
- 当前 `do_system_call` 同时含 Linux x86_64 编号翻译表、调试名称表、约两千行 OS01 业务 `switch`，末尾在 CPL=3 时调用 `arch_do_signal_delivery`。
- `fork`、`exec`、`sigreturn` 依赖完整 `pt_regs_t` 现场；`SYS_reboot` 直接使用 x86 端口 I/O 和 `hlt`。symlink/readlink/lstat/fstatat 的现有实现位于 `sched/task.c`，其 `regs` 参数未被使用。
- aarch64 当前只具备 EL1 异常/IRQ 路径；其内核源码构建列表有意受限，尚未包含完整 syscall 依赖。

考虑了两种拆法：

1. **选用：通用分发表 + 显式现场钩子。** 大多数处理函数只见六个参数；少数需要改变用户返回现场的调用通过架构钩子处理。迁移期间可以逐组替换 case，并逐组验证。
2. **不选：将整个 `switch` 原样搬到 `kernel/syscall/`。** 虽然文件名变了，处理函数仍直接访问 `regs->rax/rdi/...`，aarch64 无法复用业务层，故不满足 ARCH-1。

不引入动态 syscall 注册：当前编号是固定 UAPI，静态 `const` 分发表更易审计，也不增加启动顺序要求。

## 3. 分层与接口契约

`kernel/include/syscall/dispatch.h` 定义内部请求 `syscall_ctx_t`：`uint64_t nr`、`uint64_t args[6]`、一个仅供特殊调用使用的 opaque `void *arch_frame`，以及初始为 `false` 的 `bool suppress_writeback`。分发接口为 `int64_t syscall_dispatch(syscall_ctx_t *ctx)`；表项处理函数为 `int64_t (*syscall_handler_t)(syscall_ctx_t *)`。普通处理函数只能读取 `nr/args`，不能强转或解引用 `arch_frame`，也不改变 `suppress_writeback`。分发表长度由 UAPI 最大编号加一确定，条目用 `[SYS_name]` 指定；调试名称与处理函数处于同一条目，避免两张表漂移。无处理函数的编号沿用当前默认 `-EINVAL`，包括 62；越界编号同样沿用当前默认值。ARCH-2 可单独决定是否改为 `-ENOSYS`。

x86_64 `do_system_call` 的顺序固定为：保留现有调试栈检查；对 `PF_LINUX_ABI` 进程运行**原有**编号翻译；从寄存器构造 `syscall_ctx_t`；调用 `syscall_dispatch`；当 `suppress_writeback` 为 `false` 时写回 `rax`；仅当返回现场是用户 CPL=3 时运行现有信号投递。`SYS_sigreturn` 成功恢复现场后将 `suppress_writeback` 设为 `true`，防止已恢复的 `rax` 被普通返回值覆盖；校验失败时仍按普通路径写回错误码。`do_exit` 和成功的相关现场切换保持原有不可返回/返回行为。Linux 翻译逻辑仍留在 x86_64 分发前路径；其 `int8_t` 表项、未映射项和结构体兼容问题明确留给 ARCH-2。

`kernel/include/arch/syscall.h` 声明少数现场与平台接口，x86_64 实现位于 `kernel/arch/x86_64/intr/`：用户态 fork 现场复制、exec 入口现场提交、sigreturn 帧恢复，以及当前 syscall 使用的架构电源动作。通用 `sys_proc.c` 保留 `exec` 的路径/argv/envp 深拷贝及上限检查，并在旧地址空间失效前完成；架构钩子接收已复制的内核参数。`sigreturn` 的 x86_64 sigframe 布局、校验和寄存器恢复均保留在 x86_64 实现中。现有 `sched/task.c::do_fork/sys_exec` 内部的架构耦合不在 ARCH-1 中重构；钩子封装其当前调用路径，不宣称这些内部函数已跨架构通用。

`SYS_putchar` 的 framebuffer/串口输出和 `SYS_reboot` 的 ACPI/端口动作通过平台接口调用，避免把 x86 `outb/outw/hlt` 带入 `kernel/syscall/`。用户指针检查继续使用 `memory/uaccess` 的 fault-tolerant 路径，保持检查、拷贝、释放顺序；不因搬迁而直接解引用用户指针。

## 4. 文件职责与迁移顺序

| 文件 | 职责 |
|---|---|
| `kernel/include/syscall/dispatch.h`、`kernel/syscall/dispatch.c` | 请求/处理函数接口、静态分发表、名称与未知编号行为 |
| `kernel/syscall/sys_fs.c` | 文件、路径、目录、poll/select 入口适配及现有 symlink 系列处理 |
| `kernel/syscall/sys_proc.c` | 进程、exec 用户输入拷贝、进程组及信号 syscall 的非现场逻辑 |
| `kernel/syscall/sys_mm.c` | brk、mmap、mprotect、munmap、futex |
| `kernel/syscall/sys_time.c`、`sys_net.c`、`sys_misc.c` | 时间、socket/网络、其余 syscall |
| `kernel/include/arch/syscall.h`、`kernel/arch/x86_64/intr/syscall_frame.c` | 特殊现场和平台动作接口的 x86_64 实现 |
| `kernel/arch/x86_64/intr/trap.c` | 异常、x86 syscall 寄存器适配、现有 Linux 翻译与信号返回检查 |
| `kernel/sched/task.c` | 移出四个 symlink 系列 syscall 实现；保留其余调度/进程内部实现 |
| `kernel/Makefile`、`docs/syscall/syscall.md` | x86_64 通用源码发现及文档更新 |

迁移按五个可验证阶段进行：

1. 建立通用请求、分发表与 x86_64 入口适配。迁移期间，x86_64 入口仅将已迁移编号送入新表，其余编号暂交旧 `switch`；临时双路径在第 4 阶段删除。
2. 按 FS、内存、时间、网络、其余普通调用搬迁处理函数；每组独立编译和运行对应回归。
3. 迁移 `fork/exec/sigreturn` 的现场相关路径及 `putchar/reboot` 平台动作；保留信号投递时序。
4. 删除旧业务 `switch`，整理 include/函数原型；将四个 symlink 系列实现移至 `sys_fs.c`，与 ARCH-4 的后续拆分方向一致。
5. 完整清理构建并执行回归，更新 syscall 文档；仅在验证通过后宣称 ARCH-1 完成。

aarch64 显式源码列表本轮不强行加入整个 `syscall/`：目前缺少其用户态和多个通用处理函数依赖。接口签名须允许未来从 `svc` 的编号/六参数构造同一请求，aarch64 接入与运行测试属于后续里程碑。

## 5. 验证与风险控制

必须覆盖：OS01 编号 0..74 的原有 systest；六参数 `mmap/pselect6/sendto`；`fork` 父子返回值；`exec` 成功现场及恶意 argv/envp；`sigreturn` 后寄存器/栈恢复与待处理信号；错误用户指针；未知编号和未实现的 62；正常启动的 BusyBox Linux ABI 路径。不得把 `KERNEL_SELFTEST=1` 与 systest 同时运行。

完成代码迁移后运行 `make clean`（若结构体或接口布局改变则尤为必要），再运行 `make OS01_SYSTEST=1 test-syscall`、`make test-host`、`make test-static`；内核自测另用 `make KERNEL_SELFTEST=1 test-kernel-selftest`。检查 `kernel/syscall/` 不含 `regs->`、per-arch include 或 x86 内联汇编，并检查构建输入确实包含 `syscall/*.c`。这些命令是实施验收要求，设计阶段不声称已经通过。

最主要的回归风险是 `sigreturn` 的恢复值被普通返回值覆盖、`exec` 在旧地址空间释放后仍访问用户指针、fork 子现场返回值不为 0、Linux 翻译前后顺序变化，以及业务搬迁时丢失 fault-tolerant copy 或改变错误码。各阶段要以实际运行结果验证这些边界；发生失败时先修复当前阶段再继续搬迁。

# 架构评审 — Group 9: 用户态 + libc

> **审查日期**: 2026-07-25
> **覆盖文件**: `user/crt0.S`, `sigreturn_trampoline.S`, `init.c`, `systest.c`, `libc/include/**`, `libc/pthread/mutex.c`, `user/Makefile`

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P2 | libc | `int $0x80` 系统调用接口比 `syscall` 指令慢 | 待处理 |
| 2 | P2 | libc | BSS 清零使用 `rep stosb` 而非更快的 `rep stosq` | 待处理 |
| 3 | P2 | crt | 无 `__stack_chk_guard` 初始化，`-fstack-protector` 兼容性未测试 | 待处理 |
| 4 | P3 | libc | `crt0.S` 硬编码 `SYS_exit = 2`，与 `syscall.h` 无编译时关联 | 待处理 |
| 5 | P3 | libc | 无 TLS 支持（pthread 的 futex 锁可用但无 `fs`/`gs` 段用户态切换） | 待处理 |
| 6 | P3 | libc | 无动态链接器（仅支持静态链接 ELF） | 待处理 |
| 7 | P3 | libc | libc 仅提供内联 syscall 包装，无标准函数（printf/scanf/malloc 等） | 待处理 |

---

### [P2] 1. `int $0x80` vs `syscall` 指令

- **位置**: `libc/include/sys/syscall.h:65`
- **现象**: 系统调用通过 `int $0x80`（中断门）实现。x86_64 提供更快的 `syscall`/`sysret`（`IA32_STAR` MSR）路径。每次 syscall 额外开销约 30-50 周期（中断门相比于 MSR-based syscall）
- **建议**: 添加 `syscall` 指令支持（设置 `IA32_STAR`/`IA32_LSTAR` MSR），保留 `int $0x80` 作为兼容路径

### [P2] 2. BSS 清零使用 `rep stosb`

- **位置**: `user/crt0.S:21`
- **现象**: `rep stosb` 每次迭代仅写入 1 字节。`rep stosq` 每次写入 8 字节，对较大的 BSS 段（如 busybox 的 BSS 可达数百 KB）有 8 倍性能提升
- **建议**: 改为 `movq %rax, %rdi` 初始化后使用 `rep stosq`

### [P2] 3. 无 `__stack_chk_guard` 初始化

- **位置**: `user/crt0.S:1-35`
- **现象**: crt0.S 不清除栈保护的 canary。如果用户代码以 `-fstack-protector` 编译（被动启用），首次栈检查会读取未初始化的 `__stack_chk_guard` 并 panic
- **建议**: 如果编译用户代码时启用了栈保护，在 crt0.S 中设置 `__stack_chk_guard`

### [P3] 4. `crt0.S` 硬编码 `SYS_exit = 2`

- **位置**: `user/crt0.S:31`
- **现象**: `movl $2, %eax` 硬编码 exit 系统调用号。如果 `syscall.h` 的编号发生变化，crt0.S 不会自动同步
- **建议**: 从 `libc/include/sys/syscall.h` 生成汇编文件 `syscall_offsets.S`，或添加注释警告修改 syscall 编号时需同步

### [P3] 5. 无 TLS 支持

- **位置**: `libc/pthread/mutex.c`
- **现象**: pthread 互斥锁使用基于 futex 的原子操作实现（有效），但用户态无 TLS（线程本地存储）支持。每个线程共享相同的 `fs`/`gs` 基址，无法在用户态识别当前线程
- **建议**: 未来支持：在 `arch_task_init_platform` 中为每个用户任务设置唯一的 `FS.base`，libc 管理 `fs` 段

### [P3] 6. 无动态链接器

- **位置**: 全系统
- **现象**: 所有用户空间 ELF 必须静态链接。无法使用共享库（.so）
- **建议**: 未来工作：添加 ELF 解释器支持和 `mmap` 文件映射

### [P3] 7. libc 极简

- **位置**: `libc/`
- **现象**: libc 仅包含内联 syscall 包装和 `pthread/mutex.c`。无 `printf`/`scanf`/`malloc`/`strtol` 等。所有用户态程序（busybox）必须自带这些功能
- **建议**: 在 libc 中添加基本功能的轻量级实现，或集成 newlib/musl

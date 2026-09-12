# trap.c 源码导读（3065 行，最大单文件，读不懂时从这里进）

> `docs/interrupt.md` + `docs/syscall.md` 是参考手册；本文件是 `kernel/arch/x86_64/trap.c` 的**阅读路线**。

## trap.c 全景

```
1-70      前置声明（fs 侧 sys_* 原型，签名带 pt_regs_t）
68-157    user_va_to_phys() + 异常公共辅助
158-800   do_* 异常 handler ×20（divide_error … virtualization_exception）
          ├─ 简单 log+panic 类可整段跳过
          ├─ do_debug (L169) / do_int3 (L189)：断点单步，调 syscall 时会经过
          ├─ do_double_fault (L243) / do_stack_segment_fault (L340)：栈爆炸现场
          └─ ★ do_page_fault (L435)：COW/mmap 缺页核心，必读
800-1000  arch_do_signal_delivery (L798) + signal hook
1099-3035 ★ do_system_call (L1099)：75 个 syscall 的 switch 分发（case SYS_*）
3036-末尾 sys_vector_install()：IDT 装载
```

## 推荐阅读顺序（3 遍法）

**第 1 遍·两条入口**：
1. entry.S `ENTRY(page_fault)` (L269) / `ENTRY(system_call)` (L310) → 各自压栈建 pt_regs → 跳 C handler
2. `do_system_call` (L1099) 开头三件事（都在前 60 行）：
   - 栈溢出 guard（NDEBUG 包裹，RSP 距栈底 <2KB 告警）
   - **Linux ABI 翻译表**（L1114，`PF_LINUX_ABI` 任务 rax<320 时查 `linux_to_os01[]`）——busybox 能直接跑的原因
   - switch (regs->rax) 75 个 case → `case SYS_exec` (L1344) 值得细读（地址空间切换）

**第 2 遍·缺页异常**（`do_page_fault` L435，~290 行）：
按 error code 位拆解：present? user? write? → 走 vma_find → COW 判定（配合 docs/cow-mmap.md）→ mmap 匿名缺页 → 合法性拒绝（SIGSEGV）。读完这条你就能解释"fork 后写一下为什么会 copy page"。

**第 3 遍·信号投递**：`arch_do_signal_delivery` (L798)——检查 pending、构造用户态 trampoline 栈帧（配合 docs/signal.md 和 user/sigreturn_trampoline.S）。

## 已知坑

- 异常 handler 签名统一带 `error_code`，但**不是所有异常都有硬件 error code**——entry.S 里没有的会 push 伪值占位，读 entry.S 时留意每个 ENTRY 的压栈差异
- `do_general_protection` (L378) ~57 行是最长异常 handler，通常是段/权限问题现场，遇到 GP fault 优先看它打的 log
- syscall 分发是**巨型 switch 不是函数表**，加 syscall 要同步改 `kernel/include/uapi/syscall.h` + trap.c + （如需 Linux 兼容）翻译表三处

## 自测问题

1. 为什么 `int3` 断点能触发而 `syscall` 不走中断门？两者栈切换差异是什么？
2. 缺页发生在内核态拷贝用户指针时（uaccess），handler 怎么区分恶意指针和合法按需映射？
3. `PF_LINUX_ABI` 任务调 `SYS_fork` 时 rax 里是什么值？翻译表为什么上限是 320？

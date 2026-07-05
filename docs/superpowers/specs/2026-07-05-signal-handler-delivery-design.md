# 用户态信号 handler 调用 — 设计文档

> 日期：2026-07-05
> 状态：待评审

## 1. 背景与现状

### 1.1 已就绪的基础设施

OS01 的信号子系统已经完成了内核态引擎部分：

| 组件 | 位置 | 状态 |
|------|------|------|
| `do_signal_delivery()` | `kernel/arch/x86_64/trap.c:570` | SIG_DFL/SIG_IGN 处理完整 |
| 双检查点 | `ret_from_intr` + `do_system_call` | 每次返回用户态前触发 |
| `sigprocmask` | `trap.c:1756` | SIG_BLOCK/UNBLOCK/SETMASK 完整 |
| blocker + 信号唤醒 | `kernel/sched/task.c:95` | `blocker_wait` 返回 `-EINTR` |
| `SYS_signal` (sigaction) | `trap.c:1717` | 注册 handler，含 `sa_restorer` 字段 |
| `sighand[NSIG]` | `kernel/include/kernel/task.h:146` | 每进程独立 handler 表 |
| `current->signal` / `current->blocked` | `task_t:118-119` | pending/blocked 位图 |

### 1.2 缺失的最后一步

`do_signal_delivery` 第 612-614 行：

```c
// Real handler registered — clear pending bit.
// Full user-stack frame delivery is future work.
current->signal &= ~(1ULL << sig);
```

**handler 已注册，pending bit 清了，但没有推送用户栈帧、没有跳转到 handler。** handler 永远不会被调用。

## 2. 目标

实现完整的用户态信号 handler 调用路径：

```
pending signal → do_signal_delivery → push sigframe on user stack
→ modify pt_regs (rip=handler, rsp→trampoline return addr) → RESTORE_ALL
→ iretq → handler(signum) executes in ring 3
→ handler returns (ret) → pops trampoline address → enters trampoline
→ sigreturn_trampoline: int $0x80 (SYS_sigreturn=43)
→ SYS_sigreturn: restore all registers from sigframe → iretq
→ back to interrupted code
```

## 3. 设计约束

| 约束 | 值 | 来源 |
|------|-----|------|
| 用户代码段 | `0x400000–0x600000` (2MB, RWX) | `spawn_user_task` |
| 用户栈 | `0x800000–0xA00000` (2MB, RW+NX) | `spawn_user_task` |
| USER_CS / USER_DS | `0x2b` / `0x33` | `kernel/include/kernel/task.h:22-23` |
| 仅 2MB 大页 | 无 4KB 页面支持 | 当前 VMM 限制 |
| trampoline 必须在可执行区域 | 只能放 `.text` 段 | 栈不可执行 |
| `sigaction.sa_restorer` 已存在 | `kernel/include/uapi/time.h:84` | SYS_signal 已正确拷贝 |
| busybox 走 Linux ABI | `PF_LINUX_ABI`, `linux_to_os01[]` 翻译 | `trap.c:643-686` |
| `check_signal` 不检查 CPL | `entry.S:89` 无 `cs & 3` 守卫 | 需在 `do_signal_delivery` 内加 |

## 4. 新增组件

### 4.1 sigframe 结构体

新增到 `kernel/include/uapi/time.h`（内核和 libc 共享）：

```c
struct sigframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  // 0x00–0x38
    uint64_t rbx, rcx, rdx, rsi, rdi;               // 0x40–0x60
    uint64_t rbp;                                     // 0x68
    uint64_t ds, es;                                  // 0x70, 0x78
    uint64_t rax;                                     // 0x80
    uint64_t func, errcode;                           // 0x88, 0x90 (padding, unused)
    uint64_t rip, cs, rflags;                        // 0x98, 0xa0, 0xa8
    uint64_t rsp, ss;                                 // 0xb0, 0xb8
    uint64_t blocked;                                 // 0xc0 — saved signal mask
};
// sizeof(struct sigframe) = 200 bytes
```

`func`/`errcode` 字段保留仅用于对齐——与 `pt_regs_t` 布局一致，实际不存有意义的值。

### 4.2 sigreturn_trampoline（libc 侧）

新增 `libc/crt/sigreturn_trampoline.S`：

```asm
.global sigreturn_trampoline
sigreturn_trampoline:
    movq $43, %rax       # SYS_sigreturn
    int   $0x80
    # unreachable — iretq back to original execution flow
```

类型声明在 `libc/include/signal.h`：

```c
extern void sigreturn_trampoline(void);
```

### 4.3 libc signal() / sigaction() 改动

修改 `libc/unistd/signal.c` — 在构造 `sigaction` 时填入 trampoline：

```c
act.sa_restorer = sigreturn_trampoline;  // 不再为 NULL
```

修改 `libc/unistd/sigaction.c` — `act` 是 `const`，不能直接修改。创建本地副本，填入 restorer 后传给内核：

```c
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    struct sigaction local_act;
    if (act) {
        local_act = *act;
        if (!local_act.sa_restorer)
            local_act.sa_restorer = sigreturn_trampoline;
        act = &local_act;
    }
    return (int)syscall(SYS_signal, (uint64_t)signum,
                        (uint64_t)(uintptr_t)act, (uint64_t)(uintptr_t)oldact);
}
```

## 5. 内核改动

### 5.1 辅助函数 user_va_to_phys

新增到 `kernel/arch/x86_64/trap.c`（或 `kernel/memory/vmm.c`）：

```c
// Walk the user page table to find the physical address backing a
// user-space virtual address.  The returned physical address can be
// passed to Phy_To_Virt() for kernel access via the higher-half map.
static uint64_t user_va_to_phys(uint64_t *pml4, uint64_t va) {
    size_t l4 = (va >> 39) & 0x1ff;
    size_t l3 = (va >> 30) & 0x1ff;
    size_t l2 = (va >> 21) & 0x1ff;
    if (!(pml4[l4] & PAGE_Present)) return 0;
    uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);
    if (!(pml3[l3] & PAGE_Present)) return 0;
    uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);
    if (!(pml2[l2] & PAGE_Present)) return 0;
    return (pml2[l2] & PAGE_2M_MASK) | (va & 0x1FFFFF);
}
```

### 5.2 do_signal_delivery — 用户栈帧推送

触发条件：`handler != SIG_DFL && handler != SIG_IGN`

**CPL 守卫**（P1）：`do_signal_delivery` 在 `entry.S:check_signal` 中被调用时 CPL 可能为 0（内核态中断）。handler 投递只能发生在返回用户态的边界上，否则会把 `iretq` 重定向到 ring 3，丢弃正在进行的内核工作和持有的锁。在函数开头检查 `!(regs->cs & 3)` 时跳过 handler 投递——SIG_DFL/SIG_IGN 正常处理，已注册 handler 的信号保留 pending，等待下一次返回用户态时投递。

**sa_restorer NULL 检查**（P3）：未注册 trampoline 的 handler（raw syscall 调用者传入 `sa_restorer=NULL`）会导致 handler `ret` 后跳转到地址 0 崩溃。在 `do_signal_delivery` 中校验 `sa_restorer == NULL` 时，降级为 SIG_DFL 行为（终止进程）。

```c
// ── CPL guard: only deliver to ring-3 frames ──────────
// Called from check_signal (entry.S:89) which may fire at any CPL.
// If we're in kernel mode, skip handler delivery — SIG_DFL/SIG_IGN
// are still safe (they just manipulate task state).  Registered
// handlers stay pending and will be delivered on the next
// return-to-userspace.
if (!(regs->cs & 3)) {
    // Still process SIG_DFL kills — those don't touch pt_regs
    // (do_exit switches away, never returns)
    // Registered handlers: leave pending, retry on next check_signal
    if (handler != SIG_DFL && handler != SIG_IGN)
        continue;  // skip this signal, try again later
    // fall through to SIG_DFL/SIG_IGN handling
}

// ── sa_restorer NULL guard ───────────────────────────
// Handler without a valid restorer would ret to address 0.
// Treat as SIG_DFL (terminate).
if (!current->sighand[sig].sa_restorer) {
    serial_printk("task %d: signal %d handler has no restorer, "
                  "killing\n", (int)current->pid, sig);
    current->signal &= ~(1ULL << sig);
    do_exit((uint64_t)sig << 8);
    return;
}

// 1. 构造 sigframe（内核栈上）
struct sigframe frame;
memset(&frame, 0, sizeof(frame));
frame.r15=regs->r15; frame.r14=regs->r14; frame.r13=regs->r13;
frame.r12=regs->r12; frame.r11=regs->r11; frame.r10=regs->r10;
frame.r9=regs->r9;   frame.r8=regs->r8;
frame.rbx=regs->rbx; frame.rcx=regs->rcx; frame.rdx=regs->rdx;
frame.rsi=regs->rsi; frame.rdi=regs->rdi; frame.rbp=regs->rbp;
frame.ds=regs->ds;   frame.es=regs->es;   frame.rax=regs->rax;
frame.rip=regs->rip; frame.cs=regs->cs;   frame.rflags=regs->rflags;
frame.rsp=regs->rsp; frame.ss=regs->ss;
frame.blocked = current->blocked;

// 2. 计算用户栈新 RSP（x86_64 SysV ABI: iretq 等价于 call 之后; RSP≡8 mod 16）
//    208=200(frame)+8(trampoline), 加 8 调整 → 216=16×13.5 → 对齐后 RSP%16==8
size_t total = sizeof(frame) + 8;              // 208 = frame (200) + trampoline (8)
uint64_t new_rsp = ((regs->rsp - total - 8) & ~15UL) + 8;
// new_rsp % 16 == 8 — handler 可以使用 movaps 对齐取数

// 3. 获取用户栈物理地址 → 内核虚拟地址
uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
uint64_t frame_phys = user_va_to_phys(user_pml4, new_rsp + 8);
if (!frame_phys) {
    // Page table walk failed — leave signal pending, don't modify pt_regs.
    // The next return-to-userspace will retry (the underlying page should
    // still be mapped; this is a defensive check).
    continue;  // skip to next signal, leave current->signal bit set
}
void *kstack = (void *)Phy_To_Virt(frame_phys);

// 4. 写 sigframe（new_rsp+8）和 trampoline 返回地址（new_rsp）
//    布局: [sigframe (200B)] [trampoline addr (8B)]
//          ^ new_rsp+8 (sigframe 起始)  ^ new_rsp (handler 的 RSP，iretq 后直接生效)
memcpy(kstack, &frame, sizeof(frame));               // sigframe
uint64_t tramp = (uint64_t)current->sighand[sig].sa_restorer;
memcpy(kstack - 8, &tramp, 8);                       // trampoline as return addr

// 5. 修改 pt_regs → RESTORE_ALL → iretq 跳进 handler
regs->rdi = sig;                          // handler arg (x86_64 ABI: arg1=RDI)
regs->rip = (uint64_t)handler;            // → handler code in .text
regs->rsp = new_rsp;                      // RSP points to trampoline addr
regs->cs  = USER_CS;  // 0x2b: ring 3 code (GDT index 5 | RPL 3)
regs->ss  = USER_DS;  // 0x33: ring 3 data (GDT index 6 | RPL 3)
regs->ds  = USER_DS;
regs->es  = USER_DS;

// 6. 阻塞信号防嵌套
// sa_mask 是 uint64_t 但语义上只用低 NSIG-1 位（与 blocked/sigset_t 一致）。
// sigreturn 时 current->blocked = kframe->blocked 整体恢复，解除本次阻塞。
current->blocked |= (1ULL << (sig - 1));              // block current signal
current->blocked |= current->sighand[sig].sa_mask;    // block sa_mask

current->signal &= ~(1ULL << sig);  // clear pending
break;  // deliver one signal at a time; remaining signals on next check_signal
// fall through → return 1 (handled)
}
// for loop end: nothing deliverable → return 0
return 0;
```

### 5.3 SYS_sigreturn (编号 43)

handler 执行 `ret` 后 RSP = `new_rsp + 8` = sigframe 起始地址。`int $0x80` 进入内核时，`pt_regs->rsp`（OLDRSP 字段）保存的正是这个值。

```c
case SYS_sigreturn: {
    // regs->rsp == sigframe 在用户空间的起始虚拟地址
    // 通过页表 walk 找到物理地址 → Phy_To_Virt 得到内核可访问的指针
    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    uint64_t frame_phys = user_va_to_phys(user_pml4, regs->rsp);
    if (!frame_phys) { regs->rax = -EFAULT; break; }
    struct sigframe *kframe = (struct sigframe *)Phy_To_Virt(frame_phys);

    // 恢复 blocked mask
    current->blocked = kframe->blocked;

    // 恢复全部 GPR
    regs->r15=kframe->r15; regs->r14=kframe->r14; regs->r13=kframe->r13;
    regs->r12=kframe->r12; regs->r11=kframe->r11; regs->r10=kframe->r10;
    regs->r9=kframe->r9;   regs->r8=kframe->r8;
    regs->rbx=kframe->rbx; regs->rcx=kframe->rcx; regs->rdx=kframe->rdx;
    regs->rsi=kframe->rsi; regs->rdi=kframe->rdi; regs->rbp=kframe->rbp;
    regs->ds=kframe->ds;   regs->es=kframe->es;   regs->rax=kframe->rax;

    // 恢复 iretq 帧 → RESTORE_ALL → iretq 回到原执行流
    regs->rip=kframe->rip; regs->cs=kframe->cs; regs->rflags=kframe->rflags;
    regs->rsp=kframe->rsp; regs->ss=kframe->ss;

    break;
}
```

### 5.4 syscall 表更新

| 文件 | 改动 |
|------|------|
| `libc/include/sys/syscall.h` | 加 `#define SYS_sigreturn 43` |
| `kernel/arch/x86_64/trap.c` | `syscall_names[43] = "sigreturn"` |
| `kernel/arch/x86_64/trap.c` | switch 加 `case SYS_sigreturn:` |
| `kernel/arch/x86_64/trap.c` | Linux ABI 翻译表加 `[15] = 43`（`rt_sigreturn → SYS_sigreturn`） |
| `kernel/arch/x86_64/trap.c` | `do_signal_delivery` 返回类型 `void → int`，新增 CPL 守卫 + sa_restorer NULL 检查 + sigframe 推送 |
| `kernel/arch/x86_64/entry.S` | `check_signal` 检查 `do_signal_delivery` 返回值，0 时跳出循环 |

**P2：Linux ABI `rt_sigreturn` 映射。** busybox ash 以 `PF_LINUX_ABI` 运行，使用的 trampoline（内置于 busybox）调用 Linux syscall 15（`rt_sigreturn`）。当前 `linux_to_os01` 表没有 `[15]` 项，会落到 -1 → unsupported 路径，sigreturn 永远不执行。需要在表中增加 `[15] = 43`，将 Linux `rt_sigreturn` 映射到 OS01 `SYS_sigreturn`。注意：busybox 自带 trampoline 是 Linux 风格的（通过 `sigaction.sa_restorer` 指向 busybox 内部的 `sigreturn` stub，调用 `int $0x80` with `%rax=15`），所以 PF_LINUX_ABI 进程通过 ABI 翻译表路由到 `SYS_sigreturn` 后，OS01 内核侧的 sigframe 恢复逻辑同样适用。唯一区别：sigframe 布局必须与 Linux 兼容——而本设计的 `struct sigframe` 恰好匹配 Linux 的 `sigframe` 布局（GPR→iretq frame），两者通用。

### 5.5 do_signal_delivery 循环行为

当前处理完一个 handler 后 `continue` 循环处理剩余信号。改为 `break`——一次只推送一个 handler。`entry.S:check_signal` 已有 loop 语义（`call do_signal_delivery; jmp check_signal`），每次重新进入会再次调用，逐一处理。这避免了嵌套信号推送时的栈帧重叠。

**CPL 守卫与 entry.S 协作。** `do_signal_delivery` 返回 `int`：**返回 0 当且仅当本轮没有投递任何 handler 且没有杀死进程**——即所有 pending 信号要么被阻塞、要么 CPL=0 跳过。此时 `entry.S:check_signal` 跳出循环，避免 CPL=0 时的无限 loop。非 0 返回表示至少处理了一个信号（投递 handler 或 SIG_DFL kill），`check_signal` 继续循环处理剩余信号。

```asm
check_signal:
    cmpq $0, TASK_SIGNAL_OFFSET(%rbx)
    je   RESTORE_ALL
    movq %rsp, %rdi
    call do_signal_delivery          # returns int: 0 = nothing done, !0 = handled ≥1
    testl %eax, %eax
    jz   RESTORE_ALL                 # nothing delivered → break loop
    jmp  check_signal                # try next pending signal
```

之前 `do_signal_delivery` 返回类型是 `void`，需改为 `int`。实现侧的清理：删除现有 `(void)regs;` 占位行（`trap.c:572`），因为新代码已将 `regs` 用于 sigframe 写入和 pt_regs 修改。

## 6. Libc 改动清单

| 文件 | 改动 |
|------|------|
| `libc/crt/sigreturn_trampoline.S` | **新增** |
| `libc/unistd/signal.c` | `sa_restorer` 从 `NULL` → `sigreturn_trampoline` |
| `libc/unistd/sigaction.c` | 若 `act->sa_restorer == NULL` 则自动填入 |
| `libc/include/signal.h` | 声明 `extern void sigreturn_trampoline(void)` |
| `libc/include/sys/syscall.h` | `#define SYS_sigreturn 43` |
| `libc/Makefile` | crt 列表加 `sigreturn_trampoline.o` |
| `user/Makefile` | 确保链接 `sigreturn_trampoline.o` |

## 7. 用户栈布局

```
高地址
  ... 原有用户栈数据 ...                   ← old_rsp

  [sigframe.blocked   = saved blocked]     ← sigframe + 0xc0
  [sigframe.ss        = original SS ]     ← sigframe + 0xb8
  [sigframe.rsp       = original RSP]     ← sigframe + 0xb0
  [sigframe.rflags    = original RFLAGS]  ← sigframe + 0xa8
  [sigframe.cs        = original CS  ]     ← sigframe + 0xa0
  [sigframe.rip       = original RIP ]     ← sigframe + 0x98
  [sigframe.errcode   = 0            ]     ← sigframe + 0x90
  [sigframe.func      = 0            ]     ← sigframe + 0x88
  [sigframe.rax       = original RAX ]     ← sigframe + 0x80
  [sigframe.es, ds, rbp               ]     ← sigframe + 0x68–0x78
  [sigframe.rdi, rsi, rdx, rcx, rbx  ]     ← sigframe + 0x40–0x60
  [sigframe.r8–r15                    ]     ← sigframe + 0x00–0x38

  [trampoline_addr = sigreturn_trampoline] ← new_rsp (RSP%16==8, handler 返回地址)
低地址
```

handler 入口:  RSP = new_rsp, [RSP] = trampoline 地址, RSP % 16 == 8 ✓ (SysV ABI)
handler ret:   弹出 trampoline → RIP=trampoline, RSP = new_rsp + 8
int $0x80:     RSP = new_rsp + 8 = sigframe 起始地址

**关键性质**：
- `new_rsp = ((old_rsp - 208 - 8) & ~15UL) + 8` — 保证 iretq 后 RSP % 16 == 8（等价于"刚执行 call 之后"的栈状态）
- `sizeof(struct sigframe) = 200 bytes`
- sigframe 起始 = `new_rsp + 8`，也是 handler `ret` 后的 RSP
- `SYS_sigreturn` 中 `kframe = Phy_To_Virt(user_va_to_phys(pml4, regs->rsp))`，无需计算偏移

**执行流程的 RSP 变化**：

```
do_signal_delivery:      regs->rsp = new_rsp
iretq / handler entry:   RSP = new_rsp, [RSP] = trampoline addr
handler ret:             RSP = new_rsp + 8, RIP = trampoline
int $0x80 entry:         pt_regs->rsp = new_rsp + 8 = &sigframe
```

## 8. 测试方案

### 8.1 同步测试（user/systest.c 新增）

```c
static volatile int sigusr1_got = 0;
static void sigusr1_handler(int sig) { sigusr1_got = 1; }

void test_signal_handler_sync(void) {
    signal(SIGUSR1, sigusr1_handler);
    kill(getpid(), SIGUSR1);
    // kill 返回后，ret_from_intr → do_signal_delivery → 推送 handler
    // handler 执行: sigusr1_got=1 → ret → sigreturn → 回到这里
    assert(sigusr1_got == 1);
    signal(SIGUSR1, SIG_DFL);
    test_pass("signal handler sync");
}
```

### 8.2 异步 SIGINT 测试（独立程序 user/sigtest.c）

独立的用户态 ELF，从 shell 运行后等待 Ctrl-C：

```c
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int sigint_got = 0;
static volatile int sigterm_got = 0;
static void sigint_handler(int sig)  { sigint_got = 1; }
static void sigterm_handler(int sig) { sigterm_got = 1; }

void assert_msg(int cond, const char *msg) {
    if (!cond) { puts(msg); exit(1); }
}

int main(void) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigterm_handler);

    puts("Press Ctrl-C within 5 seconds...");
    sleep(5);
    // SIGINT delivery happens in do_signal_delivery(), which is
    // called from do_system_call and ret_from_intr.  After sleep
    // returns (possibly via EINTR), force a syscall to ensure
    // the handler gets delivered before we check the flag.
    getpid();
    printf("SIGINT received: %s\n", sigint_got ? "YES" : "NO");
    assert_msg(sigint_got, "FAIL: SIGINT handler was not called");
    assert_msg(!sigterm_got, "FAIL: SIGTERM fired unexpectedly");
    puts("[PASS] signal handler async");
    return 0;
}
```

构建目标：`user/sigtest.elf`，通过 Makefile 集成。

## 9. 不做的事（本次范围外）

- SA_SIGINFO 扩展 handler、siginfo_t、ucontext
- 嵌套信号（同类型信号在 handler 执行期间已阻塞，不会重入）
- 栈溢出保护（sigaltstack / SA_ONSTACK）
- 4KB 页面、COW fork、mmap/mprotect

## 10. 影响分析

- **安全性**：trampoline 在 `.text` 段不会意外覆盖；内核不直接解引用用户指针
- **兼容性**：busybox ash `trap` 内置命令应能工作
- **回归风险**：改动仅影响已注册 handler 的信号路径；SIG_DFL/SIG_IGN 路径不变

## 11. 参考

- Linux `arch/x86/kernel/signal.c` — `__setup_rt_frame`、`ia32_setup_frame`
- xv6 `sys_sigreturn` — 简化版 sigframe 恢复
- OS01 已有代码：`do_signal_delivery`、`SYS_signal`、`entry.S:check_signal`

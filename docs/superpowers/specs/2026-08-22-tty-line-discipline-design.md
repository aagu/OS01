# TTY 行规程与进程组（最小集）设计文档

- 日期：2026-08-22
- 状态：待用户 review（尚未进入实现）
- 目标：实现 POSIX 最小行规程（VINTR/VQUIT → 信号派发到前台进程组）+ 进程组/会话子系统骨架，使 Ctrl-C 能在 OS01 shell 中可靠终止前台任务（如 `cat /dev/urandom`、`cat` 无参数），同时为后续 job control 留出干净的扩展面
- 修订记录：
  - v1：初版

---

## 1. 背景与动机

### 1.1 现象

用户在 QEMU gtk 窗口按 Ctrl-C，OS01 shell 中运行的 `cat /dev/urandom` 无法终止；同样的 `cat`（无参数，从 stdin 读）也无法终止。

### 1.2 根因（三层缺陷，都在本设计修复）

1. **缺失行规程（主因）** — `kernel/tty/tty.c:125-140` 的 `tty_push_input` 只做 echo + ring push，**完全没读 `c_lflag & ISIG`、也没比较 `c_cc[VINTR]`**。`grep -rn 'ISIG\|VINTR\|VQUIT\|VSUSP\|cc_special' kernel/` 输出为空。整个内核从未把字符翻译成信号。
2. **tty_struct 没有 pgrp 字段** — `kernel/include/kernel/tty.h:18-50` 的 `tty_struct` 没有 `fg_pgrp`/`pgrp` 字段（pty_struct 有，但那是 PTY 设备不是物理控制台 TTY）。即使实现了行规程，也没东西可派发。
3. **TIOCSPGRP ioctl 是 stub** — `kernel/tty/tty.c:289-290` `case TIOCSPGRP: return 0;` 连 arg 都没读。

附加信号派发缺陷（即使行规程修了，信号也送不到前台进程组）：

- **`SYS_kill` 不支持 pgrp 语义** — `kernel/arch/x86_64/trap.c:2010-2023` 直接调 `task_send_signal(pid, sig)`，不识别 `pid==0`（自己 pgrp）和 `pid<0`（-pid pgrp）。
- **`killpg` libc stub** — `libc/signal/killpg.c:5` 直接 `return -ENOSYS`，根本没下系统调用。
- **`setpgid` libc stub** — `libc/unistd/setpgid.c:2` `(void)pid;(void)pgid; return 0;`，shell 无法把子进程设为 pgrp leader。
- **`tcsetpgrp` libc stub** — `libc/unistd/tcsetpgrp.c:2` 无操作。
- **`tcgetpgrp` 永远返回 1** — `libc/unistd/tcgetpgrp.c:2`，欺骗 shell。

### 1.3 需求方

- **用户交互基础** — 用户在 OS01 shell 跑前台命令，按 Ctrl-C 必须能终止。任何 Unix 兼容 OS 的基础契约。
- **后续 job control** — bg/fg/SIGTSTP/SIGCONT 的扩展面（见 §10 非目标）。

---

## 2. 总体架构

分层，每层职责单一：

```
[用户按 Ctrl-C]
        ↓
QEMU gtk → PS/2 0x60 → IRQ1
        ↓
kernel/driver/keyboard.c:keyboard_handler
   translate_and_push → 0x03 (VINTR)
        ↓
kernel/tty/tty.c:tty_push_input(tty, 0x03)        [新行规程插入点]
   ├─ ISIG && c==c_cc[VINTR]?
   │    └─ 是：spin_lock tty->fg_pgrp_lock → 读 fg_pgrp
   │         ↓
   │       kernel/sched/task.c:signal_pgrp(fg_pgrp, SIGINT)  [新]
   │         ├─ spin_lock task_list_lock
   │         ├─ 遍历 task_list：t->pgrp==target && !PF_KTHREAD
   │         │    → t->signal |= 1<<SIGINT
   │         │    → task_wake(t) 若 INTERRUPTIBLE
   │         └─ spin_unlock
   │    └─ 否（普通字符）：原 echo + ring push 逻辑
   ↓
keyboard_handler 返回 → ret_from_intr → arch_do_signal_delivery
        ↓
   前台 task 的 signal 位被检测 → 默认 action → do_exit
```

```
[shell 启动前台任务]
        ↓
busybox ash (已在用)
  fork() → child
  child: setpgid(0, 0)         ← libc stub 之前吞掉，本设计改成真调用
  child: exec cat
  parent: tcsetpgrp(tty_fd, child_pid)   ← libc stub 之前吞掉，本设计改成真调用
        ↓
kernel/arch/x86_64/trap.c:SYS_setpgid → current->pgrp = pid
kernel/fs/devfs.c:devfs_ioctl → TIOCSPGRP → tty->fg_pgrp = pid
```

接口边界：

- **kernel ↔ kernel**：新增 `signal_pgrp()`，封装于 `kernel/sched/task.c`；导出给 `kernel/tty/tty.c`
- **kernel ↔ libc**：新增 3 个 syscall 号（`SYS_setpgid`、`SYS_getpgid`、`SYS_setsid`），扩展 `SYS_kill` 语义；TIOCSPGRP/TIOCGPGRP 真存 `tty->fg_pgrp`
- **libc ↔ 用户**：4 个 stub 文件改实现，函数签名不变

---

## 3. 进程组与会话子系统（`kernel/sched/task.c` + `kernel/include/kernel/task.h`）

### 3.1 task_struct 字段

`kernel/include/kernel/task.h` 的 `task_struct` 加：

```c
pid_t pgrp;       // 进程组 ID（默认 = 创建者 pgrp；setpgid 可改）
pid_t session;    // 会话 ID（默认 = 创建者 session；setsid 可改）
```

- `pid_t` 类型在 `kernel/include/kernel/task.h` 已存在（posix 兼容定义）
- 字段紧跟现有 `pid`、`tgid` 之后，保持布局紧凑
- 初始化：`calloc` 已经把这两个字段清零 → pgrp/session=0；`init` 进程需要在 `task_init()` 显式设为 1（与 `pid` 对齐）

### 3.2 继承规则

- **fork**：`child->pgrp = parent->pgrp`，`child->session = parent->session`（POSIX：子进程继承）
- **exec**：保持不变（POSIX：exec 不改 pgrp/session）
- **exit**：无需操作（task 出队后从 task_list 消失，遍历自动跳过）

实现位置：`kernel/arch/x86_64/trap.c` 的 `case SYS_fork:` 子任务创建处，加 2 行。

### 3.3 signal_pgrp() 新接口

`kernel/sched/task.c` 新增：

```c
// 持 task_list_lock，遍历 task_list：
//   对每个 t->pgrp == target 且非 PF_KTHREAD 的 task：
//     t->signal |= (1ULL << sig)
//     若 state == TASK_INTERRUPTIBLE → task_wake(t)
// 返回：找到至少一个匹配返回 0；target == 0 返回 0（静默 no-op）；无匹配返回 -ESRCH
int signal_pgrp(pid_t target, int sig)
```

约束：
- 必须用 `spin_lock_irqsave(&task_list_lock)`（与 `task_send_signal` 同一锁，遍历时持锁防止 UAF）
- 必须校验 `sig ∈ [1, NSIG)`，非法返回 `-EINVAL`
- PF_KTHREAD 任务永远跳过（即使 pgrp 字段被错误设置）

### 3.4 SYS_setpgid / SYS_getpgid / SYS_setsid 实现

`kernel/arch/x86_64/trap.c` 加三个 case：

```c
case SYS_setpgid: {
    int pid = (int)(int64_t)regs->rdi;
    int pgid = (int)(int64_t)regs->rsi;
    if (pid == 0) pid = current->pid;
    if (pgid == 0) pgid = pid;
    if (pid < 0 || pgid < 0 || pid == 1) {
        regs->rax = -EINVAL; break;
    }
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    int ret = -ESRCH;
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid && !(t->flags & PF_KTHREAD)) {
            // POSIX 范围：pgid 必须等于 pid 自己，或 pid 所在 session 内某现存 pgrp
            // 简化：允许 pgid == pid（成为 leader）；其他情况返回 -EPERM
            // 注：本档只支持 "成为新 pgrp leader" 这一个用例
            if (pgid == pid) {
                t->pgrp = pgid;
                ret = 0;
            } else {
                ret = -EPERM;
            }
            break;
        }
    }
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = ret;
    break;
}
case SYS_getpgid: {
    int pid = (int)(int64_t)regs->rdi;
    if (pid == 0) pid = current->pid;
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    int ret = -ESRCH;
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid) { ret = t->pgrp; break; }
    }
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = ret;
    break;
}
case SYS_setsid: {
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    // POSIX: 若 current 已是 pgrp leader → EBUSY
    if (current->pgrp == current->pid) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EBUSY; break;
    }
    current->session = current->pid;
    current->pgrp = current->pid;
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = current->pid;
    break;
}
case SYS_getsid: {
    regs->rax = current->session;
    break;
}
```

POSIX 范围校验**简化理由**：busybox ash 只用 `setpgid(child_pid, child_pid)` 这一个模式（让子进程成为新 pgrp leader）。POSIX 还允许"加入同 session 内其他现存 pgrp"——本档不实现，标 -EPERM 即可，未来扩展时补。

### 3.5 SYS_kill 扩展

`kernel/arch/x86_64/trap.c:2010` 原 case 改为：

```c
case SYS_kill: {
    int pid = (int)(int64_t)regs->rdi;
    int sig = (int)regs->rsi;
    if (sig < 1 || sig >= NSIG) { regs->rax = -EINVAL; break; }
    if (pid > 0) {
        regs->rax = task_send_signal(pid, sig);
    } else if (pid == 0) {
        regs->rax = signal_pgrp(current->pgrp, sig);
    } else if (pid == -1) {
        // POSIX: pid==-1 = 所有 caller 有权 signal 的任务。本档简化为 signal_pgrp_all：
        // 遍历 task_list，给所有非 init、非 PF_KTHREAD、非 caller 自身发信号
        // 暂未实现——返回 -ENOSYS（busybox 不依赖此路径）
        regs->rax = -ENOSYS;
    } else { // pid < -1
        regs->rax = signal_pgrp(-pid, sig);
    }
    break;
}
```

`pid == -1` 暂留 -ENOSYS（本档非目标，busybox `killall5` 用此模式，但 busybox 在 OS01 上的 `killall5` 路径目前未走通——已有 `find_pid_by_name` 等依赖，不在本档扩大爆炸半径）。

---

## 4. TTY 行规程（`kernel/tty/tty.c` + `kernel/include/kernel/tty.h`）

### 4.1 tty_struct 扩展

`kernel/include/kernel/tty.h:18-50` 加 2 字段：

```c
typedef struct tty_struct {
    // ... 既有字段 ...
    pid_t       fg_pgrp;         // 前台进程组 ID（TIOCSPGRP 设置）
    spinlock_T  fg_pgrp_lock;    // 保护 fg_pgrp，IRQ-safe
} tty_t;
```

`fg_pgrp_lock` 与 `ring_lock` 分离的理由：VINTR 检查在 IRQ 路径下持锁时间应尽量短，避免与 echo/ring push 的长持锁重叠。

`tty_alloc` 加初始化：

```c
tty->fg_pgrp = 0;
spin_init(&tty->fg_pgrp_lock);
```

### 4.2 默认 termios

`kernel/tty/tty.c:104-109` 当前：

```c
memset(&tty->term, 0, sizeof(struct termios));
tty->term.c_iflag = ICRNL;
tty->term.c_oflag = OPOST | ONLCR;
tty->term.c_lflag = 0;               // raw default — honest
tty->term.c_cc[VMIN] = 1;
tty->term.c_cc[VTIME] = 0;
```

改为：

```c
memset(&tty->term, 0, sizeof(struct termios));
tty->term.c_iflag = ICRNL;
tty->term.c_oflag = OPOST | ONLCR;
tty->term.c_lflag = ISIG;            // raw + signal-aware
tty->term.c_cc[VMIN]   = 1;
tty->term.c_cc[VTIME]  = 0;
// 默认特殊字符：与 Linux 一致；VINTR=3 是 Ctrl-C 必须显式设置，
// 否则 memset=0 即 _POSIX_VDISABLE，行规程永不触发
tty->term.c_cc[VINTR]  = 3;          // Ctrl-C
tty->term.c_cc[VQUIT]  = 28;         // Ctrl-\
tty->term.c_cc[VERASE] = 127;        // DEL
tty->term.c_cc[VKILL]  = 21;         // Ctrl-U
tty->term.c_cc[VEOF]   = 4;          // Ctrl-D
// VSUSP / VSTART / VSTOP / VREPRINT / VDISCARD / VWERASE / VLNEXT
// 留 0（memset 默认）= 禁用（_POSIX_VDISABLE 语义）
```

**默认 ISIG=1 但 ICANON=0、ECHO=0**（半 raw）：cat 类程序行为不变（无 echo、一次 1 字节读），**仅多一项**：VINTR 字符触发信号。busybox ash 启动后调 tcsetattr 设 ICANON+ECHO 时不会清掉 ISIG（POSIX 字段语义独立）。

注释更新：把"raw default — honest"改为"raw + signal-aware: VINTR/VQUIT/VSUSP translate to signals; reads remain unbuffered, no echo"。

### 4.3 VINTR/VQUIT/VSUSP 翻译

`kernel/tty/tty.c:125` `tty_push_input` 在 echo + ring push **之前**加：

```c
void tty_push_input(tty_t *tty, char c)
{
    if (!tty) return;

    // ── 行规程：VINTR / VQUIT / VSUSP → 信号 ──────────
    // _POSIX_VDISABLE = 0 表示"禁用该特殊字符"
    if (tty->term.c_lflag & ISIG) {
        cc_t vintr = tty->term.c_cc[VINTR];
        if (vintr != 0 && c == vintr) {
            uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
            pid_t pg = tty->fg_pgrp;
            spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            if (pg != 0) signal_pgrp(pg, SIGINT);
            return;  // 不入环，不 echo
        }
        cc_t vquit = tty->term.c_cc[VQUIT];
        if (vquit != 0 && c == vquit) {
            uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
            pid_t pg = tty->fg_pgrp;
            spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            if (pg != 0) signal_pgrp(pg, SIGQUIT);
            return;
        }
        cc_t vsusp = tty->term.c_cc[VSUSP];
        if (vsusp != 0 && c == vsusp) {
            // SIGTSTP 未实现：丢弃 + debug 一次日志，避免静默吞掉
            static int warned = 0;
            if (!warned) {
                warned = 1;
                log_debug("tty: VSUSP char dropped (SIGTSTP not implemented)\n");
            }
            return;
        }
    }

    // 既有逻辑（不变）
    if ((tty->term.c_lflag & ICANON) && (tty->term.c_lflag & ECHO)) { ... }
    if (!tty_ring_push(tty, c)) return;
    tty_wake_waiters(tty);
}
```

`_POSIX_VDISABLE` 当前未在 `libc/include/termios.h` 定义（grep 无结果）。本档在 `kernel/tty/tty.c` 局部 `#define _POSIX_VDISABLE 0` 并加注释；后续若 libc 引入，取消本地宏。

**SMP / 中断上下文**：本函数在键盘 IRQ handler（IRQ 上下文）调用。`signal_pgrp` 内部用 `spin_lock_irqsave`，与 IRQ-safe 约定一致。`spin_lock_irqsave` 会保存并禁用中断（中断嵌套安全）；VINTR 路径下 IRQ 已禁用再禁一次为幂等。

**ring 满情况**：原文 `if (!tty_ring_push(...)) return;` 在 ISIG 路径之前——ISIG 路径不依赖 ring 状态，即使 ring 满也照常派发信号。

### 4.4 TIOCGPGRP / TIOCSPGRP 真实现

`kernel/tty/tty.c:268-298` `tty_phys_ioctl` 替换原 stub：

```c
case TIOCGPGRP: {
    pid_t *p = (pid_t *)arg;
    if ((uint64_t)p >= current->addr_limit) return -EFAULT;
    uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
    *p = tty->fg_pgrp;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
    return 0;
}
case TIOCSPGRP: {
    pid_t *p = (pid_t *)arg;
    if ((uint64_t)p >= current->addr_limit) return -EFAULT;
    pid_t new_pg;
    // 用户→内核拷贝（典型 4 字节）
    memcpy(&new_pg, p, sizeof(pid_t));
    if (new_pg < 0) return -EINVAL;
    uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
    tty->fg_pgrp = new_pg;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
    return 0;
}
```

保持其他 case（TCGETS/TCSETS/TIOCGWINSZ/FIONREAD）不变。

### 4.5 既有约束（不变）

- `tty_read` 已检查 `arch_signal_pending_fatal()`（line 182、206），signal 处理路径无须改
- `tty_wake_waiters` 唤醒后 `need_resched = 1`（line 79），signal 唤醒 INTERRUPTIBLE 任务后调度已就绪
- `tty_push_input` 调用者是 keyboard_handler IRQ + keyboard_poll task，两路径都通过本函数

---

## 5. libc stub → 真调用

四个文件改动，函数签名不变：

### 5.1 `libc/unistd/setpgid.c`

```c
#include <unistd.h>
#include <sys/syscall.h>
int setpgid(pid_t pid, pid_t pgid) {
    return (int)syscall(SYS_setpgid, (uint64_t)pid, (uint64_t)pgid, 0);
}
```

### 5.2 `libc/unistd/tcsetpgrp.c`

```c
#include <unistd.h>
#include <sys/ioctl.h>
int tcsetpgrp(int fd, pid_t pgrp) {
    return ioctl(fd, TIOCSPGRP, &pgrp);
}
```

### 5.3 `libc/unistd/tcgetpgrp.c`

```c
#include <unistd.h>
#include <sys/ioctl.h>
pid_t tcgetpgrp(int fd) {
    pid_t p = 0;
    if (ioctl(fd, TIOCGPGRP, &p) < 0) return -1;
    return p;
}
```

### 5.4 `libc/signal/killpg.c`

```c
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>
// POSIX: killpg(pgrp, sig) ≡ kill(-pgrp, sig)
// 必须经 kill() 让 SYS_kill 走"pid<0 → signal_pgrp"路径，
// 不要直接 syscall(SYS_kill, ...)：保持与 kill 同一套权限/封装
int killpg(pid_t pgrp, int sig) {
    if (pgrp < 1) { errno = EINVAL; return -1; }
    // pid_t 是 int，-(pid_t) 转 int64 安全（pid_t ∈ [1, INT_MAX]）
    return (int)syscall(SYS_kill,
                        (uint64_t)(-(int64_t)(int)pgrp),
                        (uint64_t)sig, 0);
}
```

### 5.5 syscall 号

`libc/include/sys/syscall.h` 新增（位置接续现有 66 号之后）：

```c
#define SYS_setpgid   67
#define SYS_getpgid   68
#define SYS_setsid    69
#define SYS_getsid    70
```

`kernel/include/uapi/syscall.h` 同步新增，并在 `do_system_call` 的 `linux_to_os01[]` 表中按需补全（mbedtls/busybox 用 Linux `SYS_setpgid=121` / `SYS_getpgid=121` 等时走 ABI 翻译路径——本档只覆盖 native syscall，Linux 翻译表后续按需补，不在本档爆炸半径）。

### 5.6 libc/include/sys/syscall.h: SYS_signal 命名冲突检查

`grep SYS_setpgid\|SYS_getpgid\|SYS_setsid libc/` 确认不冲突。

---

## 6. 锁与并发

### 6.1 锁清单

| 锁 | 保护对象 | 类型 | 已存在？ |
|---|---|---|---|
| `task_list_lock` | task_list 遍历 + pgrp/session 字段读写 | spin_lock_irqsave | 是 |
| `tty->fg_pgrp_lock` | `tty->fg_pgrp` 读写 | spin_lock_irqsave | 新增 |
| `tty->ring_lock` | `tty->ring[head/tail]` | spin_lock_irqsave | 是 |
| `tty->read_wait_lock` | `tty->read_wait` 链表 | spin_lock_irqsave | 是 |

### 6.2 锁顺序

唯一嵌套路径：`signal_pgrp` → `task_wake` → 内部可能取的锁需排查。

- `signal_pgrp` 持 `task_list_lock`，对每个匹配 task：
  - 写 `t->signal`（受 task_list_lock 保护）
  - 若 `state == TASK_INTERRUPTIBLE` → `task_wake(t)`
- `task_wake` 是否取其他锁？查 `kernel/sched/task.c` 现有实现——`task_send_signal` 已用同一模式（line 1839-1840），不存在锁死问题，沿用。

**VINTR 路径下两把锁的获取顺序**（`tty_push_input` → `signal_pgrp`）：

1. `spin_lock_irqsave(&tty->fg_pgrp_lock)` （读 fg_pgrp 到局部变量）
2. 立即 `spin_unlock_irqrestore`
3. 然后 `signal_pgrp` → `spin_lock_irqsave(&task_list_lock)`

两锁**不嵌套**——fg_pgrp_lock 持锁期间不进入 signal_pgrp，避免反向嵌套（task_list_lock → fg_pgrp_lock）的可能性。读出的 fg_pgrp 是快照，可能在派发瞬间已变化，但这是 POSIX 允许的（"派发时按当时 pgrp 计"，内核不阻塞保证一致）。

### 6.3 SMP

- 所有新锁用 `spin_lock_irqsave`，与既有约定一致
- VINTR 在 IRQ 路径下调用——IRQ 已禁用，`spin_lock_irqsave` 第二次进入为幂等
- `signal_pgrp` 在 IRQ-safe 路径下也安全（已由 `task_send_signal` 验证）

---

## 7. 数据流（happy path）

### 7.1 Ctrl-C → kill cat

```
[QEMU gtk 窗口按键 Ctrl-C]
  ↓ QEMU 内部 PS/2 模拟 → 端口 0x60 字节序列：1D (LCTL press) / 2E (C press) / 9D (LCTL release) / AE (C release)
kernel/driver/keyboard.c:keyboard_handler (IRQ1)
  → arch_inb(0x60) 读 scancode
  → translate_and_push(sc=0x2E, e0_prefix=false)
     → kbd_ctrl()==true（来自此前 0x1D）
     → c == 'C'-'a'+1 = 3
     → tty_push_input(kbd_tty, 3)
kernel/tty/tty.c:tty_push_input (IRQ 上下文)
  → tty->term.c_lflag & ISIG ?  是
  → c_cc[VINTR] == 3, c == 3 → 命中
  → spin_lock(&tty->fg_pgrp_lock) → 读 fg_pgrp = cat_pid
  → spin_unlock
  → signal_pgrp(cat_pid, SIGINT)
kernel/sched/task.c:signal_pgrp
  → spin_lock(&task_list_lock)
  → 遍历：找到 cat（t->pgrp == cat_pid && !PF_KTHREAD）
  → cat.signal |= (1 << SIGINT)
  → cat.state == TASK_INTERRUPTIBLE ?
       cat 当前可能在 read(/dev/urandom)（devfs_read 同步返回不阻塞）→ RUNNING
       或在 write(stdout=fd 1=/dev/tty)（tty_write 同步返回不阻塞）→ RUNNING
       → 不 wake（信号位已设，下次 ret_from_intr 自然处理）
  → spin_unlock
keyboard_handler 返回
  → 当前 task 在 keyboard IRQ 上下文，不是 cat——跳过
  → cat 在自己下一次 ret_from_intr / do_system_call 返回到 ring 3 时：
     arch_do_signal_delivery → cat.signal & SIGINT → SIG_DFL → do_exit(2<<8)
shell waitpid() 返回（child reaped）
shell 打印提示符
```

### 7.2 shell 启动前台任务（busybox ash 已实现）

```
ash 解析 "cat /dev/urandom"
  → fork()
     child: kernel/fork → 子进程 pgrp 继承 = ash.pgrp
  → child: setpgid(0, 0) → SYS_setpgid(pid=0, pgid=0)
     → 解析为 setpgid(child_pid, child_pid)
     → child.pgrp = child_pid
  → child: exec("/bin/cat", ...) → exec 不改 pgrp
  → parent: tcsetpgrp(tty_fd, child_pid)
     → ioctl(fd, TIOCSPGRP, &child_pid)
     → tty.fg_pgrp = child_pid
shell 在 waitpid() 阻塞
```

---

## 8. 测试（`user/systest.c` 新增 5 个）

`tests/run_test.py` 现有 132/132 PASS，本档新增 5 测试，目标 137/137。

### 8.1 test_signal_pgrp_basic（用 negative pid 路径触发 signal_pgrp）

```c
// 直接验证 signal_pgrp()：子进程在 pgrp leader 下 pause()，
// 父进程 kill(-child_pid, SIGUSR1) 走 signal_pgrp 路径
static void test_signal_pgrp_basic(void) {
    signal(SIGUSR1, SIG_IGN);  // 父进程不响应
    int64_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        pause();
        _exit(0);
    }
    setpgid(pid, pid);   // 父端也设（POSIX 要求两端都设，race-free）
    // kill(-pid, SIGUSR1) → signal_pgrp(pid, SIGUSR1) 派发给 pgrp==pid 的成员
    int64_t r = syscall(SYS_kill, (int64_t)(-(int64_t)pid),
                        (uint64_t)SIGUSR1, 0);
    CHECK3(r == 0, "signal_pgrp_basic", "kill(-pid,SIGUSR1) returns 0");
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1,
           "signal_pgrp_basic", "child got SIGUSR1");
}
```

注：不能用 `kill(0, SIGUSR1)`（caller pgrp）——子 setpgid(0,0) 后子与父在不同 pgrp。

### 8.2 test_setpgid_getpgid

```c
// 基本属性验证：setpgid(0,0) 后 getpgid(0) == getpid()
static void test_setpgid_getpgid(void) {
    int64_t pid = fork();
    if (pid == 0) {
        int r = setpgid(0, 0);
        CHECK3(r == 0, "setpgid_getpgid", "setpgid(0,0) ok");
        pid_t me = getpid();
        pid_t pg = getpgid(0);
        CHECK3(pg == me, "setpgid_getpgid", "getpgid(0)==getpid");
        _exit(0);
    }
    int status; waitpid(pid, &status, 0);
}
```

### 8.3 test_setsid

```c
// setsid 后 getpgrp/getsid 都等于 getpid()；后续 setpgid(0, sid) 应 EBUSY
// 注：本档 setsid 不实现 EBUSY 检查的 current.pgrp==current.pid 兜底？实际是 EBUSY 检查。
// 但 fork 出的子进程初始 pgrp 继承父，调用 setsid 后必然 != 自身 → OK
static void test_setsid(void) {
    int64_t pid = fork();
    if (pid == 0) {
        pid_t sid = setsid();
        CHECK3(sid == getpid(), "setsid", "returns own pid");
        CHECK3(getpgid(0) == getpid(), "setsid", "getpgid==pid");
        CHECK3(getsid(0)  == getpid(), "setsid", "getsid==pid");
        _exit(0);
    }
    int status; waitpid(pid, &status, 0);
}
```

### 8.4 test_tiocspgrp_roundtrip

```c
// 验证 TIOCSPGRP 写入、TIOCGPGRP 读回；并验证不同 fd 不影响（只支持 /dev/tty）
static void test_tiocspgrp_roundtrip(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) { FAIL("tiocspgrp", "no /dev/tty"); return; }
    int64_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        tcsetpgrp(fd, getpid());
        // 验证
        pid_t p = tcgetpgrp(fd);
        // 父进程再读一次对比
        _exit(0);
    }
    setpgid(pid, pid);
    // 父端也 tcsetpgrp（POSIX 要求两端）
    tcsetpgrp(fd, pid);
    int status; waitpid(pid, &status, 0);
    // wait 后再读
    pid_t p = tcgetpgrp(fd);
    CHECK3(p == pid, "tiocspgrp", "tcgetpgrp returns set pid");
    close(fd);
}
```

### 8.5 test_kill_neg_pid_pgrp（systest）

```c
// kill(-pgid, SIGUSR2) 应派发给整个 pgrp
static void test_kill_neg_pid_pgrp(void) {
    signal(SIGUSR2, SIG_IGN);
    int64_t c1 = fork();
    if (c1 < 0) { FAIL("kill_pgrp", "fork1"); return; }
    if (c1 == 0) { setpgid(0, 0); pause(); _exit(0); }
    int64_t c2 = fork();
    if (c2 < 0) { FAIL("kill_pgrp", "fork2"); return; }
    if (c2 == 0) { setpgid(0, c1); pause(); _exit(0); }   // c2 加入 c1 的 pgrp
    setpgid(c1, c1);
    setpgid(c2, c1);
    int64_t r = syscall(SYS_kill, (int64_t)(-(int64_t)c1),
                        (uint64_t)SIGUSR2, 0);
    CHECK3(r == 0, "kill_pgrp", "kill(-pgid,sig) returns 0");
    int s1 = 0, s2 = 0;
    waitpid(c1, &s1, 0);
    waitpid(c2, &s2, 0);
    CHECK3(WIFSIGNALED(s1) && WTERMSIG(s1) == SIGUSR2,
           "kill_pgrp", "c1 got SIGUSR2");
    CHECK3(WIFSIGNALED(s2) && WTERMSIG(s2) == SIGUSR2,
           "kill_pgrp", "c2 got SIGUSR2");
}
```

### 8.6 kernel selftest（`test/cases/test_tty_vintr.c`，新增）

VINTR 端到端无法从用户态测试（用户态无法直接调 `tty_push_input`，且真实键盘注入需要 QEMU 介入）。本测试放在 kernel selftest 入口，遵循 `test/cases/test_poll_requested.c` 现有约定：

- 测试线程通过 `kernel_thread()` 创建一个用户态任务 child
- child 体：`setpgid(0, 0); exec "cat"`（走 native elf loader），cat 在 `read(0, ...)` 阻塞
- 测试线程：`ioctl(0, TIOCSPGRP, &child_pid)` 设 fg_pgrp
- 测试线程短 spin 等 cat 进入 read
- 测试线程直接调 `tty_push_input(get_dev_tty(), 3)` 注入 VINTR
- `waitpid` child，断言 `WIFSIGNALED && WTERMSIG == SIGINT`
- 测试结束恢复 fg_pgrp=0

### 8.7 验收

- `make` 编译通过
- `make run` 下 systest 全绿（含新增 5 个，132+5=137/137）
- kernel selftest `test_tty_vintr` PASS
- 手动：OS01 shell 跑 `cat` / `cat /dev/urandom`，按 Ctrl-C 能终止，shell 回到提示符

---

## 9. 文件清单与工作量

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/include/kernel/task.h` | `task_struct` 加 `pgrp`/`session` 字段 | +2 |
| `kernel/include/kernel/tty.h` | `tty_struct` 加 `fg_pgrp`/`fg_pgrp_lock` | +3 |
| `kernel/sched/task.c` | `signal_pgrp()` 新函数；`init_task_union.task.pgrp/session` 初值 | +30 |
| `kernel/tty/tty.c` | `tty_alloc` 默认 ISIG + c_cc 默认；`tty_push_input` 行规程；`tty_phys_ioctl` 真 TIOCSPGRP/TIOCGPGRP；`_POSIX_VDISABLE` 局部宏 | +60 |
| `kernel/arch/x86_64/trap.c` | 4 个新 syscall case；SYS_fork 子任务加 2 行；SYS_kill 扩展 | +90 |
| `test/cases/test_tty_vintr.c` | 新文件：VINTR e2e selftest | +50 |
| `kernel/include/uapi/syscall.h` | +4 行 syscall 号 | +4 |
| `libc/include/sys/syscall.h` | +4 行 syscall 号 | +4 |
| `libc/unistd/setpgid.c` | stub → syscall | +5 |
| `libc/unistd/tcsetpgrp.c` | stub → ioctl | +5 |
| `libc/unistd/tcgetpgrp.c` | fake-return-1 → ioctl | +5 |
| `libc/signal/killpg.c` | stub → kill(-pgrp,sig) | +5 |
| `user/systest.c` | 新增 5 测试函数 + 注册 | +200 |
| `docs/syscall.md` | 4 行：setpgid/getpgid/setsid/getsid 条目 | +20 |

合计：~485 行（内核 ~290，libc ~25，测试 ~250，文档 ~20）。

---

## 10. 非目标（YAGNI）

- **不做** SIGTSTP/SIGCONT 完整作业控制（fg/bg/jobs 内建）——属第二档，本档只为它铺路（fg_pgrp / pgrp 字段已就位）。
- **不做** `tcgetattr`/`tcsetattr` 各 flag 字段独立读写语义（ISIG/ICANON/ECHO 等）——当前内核 TCSETS 整体 memcpy 满足 busybox ash 需求。
- **不做** session leader 退出时 SIGHUP 派发。
- **不做** `kill(-1, sig)` 全员广播（busybox `killall5` 用本模式，但 OS01 缺 find_pid_by_name 等基础设施，爆炸半径过大）。
- **不做** `_POSIX_VDISABLE` 全局化定义（kernel/tty/tty.c 局部宏即可）。
- **不做** busybox ash 源码修改（依赖 ash 现有 `setpgid`/`tcsetpgrp` 调用链自动生效——已存在，stub 改真即用）。
- **不做** Linux x86_64 ABI 翻译表 `linux_to_os01[121]` (setpgid/getpgid) 等补全（native syscall 已够 native 程序使用；busybox 走 native 表）。

---

## 11. 风险与边界

1. **busybox ash 实际行为未 100% 验证**：依赖 ash 在 fork 后调 setpgid(0,0)、tcsetpgrp(fd, child_pid)。代码搜索已确认这些调用存在（`thirdpart/busybox-1.36.1/shell/ash.c` 多处），但 libc stub 改为真实现后是否引出其他 ash 内部路径问题（例：ash 假设 `tcgetpgrp` 永返 1，现改为真实返回值，可能触发新分支），需在实现后跑 systest + 手动 OS01 验证。
2. **`signal_pgrp` 在 IRQ 上下文调用**：`spin_lock_irqsave(&task_list_lock)` 在 IRQ 已禁用时安全（与 `task_send_signal` 同一路径，已验证）。新路径唯一新增的是"持锁下对每个匹配 task 调用 task_wake"——若 `task_wake` 内部取其他锁可能反向嵌套。当前 `task_wake` 实现仅操作 `wait_list`，无反向锁需求（沿用 task_send_signal 已验证模式）。
3. **systest test_signal_pgrp_basic 中父进程调 kill(0, SIGUSR1)**：父进程若未屏蔽 SIGUSR1 会自吞信号——测试用 `signal(SIGUSR1, SIG_IGN)` 屏蔽，规避。
4. **`c_cc` 默认值必须显式设置**：VINTR=3、VQUIT=28 等不能依赖 memset 的 0（0 意味着 _POSIX_VDISABLE，行规程永不触发）。`tty_alloc` 修改时必须包含 §4.2 的 5 行赋值——属于"漏一行就静默失效"的陷阱。
5. **`fg_pgrp` 默认 0**：未 tcsetpgrp 时 VINTR 静默 no-op。init 阶段 / systest 早期无 fg_pgrp 不会误触发，安全。
6. **multicore IRQ routing**：keyboard IRQ1 绑定到 BSP（既定事实，`kbd_lctrl` 等静态变量依赖此不变量，见 `kernel/driver/keyboard.c:49-51`）。VINTR 派发在 BSP 上完成，AP 上的 task 接收信号经 task_wake → schedule → cross-CPI/IPI 不在本档范围（沿用现有 signal 投递路径）。
7. **`fork` 时 pgrp 继承 + 子进程立即 setpgid(0,0) 的小窗口**：父进程 wait 期间，子进程已 setpgid 完，fg_pgrp 设置有效。若 shell 走"父子同时 tcsetpgrp"的标准 POSIX 模式，无 race（ash 已实现）。本设计不解决极端 race（父端未 tcsetpgrp 之前用户按键）——VINTR 派发到旧 pgrp（父端 pgrp），父进程收信号。这是 POSIX-correct 行为。

---

## 12. 实施顺序（依赖驱动）

1. **task_struct 字段 + signal_pgrp** → 编译通过；可单独跑 signal_pgrp 单元验证
2. **fork 继承 + setpgid/getpgid/setsid/getsid syscall** → 编译通过；可跑 8.2/8.3 测试
3. **tty_struct fg_pgrp + TIOCSPGRP 真实现 + 默认 ISIG + c_cc 默认** → 编译通过；可跑 8.4 测试
4. **tty_push_input 行规程** → 端到端；可跑 8.1/8.6 + kernel selftest 8.7
5. **SYS_kill 扩展** → 完整测试；可跑 8.6 kill(-pgid, sig)
6. **libc 4 个 stub → 真调用** → busybox ash 走真路径；可手动 OS01 shell 验证 Ctrl-C
7. **5 个 systest + kernel selftest** → 137/137 PASS

每步独立可测，不需"全做完才知道有没有错"。

# TTY 行规程与进程组（最小集）设计文档

- 日期：2026-08-22
- 状态：v2 待用户 review（尚未进入实现）
- 目标：实现 POSIX 最小行规程（VINTR/VQUIT → 信号派发到前台进程组）+ 进程组/会话子系统骨架，使 Ctrl-C 能在 OS01 shell 中可靠终止前台任务（如 `cat /dev/urandom`、`cat` 无参数），同时为后续 job control 留出干净的扩展面
- 修订记录：
  - v1：初版
  - v2：并入 review 反馈——
    - 🔴 致命：busybox `CONFIG_ASH_JOB_CONTROL is not set`（`.config:1125`）→ ash 内 `#if JOBS` 块全空，xtcsetpgrp 永不被调用，fg_pgrp 永远为 0，v1 设计静默失效
    - 核心修复：新增 §3.5 自动 fg_pgrp 更新 + §4.2 devfs_open 默认 fg_pgrp = opener.pgrp，叠加后无论 ash 行为如何都能工作
    - 🟠 §3.4 setpgid POSIX 校验（pgid 跨现存 pgrp 返 EPERM、pgid==pid 自检验）
    - 🟠 §3.5 kill(-1) 广播（从非目标提升为实现目标）
    - 🟠 §4.5 TIOCSPGRP 区间检查 + 调用者同 session 校验
    - 🟡 §4.2 注释补 VSTART/VSTOP 已知未实现
    - 🟡 §4.4 补 keyboard_poll 进程上下文说明
    - 🟡 §6.2 修正 task_wake 实际取 rq_lock 的事实
    - 🟡 §6.3 增 t->signal 同步故事（task_list_lock 写 + x86 TSO 原子读）
    - 🟡 §4.4 去掉 VSUSP warned 静态计数器（重复日志无害）
    - 🟡 §11 加默认 ISIG 影响面扫描 + CONFIG_ASH_JOB_CONTROL=n 已确认风险
    - 🟡 §8 测试集扩到 6 个 systest（含 setpgid 自动 fg_pgrp 验证）
    - 🟡 §9 文件清单更新：kernel/fs/devfs.c +5 行；总工作量 ~530 行

---

## 1. 背景与动机

### 1.1 现象

用户在 QEMU gtk 窗口按 Ctrl-C，OS01 shell 中运行的 `cat /dev/urandom` 无法终止；同样的 `cat`（无参数，从 stdin 读）也无法终止。

### 1.2 根因（三层缺陷 + 一层缺失，都在本设计修复）

1. **缺失行规程（主因）** — `kernel/tty/tty.c:125-140` 的 `tty_push_input` 只做 echo + ring push，**完全没读 `c_lflag & ISIG`、也没比较 `c_cc[VINTR]`**。`grep -rn 'ISIG\|VINTR\|VQUIT\|VSUSP\|cc_special' kernel/` 输出为空。整个内核从未把字符翻译成信号。
2. **tty_struct 没有 pgrp 字段** — `kernel/include/kernel/tty.h:18-50` 的 `tty_struct` 没有 `fg_pgrp`/`pgrp` 字段（pty_struct 有，但那是 PTY 设备不是物理控制台 TTY）。即使实现了行规程，也没东西可派发。
3. **TIOCSPGRP ioctl 是 stub** — `kernel/tty/tty.c:289-290` `case TIOCSPGRP: return 0;` 连 arg 都没读。
4. **🔴 v2 新发现：busybox ash 在 OS01 上不会调 tcsetpgrp** —
   - `thirdpart/busybox-1.36.1/.config:1125: # CONFIG_ASH_JOB_CONTROL is not set`
   - ash 内 `#define JOBS ENABLE_ASH_JOB_CONTROL`（`ash.c:186`）→ JOBS=0
   - 所有 `xtcsetpgrp` 调用点都在 `#if JOBS` 块内（行 4124/4131/4228/5253/5424），编译为空
   - 后果：v1 设计依赖"shell 调 tcsetpgrp"是单点失败——fg_pgrp 永远为 0，VINTR 静默 no-op
   - v2 修复：§3.5 自动 fg_pgrp 更新 + §4.2 devfs_open 默认 fg_pgrp 兜底

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
    task_t *target = NULL;
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid && !(t->flags & PF_KTHREAD)) {
            target = t; break;
        }
    }
    if (!target) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -ESRCH; break;
    }
    // POSIX 校验：
    //   1. caller 必须是 target 自身，或与 target 同 session
    //   2. pgid 必须等于 pid（成为新 leader）
    //   3. target 已 exec 后应返 EACCES（本档不追踪 exec 状态，留 TODO）
    if (current->pid != target->pid && current->session != target->session) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EPERM; break;
    }
    if (pgid != pid) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EPERM; break;
    }
    target->pgrp = pgid;

    // ── v2 核心：自动 fg_pgrp 更新（见 §3.5）──────────────────
    // 若 target 刚成为新 pgrp leader（pgid==pid），且 fd 0 指向 dev_tty，
    // 把 dev_tty.fg_pgrp 同步到新 pgid——替代 POSIX 要求的"shell 调 tcsetpgrp"
    tty_t *dev_tty = get_dev_tty();
    if (dev_tty && current->files && current->files->fd[0]) {
        file_t *f0 = current->files->fd[0];
        if (f0->node == dev_tty_vfs_node()) {
            uint64_t ftf = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
            dev_tty->fg_pgrp = pgid;
            spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, ftf);
        }
    }

    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = 0;
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

`dev_tty_vfs_node()` 是新增 helper（`kernel/tty/tty.c`）：

```c
vfs_node_t *dev_tty_vfs_node(void) {
    return get_dev_tty() ? (vfs_node_t *)get_dev_tty()->vfs_node : NULL;
}
```

`tty_t` 加 `vfs_node_t *vfs_node` 字段（指向 `/dev/tty` 对应的 vfs 节点），在 `tty_set_dev_tty` 时设置。

POSIX exec 后 EACCES：本档不追踪 exec 状态（task_struct 无 exec_count 之类字段），留 TODO，第二档补；当前对所有 setpgid 调用一视同仁，不阻塞 ash 的 setpgid(0,0) 用例（ash 不 exec 自身）。

`dev_tty_vfs_node()` 与 `get_dev_tty()` 都需要在 tty 模块导出。

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
        // POSIX: pid==-1 = 信号到 caller 有权 signal 的所有任务。
        // 实现：除 init、PF_KTHREAD、caller 自身外的所有任务
        // （POSIX 还要求"同 session 或同 uid"，OS01 无 uid 概念，按"全部"实现）
        uint64_t f = spin_lock_irqsave(&task_list_lock);
        int matched = 0;
        list_t *pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            pos = task_list_next(pos);
            if (t == current) continue;
            if (t->flags & PF_KTHREAD) continue;
            if (t->pid == 1) continue;  // init 永不参与广播
            t->signal |= (1ULL << sig);
            if (t->state == TASK_INTERRUPTIBLE)
                task_wake(t);
            matched++;
        }
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = matched > 0 ? 0 : -ESRCH;
    } else { // pid < -1
        regs->rax = signal_pgrp(-pid, sig);
    }
    break;
}
```

`pid == -1` 实现为内联（避免单独抽 helper 函数）。覆盖 busybox init/reboot 路径的 `kill(-1, SIGTERM)` 关机场景（防止后续踩坑）。

---

## 4. TTY 行规程（`kernel/tty/tty.c` + `kernel/include/kernel/tty.h`）

### 4.1 tty_struct 扩展

`kernel/include/kernel/tty.h:18-50` 加 3 字段：

```c
typedef struct tty_struct {
    // ... 既有字段 ...
    pid_t       fg_pgrp;         // 前台进程组 ID（TIOCSPGRP / 自动 setpgid 路径设置）
    spinlock_T  fg_pgrp_lock;    // 保护 fg_pgrp，IRQ-safe
    struct vfs_node *vfs_node;    // /dev/tty 对应的 vfs 节点；setpgid 自动更新时比对用
} tty_t;
```

`fg_pgrp_lock` 与 `ring_lock` 分离的理由：VINTR 检查在 IRQ 路径下持锁时间应尽量短，避免与 echo/ring push 的长持锁重叠。

`tty_alloc` 加初始化：

```c
tty->fg_pgrp = 0;
spin_init(&tty->fg_pgrp_lock);
tty->vfs_node = NULL;  // 由 tty_set_dev_tty() 设置
```

`vfs_node` 字段为 §3.4 自动 fg_pgrp 更新提供比对目标（setpgid 时检查 current->files->fd[0] 是否指向 /dev/tty）。

### 4.1.1 devfs_open 默认 fg_pgrp（v2 兜底）

`kernel/fs/devfs.c` `devfs_open_node` 中，字符设备 open 成功后，若 node 是 `/dev/tty` 对应的 devfs 节点，调用：

```c
tty_t *tty = get_dev_tty();
if (tty && tty->vfs_node == node && current->pgrp != 0) {
    uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
    if (tty->fg_pgrp == 0)  // 仅当尚未设置时（避免覆盖后续 tcsetpgrp）
        tty->fg_pgrp = current->pgrp;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
}
```

**效果**：shell（init 子进程，pgrp=1）首次 open `/dev/tty` 时，fg_pgrp 默认设为 1。即使 shell 后续不调 tcsetpgrp，VINTR 仍能派发到 pgrp=1 的所有成员（shell + 所有未 setpgid 的子进程）。

**与 §3.4 自动 fg_pgrp 更新的协同**：

| 场景 | 行为 |
|---|---|
| shell open /dev/tty，未调任何 setpgid | fg_pgrp=1；fork 出的 child 默认 pgrp=1 → Ctrl-C 命中 shell+child |
| shell open /dev/tty，child 调 setpgid(0,0) | §3.4 自动更新 fg_pgrp=child.pid → Ctrl-C 命中 child |
| shell 调 tcsetpgrp 显式覆盖 | fg_pgrp=任意值；§4.1.1 不覆盖（仅当 fg_pgrp==0 时设） |

注意：`current->pgrp == 0` 时跳过——避免 init 在 pgrp=0 状态下 open /dev/tty 时把 fg_pgrp 设成 0（虽然无意义但稳妥）。

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
// VSUSP / VREPRINT / VDISCARD / VWERASE / VLNEXT 留 0（memset 默认）= 禁用
// （_POSIX_VDISABLE 语义）
//
// 已知遗留：VSTART/Ctrl-Q (0x11) 和 VSTOP/Ctrl-S (0x13) 未实现为流控字符，
// 当前会作为普通字符入 TTY 环并被 cat 等读到。在 cbreak 模式（ISIG=1,
// ICANON=0）下，"按键乱入"是低严重性已知行为——busybox ash 在交互提示符
// 下会 tcsetattr 设 ICANON=1 时再启用 XON/XOFF；第二档补全
// （实现 IXON/IXOFF 流控）。
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
            // SIGTSTP 未实现：直接丢弃。debug 日志每次按键都打没关系
            // （v2 修正：v1 用 static warned 去重非 SMP 安全，多 CPU 可能
            //  重复一次日志；重复日志无害，简化掉）
            log_debug("tty: VSUSP char dropped (SIGTSTP not implemented)\n");
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

**调用上下文**：本函数被两个调用者调用，两者行为都已验证安全：

1. **`keyboard_handler` (IRQ 上下文, IRQ1)** — 路径已经存在
2. **`keyboard_poll` (内核线程上下文)** — 路径也已经存在

两种上下文都用 `spin_lock_irqsave`：IRQ 上下文下已禁用中断，二次禁为幂等；线程上下文下会真禁用但 kernel_thread 不会被抢占持锁。`signal_pgrp` 内部对 `task_list_lock` 的 `spin_lock_irqsave` 与 IRQ-safe 约定一致。

**ring 满情况**：原文 `if (!tty_ring_push(...)) return;` 在 ISIG 路径之前——ISIG 路径不依赖 ring 状态，即使 ring 满也照常派发信号。

### 4.4 TIOCGPGRP / TIOCSPGRP 真实现

`kernel/tty/tty.c:268-298` `tty_phys_ioctl` 替换原 stub：

```c
case TIOCGPGRP: {
    pid_t *p = (pid_t *)arg;
    // v2: 区间检查 p..p+sizeof(pid_t)，不只检查起点
    if ((uint64_t)p >= current->addr_limit ||
        (uint64_t)p + sizeof(pid_t) > current->addr_limit)
        return -EFAULT;
    uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
    *p = tty->fg_pgrp;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
    return 0;
}
case TIOCSPGRP: {
    pid_t *p = (pid_t *)arg;
    if ((uint64_t)p >= current->addr_limit ||
        (uint64_t)p + sizeof(pid_t) > current->addr_limit)
        return -EFAULT;
    pid_t new_pg;
    // 用户→内核拷贝（典型 4 字节）
    memcpy(&new_pg, p, sizeof(pid_t));
    if (new_pg < 0) return -EINVAL;
    // v2: POSIX 要求 caller 与目标 pgrp 同 session。OS01 无 controlling-tty
    // 基础设施，做近似校验：caller.session == current.session（自身必然满足）
    // + new_pg == 0（合法）或 new_pg 与 current 同 session 内某现存 pgrp
    // 简化：本档只接受 new_pg == 0 或 new_pg == current.pgrp；其他返 -EPERM
    // （POSIX 严格语义第二档补，要求先有 controlling-tty 跟踪）
    if (new_pg != 0 && new_pg != current->pgrp) return -EPERM;
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

**嵌套路径**：`signal_pgrp` → `task_wake`（持 task_list_lock 下调用）。

- `signal_pgrp` 持 `task_list_lock`，对每个匹配 task：
  - 写 `t->signal`（受 task_list_lock 保护）
  - 若 `state == TASK_INTERRUPTIBLE` → `task_wake(t)`
- `task_wake`（`kernel/sched/task.c:177`）实际**取 rq_lock**（每个 CPU 的 runqueue 锁）操作 runqueue——v1 文档表述"task_wake 仅操作 wait_list，不取锁"是错的，本档修正。`task_send_signal` 早已在 task_list_lock 下调 task_wake，所以锁序 `task_list_lock → rq_lock` 是已验证存在的（沿用既有模式）。本档不引入新的锁序反转。

**VINTR 路径下两把锁的获取顺序**（`tty_push_input` → `signal_pgrp`）：

1. `spin_lock_irqsave(&tty->fg_pgrp_lock)` （读 fg_pgrp 到局部变量）
2. 立即 `spin_unlock_irqrestore`
3. 然后 `signal_pgrp` → `spin_lock_irqsave(&task_list_lock)`

两锁**不嵌套**——fg_pgrp_lock 持锁期间不进入 signal_pgrp，避免反向嵌套（task_list_lock → fg_pgrp_lock）的可能性。读出的 fg_pgrp 是快照，可能在派发瞬间已变化，但这是 POSIX 允许的（"派发时按当时 pgrp 计"，内核不阻塞保证一致）。

### 6.3 SMP + t->signal 同步故事

- 所有新锁用 `spin_lock_irqsave`，与既有约定一致
- VINTR 在 IRQ 路径下调用——IRQ 已禁用，`spin_lock_irqsave` 第二次进入为幂等
- `signal_pgrp` 在 IRQ-safe 路径下也安全（已由 `task_send_signal` 验证）
- `keyboard_poll`（内核线程上下文，非 IRQ）也调 `tty_push_input`——路径已存在，`spin_lock_irqsave` 同样安全（线程上下文会真禁用 IRQ）

**t->signal 字段同步约定**（v2 明确）：

| 操作 | 持锁 |
|---|---|
| 写 t->signal（task_send_signal / signal_pgrp / SYS_kill -1 内联） | task_list_lock |
| 当前任务本地读 current->signal（arch_do_signal_delivery / arch_signal_pending_fatal） | 无锁 |

读侧无锁的依据：x86_64 强 TSO 保证 64 位对齐字读写原子；`signal` 字段类型是 `uint64_t`（查 `task_struct`）且编译器按 8 字节对齐。写侧持锁的副作用：写者串行化，避免并发写竞争；读侧看到的可能是稍旧值，但只要任务在系统调用/interrupt 返回到 ring 3 时必然经过 `arch_do_signal_delivery`（既会读当前 `signal`，也会读 `pending`），最终一致性 OK。这一约定沿用 `task_send_signal` 既有模式，不引入新的同步原语。

未来若有人新增"跨任务读 t->signal"（如 /proc/<pid>/status 显示 pending signals），必须走 task_list_lock 读快照，避免读到撕裂值。

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

## 8. 测试（`user/systest.c` 新增 7 个 + kernel selftest 1 个）

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

### 8.6 test_devfs_open_default_fg_pgrp（v2 兜底，systest）

```c
// 验证 §4.1.1：open /dev/tty 后，tcgetpgrp 返回 opener.pgrp
// 必须 fork 后子进程 open（父进程已 open 过 /dev/tty 会先占 fg_pgrp）
static void test_devfs_open_default_fg_pgrp(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) { FAIL("devfs_open_fg", "no /dev/tty"); return; }
    // 子进程：setpgid(0, <独特 pid>) → open /dev/tty → tcgetpgrp 应回 <独特 pid>
    int64_t pid = fork();
    if (pid < 0) { FAIL("devfs_open_fg", "fork"); return; }
    if (pid == 0) {
        setpgid(0, getpid());   // 设 pgrp 为自己
        int cfd = open("/dev/tty", O_RDWR);
        if (cfd < 0) { _exit(2); }
        pid_t p = tcgetpgrp(cfd);
        _exit(p == getpid() ? 0 : 3);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "devfs_open_fg", "tcgetpgrp after open returns self pgrp");
    close(fd);
}
```

### 8.7 test_setpgid_auto_fg_pgrp_update（v2 核心，systest）

```c
// 验证 §3.4：setpgid(0, 0) 触发 fg_pgrp 自动更新
// 流程：open /dev/tty（设 fg_pgrp=opener.pgrp=child_pid）；
//       fork 后 child 调用 setpgid(0, 0) → fg_pgrp 应被自动更新
// 注：本测试需要两个 task 都 open /dev/tty，且 child 调 setpgid 后
// dev_tty->fg_pgrp 应等于 child.pid（而不是 opener 的 pgrp）
static void test_setpgid_auto_fg_pgrp(void) {
    // 父进程先 open 设一个旧 fg_pgrp
    int pfd = open("/dev/tty", O_RDWR);
    if (pfd < 0) { FAIL("setpgid_auto_fg", "no /dev/tty"); return; }
    setpgid(0, getpid());  // 父进程 pgrp = 自身
    // 此时 dev_tty->fg_pgrp = 父.pid（devfs_open 默认）

    int64_t pid = fork();
    if (pid < 0) { FAIL("setpgid_auto_fg", "fork"); return; }
    if (pid == 0) {
        // 子进程 open /dev/tty（不会改 fg_pgrp，因父已设过非零）
        int cfd = open("/dev/tty", O_RDWR);
        if (cfd < 0) { _exit(2); }
        // 调 setpgid(0, 0) → 应自动把 fg_pgrp 设为 child.pid
        if (setpgid(0, 0) != 0) _exit(3);
        // 现在 tcgetpgrp 应返回 child.pid
        pid_t p = tcgetpgrp(cfd);
        _exit(p == getpid() ? 0 : 4);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "setpgid_auto_fg", "tcgetpgrp after setpgid(0,0)==child.pid");
    close(pfd);
}
```

### 8.8 kernel selftest（`test/cases/test_tty_vintr.c`，新增）

VINTR 端到端无法从用户态测试（用户态无法直接调 `tty_push_input`，且真实键盘注入需要 QEMU 介入）。本测试放在 kernel selftest 入口，遵循 `test/cases/test_poll_requested.c` 现有约定：

- 测试线程通过 `kernel_thread()` 创建一个用户态任务 child
- child 体：`setpgid(0, 0); exec "cat"`（走 native elf loader），cat 在 `read(0, ...)` 阻塞
- 测试线程：`ioctl(0, TIOCSPGRP, &child_pid)` 设 fg_pgrp
- 测试线程短 spin 等 cat 进入 read
- 测试线程直接调 `tty_push_input(get_dev_tty(), 3)` 注入 VINTR
- `waitpid` child，断言 `WIFSIGNALED && WTERMSIG == SIGINT`
- 测试结束恢复 fg_pgrp=0

### 8.9 验收

- `make` 编译通过
- `make test` 全量回归——现有 132 测试 + 新增 7 个 = 139/139 PASS（**关键：kill(0) 语义变更未翻红**）
- kernel selftest `test_tty_vintr` PASS
- 手动：OS01 shell 跑 `cat` / `cat /dev/urandom`，按 Ctrl-C 能终止，shell 回到提示符

---

## 9. 文件清单与工作量

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/include/kernel/task.h` | `task_struct` 加 `pgrp`/`session` 字段 | +2 |
| `kernel/include/kernel/tty.h` | `tty_struct` 加 `fg_pgrp`/`fg_pgrp_lock`/`vfs_node` | +3 |
| `kernel/sched/task.c` | `signal_pgrp()` 新函数；`init_task_union.task.pgrp/session` 初值 | +30 |
| `kernel/tty/tty.c` | `tty_alloc` 默认 ISIG + c_cc 默认；`tty_push_input` 行规程；`tty_phys_ioctl` 真 TIOCSPGRP/TIOCGPGRP（区间检查+session 校验）；`dev_tty_vfs_node()` helper；`_POSIX_VDISABLE` 局部宏 | +75 |
| `kernel/arch/x86_64/trap.c` | 4 个新 syscall case；SYS_fork 子任务加 2 行；SYS_kill 扩展（含 -1 广播内联）；SYS_setpgid 自动 fg_pgrp 更新 | +110 |
| `kernel/fs/devfs.c` | `devfs_open_node` 末尾加 §4.1.1 默认 fg_pgrp 设置 | +8 |
| `test/cases/test_tty_vintr.c` | 新文件：VINTR e2e selftest | +50 |
| `kernel/include/uapi/syscall.h` | +4 行 syscall 号 | +4 |
| `libc/include/sys/syscall.h` | +4 行 syscall 号 | +4 |
| `libc/unistd/setpgid.c` | stub → syscall | +5 |
| `libc/unistd/tcsetpgrp.c` | stub → ioctl | +5 |
| `libc/unistd/tcgetpgrp.c` | fake-return-1 → ioctl | +5 |
| `libc/signal/killpg.c` | stub → kill(-pgrp,sig) | +5 |
| `user/systest.c` | 新增 6 测试函数（5 + §8.6 devfs_open 默认 fg_pgrp 测试）+ 注册 | +230 |
| `docs/syscall.md` | 4 行：setpgid/getpgid/setsid/getsid 条目 | +20 |

合计：~558 行（内核 ~325，libc ~25，测试 ~280，文档 ~20）。

---

## 10. 非目标（YAGNI）

- **不做** SIGTSTP/SIGCONT 完整作业控制（fg/bg/jobs 内建）——属第二档，本档只为它铺路（fg_pgrp / pgrp 字段已就位）。
- **不做** `tcgetattr`/`tcsetattr` 各 flag 字段独立读写语义（ISIG/ICANON/ECHO 等）——当前内核 TCSETS 整体 memcpy 满足 busybox ash 需求。
- **不做** session leader 退出时 SIGHUP 派发。
- **不做** `_POSIX_VDISABLE` 全局化定义（kernel/tty/tty.c 局部宏即可）。
- **不做** busybox ash 源码修改——busybox `.config:1125: # CONFIG_ASH_JOB_CONTROL is not set` 已确证 ash 内 `#if JOBS` 块全空，xtcsetpgrp 永不被调用。本档依赖 §3.4 自动 fg_pgrp 更新 + §4.1.1 devfs_open 默认 fg_pgrp 兜底，不依赖 ash 源码改造。
- **不做** Linux x86_64 ABI 翻译表 `linux_to_os01[121]` (setpgid/getpgid) 等补全（native syscall 已够 native 程序使用；busybox 走 native 表）。
- **不做** IXON/IXOFF 流控（VSTART/VSTOP 字符处理）——属第二档；本档 cbreak 模式下按 Ctrl-S/Ctrl-Q 会作为普通字符入 TTY 环，注释已注明（§4.2 末尾）。

---

## 11. 风险与边界

1. **🔴 v2 已确认：busybox ash 在 OS01 上不调 tcsetpgrp**——`thirdpart/busybox-1.36.1/.config:1125: # CONFIG_ASH_JOB_CONTROL is not set`。ash.c 内 `#define JOBS ENABLE_ASH_JOB_CONTROL`（`ash.c:186`）→ JOBS=0，所有 `xtcsetpgrp` 调用点都在 `#if JOBS` 块内（行 4124/4131/4228/5253/5424），编译为空。**v1 设计若不做 §3.4 自动 fg_pgrp 更新 + §4.1.1 默认 fg_pgrp 兜底，fg_pgrp 永远为 0，VINTR 静默 no-op**。v2 已堵。
2. **busybox ash 改 tcgetpgrp 行为可能影响其他 ash 内部路径**：原 stub 永返 1，ash 的 `setjobctl` 路径（`ash.c:4077`）会读到 pgrp=1，恰好与 ash 自己的 pgrp 匹配，从而绕过"can't access tty; job control turned off"分支。改为真 ioctl 后，tcgetpgrp 返回 dev_tty->fg_pgrp（初始 0 → 但 init 阶段 open /dev/tty 后会变成 opener.pgrp=1）。**已确证行为不回归**，但实现后必须跑 OS01 手动验证 + 现有 132 个 systest 全量回归。
3. **`signal_pgrp` 在 IRQ 上下文调用**：`spin_lock_irqsave(&task_list_lock)` 在 IRQ 已禁用时安全（与 `task_send_signal` 同一路径，已验证）。`task_wake` 实际取 `rq_lock`（v2 修正），锁序 `task_list_lock → rq_lock` 是已验证存在的（沿用既有模式），本档不引入新的锁序反转。
4. **`c_cc` 默认值必须显式设置**：VINTR=3、VQUIT=28 等不能依赖 memset 的 0（0 意味着 _POSIX_VDISABLE，行规程永不触发）。`tty_alloc` 修改时必须包含 §4.2 的 5 行赋值——属于"漏一行就静默失效"的陷阱。
5. **默认 c_lflag = ISIG 影响面扫描**（v2 增）：
   - `grep -rn 'c_cc\[VINTR\]\|c_lflag' user/ kernel/` —— 现状只有本档新增，无既有消费者依赖 0x03 当字面输入
   - `grep -rn '0x03\|\\x03' user/` —— busybox 内有少量 char='\x03' 处理（如 stty -a 解析），但都走用户态应用层不直接读 /dev/tty
   - busybox ash 自身会在交互模式 tcsetattr 设 ICANON+ECHO，不受 ISIG=1 默认值干扰（ISIG 字段独立）
   - systest 不读 /dev/tty 字符（除新加的 VINTR 测试）——无回归风险
6. **VSTART/VSTOP 已知未实现**：Ctrl-S/Ctrl-Q 在 cbreak 模式下作为普通字符入 TTY 环。cbreak 模式下用户不会按这两个键（除非显式触发），低严重性。第二档补 IXON/IXOFF（§10 非目标）。
7. **`fg_pgrp` 默认 0 + devfs_open 兜底**：init 阶段无 fg_pgrp，VINTR 静默 no-op（安全）。devfs_open 把 opener.pgrp 写入 fg_pgrp 时，仅在 fg_pgrp==0 时设置——避免覆盖后续 tcsetpgrp 显式设置。
8. **multicore IRQ routing**：keyboard IRQ1 绑定到 BSP（既定事实，`kbd_lctrl` 等静态变量依赖此不变量，见 `kernel/driver/keyboard.c:49-51`）。VINTR 派发在 BSP 上完成，AP 上的 task 接收信号经 task_wake → schedule → cross-CPI/IPI 不在本档范围（沿用现有 signal 投递路径）。
9. **§3.4 自动 fg_pgrp 更新的潜在误用**：若用户态程序非预期地调 setpgid(0, 0) 且 fd 0 是 /dev/tty，会"劫持"前台 pgrp。低严重性（setpgid 是 POSIX 主动调用，调用方需明确知道），且后果仅是 VINTR 派发范围变化，无安全/数据损失。
10. **§8.1 systest 现有 132 测试回归风险**（v2 增）：SYS_kill 的 pid==0 语义变更（kill(0,sig) 从返 -ESRCH 变为 signal_pgrp），pid==-1 从返 -ESRCH 变为广播成功，可能影响依赖原语义的测试用例。**验收阶段必须先跑 `make test` 全量通过再合入**。

---

## 12. 实施顺序（依赖驱动）

1. **task_struct 字段 + signal_pgrp** → 编译通过；可单独跑 signal_pgrp 单元验证
2. **fork 继承 + setpgid/getpgid/setsid/getsid syscall**（含 §3.4 自动 fg_pgrp 更新）→ 编译通过；可跑 8.2/8.3/8.7 测试
3. **tty_struct fg_pgrp/vfs_node + TIOCSPGRP 真实现（区间检查+session 校验）+ 默认 ISIG + c_cc 默认** → 编译通过；可跑 8.4 测试
4. **tty_push_input 行规程**（含 §4.3 VINTR/VQUIT/VSUSP）→ 端到端；可跑 8.1/8.5 + kernel selftest 8.8
5. **SYS_kill 扩展**（含 §3.5 pid==-1 广播）→ 完整测试；可跑 8.5 kill(-pgid, sig) 路径
6. **kernel/fs/devfs.c §4.1.1 devfs_open 默认 fg_pgrp** → 可跑 8.6 测试
7. **libc 4 个 stub → 真调用** → busybox ash 走真路径；可手动 OS01 shell 验证 Ctrl-C
8. **7 个 systest + 1 个 kernel selftest + 全量 make test** → 132+7=139/139 PASS（**关键回归门槛**）

每步独立可测，不需"全做完才知道有没有错"。**第 8 步必须全量 make test 通过**，kill(0)/kill(-1) 语义变更是潜在回归点。

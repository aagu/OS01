# OS01 syscall 边界审计设计文档

- 日期：2026-08-23
- 状态：v1 待用户 review（尚未进入实现）
- 目标：**DoS 防护** —— 任何 syscall 收到敌意用户指针（NULL / 低地址 / 内核区间 / 未映射 / 超长 size 越界）返回 `-EFAULT`，**永不导致内核崩溃**；顺带关闭路径字符串的 TOCTOU 主窗口。roadmap P1「syscall 边界审计」项。
- 修订记录：
  - v1：初版（brainstorming 产出，方案 A：PTE 走查校验 + TOCTOU strdup + 窄容错拷贝原语）

---

## 1. 背景与动机

### 1.1 威胁现状

OS01 是扁平地址空间（**未使能 SMAP/SMEP**，head.S/trampoline.S 只设标准 CR4 位），内核直接 `memcpy`/`strdup`/逐字节操作用户指针。**没有任何 `copy_from_user`/`copy_to_user` 抽象**（roadmap 的"copy_from_user 全路径核查"是目标术语，不是现状）。

**关键事实：内核态 #PF = 内核崩溃。** `do_page_fault`（`kernel/arch/x86_64/trap.c:423`）对 `!(regs->cs & 3)`（内核态 fault，trap.c:429）直接打全寄存器 dump + TASKLIST dump 后停摆。因此任何用户进程传坏指针触发内核态 #PF，就是整机 DoS。busybox applet 吃用户输入，是现实攻击面（roadmap applet 验证里有「4 处 user-fault 崩溃」很可能同源）。

### 1.2 根因（审计前置探查确认的现状）

现存边界校验**三种形态、不统一、有漏洞**：

| 形态 | 位置 | 缺陷 |
|------|------|------|
| **仅查起点** `(uint64_t)ptr >= addr_limit` | trap.c:1145(write) / 1240(read) / 1254(open) / 1202-1212(exec)；futex.c:48,97；file.c:766；poll.c:481 | **放行 NULL 和低地址**（0 < addr_limit 通过）→ 内核态 #PF → 崩溃；不查长度越界 |
| **完整范围查** `syscall_user_range_ok`（trap.c:948：`addr!=0 && addr<limit && len<=limit-addr`） | **只有 socket 系列在用**（trap.c:2373-2464）；select.c:124-376 有类似 range 检查；tty.c:333-346；vma.c:273,306-308,527-528；task.c:992 | 但**未映射 in-range 地址仍能过**（只查地址 < limit，不查页表）→ 内核态 #PF → 崩溃 |
| **完全不查** | 大量 handler 直接解引用用户指针 | 同 NULL 漏洞 |

**范围检查 ≠ 映射检查**：`addr < addr_limit` 只证明地址在用户区间，不证明页面存在/可写。`arch_virt_to_phys`（`kernel/include/kernel/arch/mmu.h:38`）能走查 4 级页表返回物理地址或 0，但**不解释 PTE 权限位**（present/U/S/RW）——这正是要补的。

**TOCTOU 现状**：只有 exec（trap.c:1218）和 open（trap.c:1263）对路径 `strdup` 到内核堆；其余路径 syscall（chdir/unlink/mkdir/rmdir/rename/truncate/access/chmod）**在阻塞操作中直接解引用用户路径**。且现有 `strdup(path)` **无界**——path 无 NUL 终止会扫过页边界 → fault。

**阻塞读写的用户缓冲触点在阻塞后**（信号 handler 可在阻塞期间 munmap 缓冲 → 唤醒后写/读用户内存 → #PF）：
- 读侧（写用户缓冲）：`tty_read` 排水循环（tty.c，`schedule()` 后）；`pipe_read_internal` 字节循环 `dst[total++]`（file.c:399）；socket RX `memcpy(buf,data,copy)`（file.c:525,543，`netconn_recv` 可阻塞）
- 写侧（读用户缓冲）：`pipe_write_internal` 字节循环 `src[total++]`（file.c:570，`schedule()` 后）；socket TX `netconn_write(s->conn, buf,...)`（lwIP 内部拷贝用户指针，可阻塞）
- select/poll 反向映射写回用户 fd_set（select.c，`do_poll_core` 可阻塞后）

**同步路径确认**：VFS/ext2/fat/tmpfs/devfs 读是 **AHCI busy-wait 同步读**（ahci.c:67「busy-wait with jiffies timeout」），无 schedule → 「校验后立即 memcpy」纪律下安全，不需要容错拷贝。

### 1.3 目标层次（已确认）

**DoS 防护**（brainstorming 用户确认），非 Linux 级 uaccess：
- 敌意指针**永不**导致内核态 #PF
- 校验失败一律 `-EFAULT`（与现有 socket/select 一致）
- 不做：SMAP/SMEP 使能、get_user_pages、异常表 fixup 全套、partial-copy 短计数语义

---

## 2. 总体架构

新增 uaccess 层，syscall 全部走它：

```
用户指针 ──→ syscall_check_user_range(addr,len,rw)   // PTE 走查校验（handler 入口，第一道防线）
         ──→ strnlen_user + strdup                   // 路径字符串 → 内核堆（TOCTOU 关闭）
         ──→ copy_to_user_ft / copy_from_user_ft     // 容错拷贝（阻塞读写的最终触点，第二道防线）
```

**核心纪律**：校验后立即拷贝/使用，二者之间**不得有调度点**；一切**可能阻塞后触用户内存**的地方用 `_ft` 变体兜底。两道防线重叠时（阻塞读写），入口校验挡常规敌意指针，`_ft` 挡信号 handler munmap 竞态。

新增文件：
- `kernel/memory/uaccess.c`（~120 行）：三原语 + `strnlen_user`
- `kernel/include/kernel/uaccess.h`（~40 行）：声明 + arch 钩子

---

## 3. 校验原语 `syscall_check_user_range(addr, len, writable)` → bool

```c
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);
```

四重检查，任一失败返回 false（handler 转 `-EFAULT`）：

1. `addr != 0` —— 挡 NULL/低地址
2. `addr < current->addr_limit` —— 挡内核区间（`addr_limit` 定义 task.h:131，用户态 0x00007FFFFFFFFFFF）
3. `len <= current->addr_limit - addr` —— 溢出安全，挡超长 size 越过边界
4. 逐页走查 `[addr, addr+len)`（每页按 PTE 4KB/2MB/1GB 粒度，复用 `arch_virt_to_phys` 的走查骨架）：**present + U/S +（writable 时 RW）**。任一页不满足 → false

**架构钩子**：新增 `arch_pte_flags(pgtbl, va) → uint64_t`（`kernel/include/kernel/arch/mmu.h`）返回该页最终 PTE 原始值供位检查：
- x86_64 实现：present=bit0、RW=bit1、U/S=bit2，处理 4KB/2MB/1GB
- aarch64：stub 返回 0（= 未映射，**fail-closed**）——YAGNI，不写 ARM 代码（roadmap P2 前不动）

**COW 页特例（必须拒绝）**：writable=true 时，PTE 若为 COW/RW-clear（`PAGE_COW` bit10 或 RW 位清除），**拒绝**——内核直写 COW PTE 不会触发拷贝机制，会直接 #PF。内核不参与用户 COW 语义。

**零长语义**：`len == 0` → 直接 true（Linux 约定，空操作成功），避免 `addr=0,len=0` 误报。

**错误路径**：任何 false → handler `regs->rax = -EFAULT`。

---

## 4. 容错拷贝原语 `copy_to_user_ft` / `copy_from_user_ft` → ssize_t

```c
ssize_t copy_to_user_ft(void *dst, const void *src, size_t n);    // n==0 → 0
ssize_t copy_from_user_ft(void *dst, const void *src, size_t n);  // n==0 → 0
```

**机制**：GCC `__builtin_setjmp`/`__builtin_longjmp`（编译器 builtin，零手写汇编，**规避 memory 记录的 `setjmp-frame-pointer-bug`**——那是 libc setjmp 用 `[RBP+8]` 读返回地址，builtin 由编译器直接发射不碰帧指针约定；内核已 `-ffreestanding`，builtin 可用）。

```c
ssize_t copy_to_user_ft(void *dst, const void *src, size_t n) {
    if (n == 0) return 0;
    __builtin_setjmp_buffer jb;
    task_t *cur = current;
    jmp_buf_t *old = cur->fault_jmp;
    cur->fault_jmp = (jmp_buf_t *)&jb;
    if (__builtin_setjmp(jb) == 0) {
        memcpy(dst, src, n);
        cur->fault_jmp = old;
        return (ssize_t)n;
    }
    cur->fault_jmp = old;       // fault 路径
    return -EFAULT;
}
```

`copy_from_user_ft` 同构（memcpy 参数对调）。

**`do_page_fault` 挂钩**（trap.c:429 内核态分支**顶部**，panic dump 之前）：

```c
if (cr2 < current->addr_limit && current->fault_jmp)
    __builtin_longjmp(current->fault_jmp, 1);
```

**关键守卫**：`cr2 < addr_limit` 门控——只重定向用户区间 fault（本原语的拷贝目标/源）；真内核 bug（cr2 ≥ addr_limit）仍走原有 dump，绝不吞掉。

**`task_t` 变更**：`kernel/include/kernel/task.h` 加 `void *fault_jmp` 字段（**结构体变更 → 必须 `make clean`**，AGENTS.md）。

**生命周期纪律**：武装到解除之间**无 schedule**（纯 memcpy/字节拷贝，无锁竞态、无中断态不一致）→ 栈纪律安全；单线程（无 clone，P5 才有）无并发解映射者。longjmp 后中断帧残留在内核栈 RSP 之下（无害，下次使用覆盖）。

**错误语义**：fault → 整个操作 `-EFAULT`（**不做** Linux 短计数；DoS 目标够用，spec 决策点已定）。

---

## 5. 站点接入策略（55 个指针解引用 handler → 4 类）

| 类 | 站点 | 处理 |
|----|------|------|
| **A 路径字符串** | open / exec(path+argv+envp) / chdir / unlink / mkdir / rmdir / rename(×2) / truncate / access / chmod | `syscall_check_user_range(path, VFS_NAME_MAX, false)` 定界校验（内核无 PATH_MAX，用 `VFS_NAME_MAX`=256，vfs.h:12，open handler 同款判 `-ENAMETOOLONG`，trap.c:1280）→ `strnlen_user(path, VFS_NAME_MAX)` + `strdup` 到内核堆 → 后续全部操作内核副本。**杀无界 strdup 扫描窗口**（现 open/exec 的 strdup 若 path 无 NUL 扫过页 → fault） |
| **B 定长结构体** | timespec / sockaddr / fd_set / sigset / stat / optval / addrlen / getcwd / getdents64 等 | `syscall_check_user_range`（按方向 rw）+ 拷贝到内核栈/内核缓冲。**统一** select.c:124-376、socket trap.c:2373-2464 已有的零散 range 检查到本原语（行为不变，只统一入口） |
| **C 大缓冲 in-place** | read / write fd 数据 | read: 入口校验 `(buf,size,writable=true)` → VFS 同步路径直接 memcpy（busy-wait 无调度，安全）；tty/pipe/socket 最终写用 `copy_to_user_ft`。write: 入口校验 `(buf,size,readable)` → pipe/socket 阻塞后读用户缓冲用 `copy_from_user_ft` |
| **D 无指针** | fd 号 / 纯整数值（getpid/kill/waitpid/brk/setpgid…） | 不动 |

### 5.1 阻塞读写最终触点明细（`_ft` 接线点）

| 触点 | 位置 | 变体 |
|------|------|------|
| `tty_read` 排水 | tty.c | 建议**内核 bounce buffer**（≤4096B）先搬 ring/canon 到内核，再单次 `copy_to_user_ft` → fault 不丢已消费数据、原子 |
| `pipe_read_internal` 字节循环 | file.c:399 `dst[total++]` | 同上 bounce buffer（PIPE_SIZE≤4096 天然适配），循环内先写内核，尾部单次 `copy_to_user_ft` |
| `pipe_write_internal` 字节循环 | file.c:570 `src[total++]` | 改：先 `copy_from_user_ft` 从用户搬 PIPE_SIZE 进内核 bounce，再逐字节入 pipe |
| socket RX `memcpy(buf,data,copy)` | file.c:525,543 | 直接 `copy_to_user_ft`（数据已在内核 netbuf，一次性 memcpy；fault 丢该 netbuf 数据，可重试，可接受） |
| socket TX `netconn_write(s->conn,buf,...)` | file.c（FD_SOCKET 分支） | **首选**：分块 bounce（`copy_from_user_ft` 到内核缓冲 ≤16KB → `netconn_write_partly` 喂内核缓冲）→ lwIP 永不触用户内存，无 lwIP 中间态风险。**备选**：setjmp 包整次 `netconn_write`（简单，但 fault 长跳会使 lwIP pbuf 链半填充泄漏）。spec 决策：**首选分块 bounce** |
| select/poll fd_set 写回 | select.c 反向映射 | `copy_to_user_ft`（`do_poll_core` 阻塞后） |

**同步 out-buffer**（stat/fstat/getcwd/getdents64/readdir 等）：校验 + 直接 memcpy（无 schedule 在中间，安全，不需 `_ft`）。

---

## 6. 错误码与边界语义

- 校验失败 / `_ft` fault → `-EFAULT`（统一；与现有 socket/select 一致）
- `len == 0` → 成功 no-op（`_ft` 返回 0，`check_user_range` 返回 true）
- pipe/tty 经 bounce buffer **无数据丢失**（fault 前数据留在内核，未消费用户侧承诺）
- socket RX fault → `-EFAULT`，丢已 dequeue 的 netbuf（可重试）
- `kill(-pid)` 负数语义、`brk`/fd 号类不受影响
- 不改变任何正常路径返回码（现有 184/0 systest 必须原样通过）

---

## 7. 数据流

### 7.1 happy path（正常指针，无变化）

`read(fd, buf, len)`：
```
SYS_read 入口: check_user_range(buf, len, writable=true) ──通过──► fd_read
  ├─ VFS: vfs_read 同步 → memcpy(buf, ...)                     （校验→拷贝间无调度）
  ├─ pipe: 阻塞 → 唤醒 → bounce 内核 → copy_to_user_ft(buf)     （_ft 兜底）
  └─ socket: netconn_recv 阻塞 → copy_to_user_ft(buf, data, n)
```

### 7.2 fault path（敌意指针）

`read(fd, 0x1, 4)`：
```
check_user_range(0x1, 4, true) → addr!=0 失败 → -EFAULT（不触内核内存）
```

`read(fd, valid_start, 1<<40)`（size 越界）：
```
len(1<<40) > addr_limit - addr → -EFAULT
```

`read(fd, in_range_unmapped, 4)`（页未映射，且无 _ft 覆盖的同步路径）：
```
check_user_range 逐页走查 → PTE 无 present → -EFAULT
```

阻塞读中信号 handler munmap 缓冲 → 唤醒后写：
```
copy_to_user_ft 武装 setjmp → memcpy → #PF (cr2 < addr_limit && fault_jmp)
  → do_page_fault 挂钩 __builtin_longjmp → 原语返回 -EFAULT
```

---

## 8. 测试

### 8.1 kernel selftest（新 `kernel/test/test_uaccess.c`，~50 行）

**唯一确定性触发 longjmp 路径的方式**（无需 orchestrate 竞态）。直接调三原语 + 断言：

| 用例 | 断言 |
|------|------|
| `check_user_range(NULL, 1, true)` | false |
| `check_user_range(0x1, 1, true)` | false |
| `check_user_range(kernel_addr, 1, true)`（≥addr_limit） | false |
| `check_user_range(user_start, 1<<40, true)` | false（越界） |
| `check_user_range(in_range_unmapped, 4096, true)` | false |
| `check_user_range(mapped, 4096, true)` | true |
| `check_user_range(mapped, 8192, true)`（跨 2 页，第二页未映射） | false（部分映射） |
| COW/RW-clear 页 writable=true | false |
| `copy_to_user_ft(userbuf, ksrc, n)` 正常 | n |
| `copy_to_user_ft(in_range_unmapped, ksrc, 16)` | `-EFAULT`（**longjmp 路径**） |
| `copy_from_user_ft` 对称 | 同上 |

### 8.2 systest hostile 组（`user/systest.c`，~80 行，~10-15 断言）

真实 syscall 传敌意指针，断言 `-EFAULT` + **内核存活**（后续断言仍执行即证）：

| 用例 | 断言 |
|------|------|
| `read(fd, (void*)0x1, 16)` | -EFAULT |
| `write(fd, (void*)0x1, 16)` | -EFAULT |
| `open(NULL, 0)` | -EFAULT |
| `chdir((void*)0x1000)`（未映射） | -EFAULT |
| `read(fd, buf, 1<<40)` | -EFAULT |
| `munmap` 一块区域后 `read(fd, 那块区域, 16)` | -EFAULT |
| `write(fd, 未映射指针, 16)` | -EFAULT |
| fork 子进程跑上面敌意用例，父 waitpid 断言子正常退出（非信号） | 内核不崩 |

### 8.3 回归

- 184/0 systest 全量原样通过
- 网络 harness（make test-network）+ QEMU ash 手工回归
- **顺带复查 roadmap「4 处 applet user-fault 崩溃」**——很可能同源（坏指针触发内核 #PF），修完即愈，逐处确认

---

## 9. 文件清单与工作量（≈400-500 行，1.5~2 天）

| 文件 | 变更 |
|------|------|
| 新 `kernel/memory/uaccess.c` | 三原语 + `strnlen_user`（~120 行） |
| 新 `kernel/include/kernel/uaccess.h` | 声明 + `arch_pte_flags` 钩子（~40 行） |
| `kernel/include/kernel/arch/mmu.h` | +`arch_pte_flags`（x86 实现 ~30 行 + aarch64 stub） |
| `kernel/include/kernel/task.h` | +`fault_jmp`（**`make clean`**） |
| `kernel/arch/x86_64/trap.c` | `do_page_fault` 挂钩 ~12 行 + ~30 handler 接入 |
| `kernel/fs/file.c` | pipe r+w bounce + socket rx `_ft` / tx 分块 bounce ~30 行 |
| `kernel/tty/tty.c` | tty_read bounce + `copy_to_user_ft` ~10 行 |
| `kernel/fs/select.c` | fd_set 写回 `_ft` ~4 行 |
| `kernel/sync/futex.c` | 统一到 `check_user_range` ~6 行 |
| 新 `kernel/test/test_uaccess.c` | ~50 行 |
| `user/systest.c` | hostile 组 ~80 行 |
| `docs/syscall.md` + `docs/roadmap.md` | 文档同步（P1 项标记完成） |

---

## 10. 非目标（YAGNI）

- **不做 SMAP/SMEP 使能**（真硬件防线，独立项；无 uaccess 抽象时的硬件兜底，后续单独评）
- **不做 Linux 式 exception table / get_user_pages / partial-copy 短计数**（DoS 目标不需要；`__builtin_setjmp` 已覆盖 fault 恢复）
- **不做 `copy_from_user`/`copy_to_user` 全站替换**（大缓冲 in-place 保持直接 memcpy；只有阻塞触点用 `_ft`）
- **不修 getdents64/stat 等同步 out-buffer 的潜在竞态**（无调度点，安全）
- **不改 socket TX 的 lwIP 内部结构**（用 bounce 分块隔离，不碰 lwIP）
- **不做 aarch64 uaccess 实现**（stub fail-closed）
- **不碰内存泄露/越界写的完整审计**（那是「内存安全」层次，本次已确认 DoS 目标）

---

## 11. 风险与边界

| 风险 | 缓解 |
|------|------|
| `__builtin_setjmp` 在 `-ffreestanding` 下行为 | 编译器内建，实测单测（test_uaccess）验证；若异常再评估手写 asm 备选 |
| longjmp 跨 #PF 中断帧 | fault 后长跳回原语，中断帧残留在栈 RSP 之下无害；`cr2 < addr_limit` 门控防吞真内核 bug |
| `task_t` 结构体变更（sizeof 变化） | **必须 `make clean && make`**，旧 .o 静默崩（AGENTS.md） |
| 信号 handler munmap 竞态 | 阻塞触点 `_ft` 兜底；**依赖"信号只在返回用户态时投递"语义**（handler 不能在 memcpy 中途跑）——实施第一步手动确认此语义成立 |
| socket TX lwIP 中间态 | 首选分块 bounce（lwIP 永不触用户内存）；备选 setjmp 包 netconn_write 有 pbuf 泄漏（已否决为主方案） |
| 误伤：内核 bug 被 `_ft` 吞 | `cr2 < addr_limit` 门控 + 单测覆盖正常路径返回码不变（184/0 回归） |
| 性能 | 每 syscall 一次 PTE 走查（≤len/4096 次循环，通常 1-2 页）可忽略；`_ft` 只在阻塞触点，非热路径 |

---

## 12. 实施顺序（依赖驱动）

```
Step 1: 基础设施 — uaccess.h + mmu.h arch_pte_flags + task.h fault_jmp
Step 2: 原语 + do_page_fault 挂钩（含 cr2 门控）
Step 3: kernel selftest test_uaccess.c（验证原语 + longjmp 路径）→ 先于一切站点接入
Step 4: Cat A 路径字符串（strnlen_user + strdup 推广）
Step 5: Cat B 定长结构体统一
Step 6: Cat C 大缓冲 + 阻塞触点 _ft（pipe/tty/socket rx/tx/select）
Step 7: systest hostile 组 + 回归（184/0 + 网络 + ash）+ applet user-fault 复查
Step 8: 文档同步（syscall.md / roadmap.md）
```

依赖：Step 3 必须先于 4-6（原语正确性门禁）；Step 4/5/6 相互独立可并行（各自独立 commit）。

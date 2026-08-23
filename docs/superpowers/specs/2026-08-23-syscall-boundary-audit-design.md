# OS01 syscall 边界审计设计文档

- 日期：2026-08-23
- 状态：v5 待用户 review（尚未进入实现）
- 目标：**DoS 防护** —— 任何 syscall 收到敌意用户指针（NULL / 低地址 / 内核区间 / 未映射 / 超长 size 越界）返回 `-EFAULT`，**永不导致内核崩溃**；顺带关闭路径字符串的 TOCTOU 主窗口。roadmap P1「syscall 边界审计」项。
- 修订记录：
  - v1：初版（方案 A：PTE 走查校验 + TOCTOU strdup + 窄容错拷贝原语）
  - v2：首轮 review——Step 0、`current->fault_jmp` 统一、4KB+2MB、Cat A ✅、无短计数、applet 名单、`_ft` schedule 风险、commit 模板
  - v3：二轮 review（4 P0）——跨级有效权限、USER_MIN_ADDR、exec argv/envp 深拷贝、fault-tolerant strnlen_user、selftest 合成页表
  - v4：三轮 review（3 阻塞 + 2 次要）——完整逐 syscall 矩阵、waitpid 移出 D、VFS bounce 数据流、do_pipe 回滚、统一 user_write_range、selftest 测 walker、纪律改"所有解引用"
  - v5：四轮 review（2 P0 + 1 语义）——
    - 🟢 **pipe 回滚修正**：只 `fd_close(rfd)+fd_close(wfd)`，**不显式 `pipe_free(p)`**（`file_free` 在最后一个 pipe-end close 时已 `pipe_free`，file.c:64；显式再 free = double-free。与 do_pipe 现有 fd_alloc 失败路径一致）
    - 🟢 **futex 方案修正**：`do_futex_wait` 是 `user_va_to_phys`+`Phy_To_Virt` **内核直映射读**（不可能 #PF），且**无阻塞后重读**（wake 后只清 wq）——v4 矩阵"WAIT 阻塞后重读需 `copy_from_user_ft`"是**错误**。futex 保留内核映射读，只加入口 `check_user_range`+`USER_MIN_ADDR` 一致性，**不用 `_ft`**
    - 🟢 **§4 硬约束**：`_ft` 禁止在持有 spinlock / IRQ critical section / 持有必须解锁的资源时使用（longjmp 绕过解锁 → 永久死锁）
    - 🟢 **§6/§7 语义统一**：分块层 `submitted == 0` → 返回 `-EFAULT`（非 0，避免与 EOF 混淆）；`submitted > 0` → 返回短计数

---

## 1. 背景与动机

### 1.1 威胁现状

OS01 扁平地址空间（**未使能 SMAP/SMEP**），内核直接解引用用户指针。**无 `copy_from_user`/`copy_to_user` 抽象**。**内核态 #PF = 内核崩溃**（`do_page_fault` trap.c:423，内核态分支 trap.c:429 dump 后停摆）→ 任何用户进程传坏指针即整机 DoS。

### 1.2 根因（审计前置探查确认的现状）

| 形态 | 位置 | 缺陷 |
|------|------|------|
| **仅查起点** `>= addr_limit` | trap.c write:1145/read:1240/open:1254/exec:1202-1212/chdir:1426/ioctl:1660/getdents64:1680/getcwd:1486/stat:1500/waitpid:1409；futex.c:48,97；file.c:766(do_pipe)；poll.c:481 | **放行 NULL 和低地址**（0 < addr_limit 通过）→ #PF；不查长度越界 |
| **完整范围查** `syscall_user_range_ok`（trap.c:948） | socket 系列（2373-2484）；select.c:124-376；tty.c:333-346；vma.c:273,306-308,527-528 | **未映射 in-range 地址仍能过**（不查页表）→ #PF |
| **完全不查** | pipe(1419)、getrandom 依赖 user_write_range(已查)、大量 handler | 同 NULL 漏洞 |

**范围 ≠ 映射**；**x86 有效权限是跨级 AND**（pml4→pdp→pd→pt 的 present/U/S/RW 逐级累积，任一级 U/S=0 → 超级用户页）。`arch_virt_to_phys`（mmu.h:38）只返叶 PTE 物理地址不累积权限。

**已有半成品机制**：`user_write_range_begin/end`（vma.c:522-556）——addr==0/addr_limit/overflow 检查 + **持 `mm->lock`**（防并发 munmap/mprotect）+ 逐页叶 PTE 校验（present+RW，**拒 COW**）+ 2MB 巨页。**两个缺口**：① 只查叶 PTE（**不查上层权限**，P0-1 仍在）；② **持锁方案只适用于非阻塞**（getrandom/devfs random 读用它，vma.c:522 注释 + devfs.c:120-134）。阻塞路径持锁会死锁 → 需 `_ft`。

**TOCTOU 现状**：exec/open/stat/chdir 已 strdup 路径，但 **chdir 在 kfree 后用原始 `path[0]`**（trap.c:1446）、**open O_CREAT 分支用 `strlen(path)`/`memcpy(pbuf,path)`**（trap.c:1280-1282）——窗口仍在；其余路径 syscall 无 strnlen_user 定界（无界扫描窗口）。

**exec argv/envp 深解引用**（task.c:1353-1373）：无界数组遍历 + 每元素 `strlen`/`memcpy` + `str_offset[128]` 固定数组（>128 项内核栈溢出隐患）；trap.c:1207-1215 只查数组首地址。**深拷贝必须在建新 pml4/释放旧空间前完成**。

**阻塞后触用户内存**（信号 handler 可在阻塞期 munmap 缓冲）：tty_read 排水、pipe_read_internal/pipe_write_internal 字节循环、socket rx/tx、**waitpid `*user_status`**（task.c:993，`schedule()` 后）、**nanosleep `rem` 写**（trap.c:2052，blocker_wait 后）、**recvfrom addr 写回**（trap.c:2431,2433，do_recvfrom 后）、select/poll fd_set 回写。

**同步路径**：VFS/ext2/fat/tmpfs/devfs 是 AHCI busy-wait 同步读（ahci.c:67），无调度点；但 **fd_read 把用户 buf 直接传给 vfs_read → FS 回调内部解引用**——若不 bounce，fault 发生在 FS 回调内，`_ft` 根本到不了。

### 1.3 目标层次（已确认）

**DoS 防护**：敌意指针永不导致内核态 #PF；校验失败一律 `-EFAULT`。不做 SMAP/SMEP、get_user_pages、异常表 fixup 全套、partial-copy 短计数。

---

## 2. 总体架构

三个机制分工（统一，不重复造）：

```
① syscall_check_user_range(addr,len,rw)   走查器：跨级有效权限 + USER_MIN_ADDR + addr_limit + 溢出
     └─ arch_user_range_accessible(pgtbl,addr,len,rw)   [参数化 pgtbl，selftest 用合成表]
② user_write_range_begin/end (vma.c:522)  非阻塞内核→用户写的 lock 方案（getrandom/devfs），
     内部走查器升级为跨级权限（修 P0-1），不重复实现
③ copy_to_user_ft / copy_from_user_ft / strnlen_user   容错拷贝（阻塞路径 + 一切兜底）
```

**核心纪律（v4 改为"所有用户内存解引用"）**：syscall 路径上**任何对用户内存的解引用**（`memcpy`/`memcpy_from`/`memcpy_to`/字段赋值/`memset`/`strcpy`/回调传用户指针）一律经以下之一，**不允许裸解引用用户指针**：
1. 路径字符串 → `strnlen_user` + `strdup`（内核堆副本）
2. 定长结构体/out-buffer → 拷贝到内核栈/内核缓冲（`copy_from_user_ft`），操作内核副本，结果 `copy_to_user_ft` 回写
3. 大缓冲 → 入口 `check_user_range` + 底层先写内核 bounce、`_ft` 回写（或反向）；阻塞触点 `_ft`
4. 非阻塞内核→用户写（getrandom/devfs）→ 沿用 `user_write_range_begin/end`（持锁，walker 已升级）

**校验后立即拷贝/使用，之间不得有调度点**；一切可能阻塞后触用户内存用 `_ft`/bounce 兜底。

新增文件：`kernel/memory/uaccess.c`（~220 行）、`kernel/include/kernel/uaccess.h`（~60 行）。**改造**：`kernel/memory/vma.c` 的 `user_write_range_begin` walker 升级跨级权限 + `kernel/fs/file.c` VFS bounce 层。

---

## 3. 校验原语 `syscall_check_user_range(addr, len, writable)` → bool

```c
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);
```

四重检查，任一失败 → false（handler 转 `-EFAULT`）：
1. `addr != 0` 且 `len > 0`（`len == 0` → true，空操作）
2. **`addr >= USER_MIN_ADDR`**（= `USER_CODE_ADDR` 0x400000，trap.c:765/task.c:1048；栈 0x800000 task.h:352，其下 0x600000 guard 未映射，**0x400000 之下无合法映射**；**`do_mmap` 也强制 `USER_MIN_ADDR`**——MAP_FIXED 低于 0x400000 拒绝，保持"其下永未映射"不变量）
3. `addr < current->addr_limit` 且 `len <= addr_limit - addr`（溢出安全）
4. `arch_user_range_accessible(current->mm->pml4, addr, len, writable)` 逐页有效权限

**架构钩子（跨级有效权限）**：`arch_user_range_accessible(void *pgtbl, uint64_t addr, uint64_t len, bool writable) → bool`（mmu.h），复用 `arch_virt_to_phys` 走查骨架，**每级累积**：

```
perms = { present=true, user=true, rw=true }
for level in pml4 → pdp → pd → pt:
    if !entry.present: return false            // 缺页
    perms.user &= entry.U_S;  perms.rw &= entry.RW
    if huge page (pd:2MB / pdp:1GB): break
return perms.user && (!writable || perms.rw)
```

- x86_64：present=bit0/RW=bit1/U/S=bit2 跨级累积。OS01 只创建 4KB+2MB 页（vmm.c 走 `pml2[...]&PAGE_2M_MASK`）→ 处理两粒度；1GB 防御性 false
- aarch64：stub false（fail-closed）
- **COW/RW-clear 页 writable=true → false**（内核直写 COW PTE 不触发用户拷贝机制 → #PF）
- 参数化 `pgtbl`：正常传 `current->mm->pml4`；selftest 传合成表

**与 `user_write_range_begin` 的关系**：后者的 walker（vma.c:535-548）升级为调用/内联 `arch_user_range_accessible`（修复其只查叶 PTE 的 P0-1 缺口），仍持 `mm->lock` 供非阻塞写调用方用。

**地位**：`check_user_range` 是早期快速拒绝 + 语义过滤；**崩溃保证由 `_ft`/bounce 兜底**（§4/§7），走查器 bug 最坏 `-EFAULT` 不崩。

---

## 4. 容错拷贝原语 `copy_to_user_ft` / `copy_from_user_ft` / `strnlen_user`

```c
ssize_t copy_to_user_ft(void *dst, const void *src, size_t n);    // n==0 → 0
ssize_t copy_from_user_ft(void *dst, const void *src, size_t n);  // n==0 → 0
int    strnlen_user(const void *user_addr, size_t max);           // ≤max，遇 NUL 即停；fault → -EFAULT
```

GCC `__builtin_setjmp`/`__builtin_longjmp`（零手写汇编，规避 setjmp-frame-pointer-bug）：

```c
ssize_t copy_to_user_ft(void *dst, const void *src, size_t n) {
    if (n == 0) return 0;
    __builtin_setjmp_buffer jb;
    jmp_buf_t *old = current->fault_jmp;
    current->fault_jmp = (jmp_buf_t *)&jb;
    if (__builtin_setjmp(jb) == 0) {
        memcpy(dst, src, n);
        current->fault_jmp = old;
        return (ssize_t)n;
    }
    current->fault_jmp = old;
    return -EFAULT;
}
```

`copy_from_user_ft` 同构；`strnlen_user` 同构（arm fault_jmp → 逐字节扫到 NUL/max → 解除；fault → `-EFAULT`）。

**`do_page_fault` 挂钩**（trap.c:429 内核态分支顶部）：

```c
if (cr2 < current->addr_limit && current->fault_jmp)
    __builtin_longjmp(current->fault_jmp, 1);
```

**关键守卫**：`cr2 < addr_limit` 门控——只重定向用户区间 fault；真内核 bug 仍走 dump。`current`（RSP 推导，task.h:285-291）+ #PF 用 IST 0（trap.c:2519）→ 解析为 faulting task；**等价性以 Step 0 实证**（§12）。

**`task_t` 变更**：+`void *fault_jmp`（**结构体变更 → `make clean`**）。

**生命周期纪律**：武装↔解除之间无 schedule。fault 返回恰 `-EFAULT`，**无短计数**；调用方将 dst 视为垃圾。

**🚫 硬约束（不可违反）**：`_ft`/`strnlen_user` 的 longjmp 会**绕过普通返回路径**，因此**禁止在以下上下文使用**：
1. 持有 spinlock（含 `bucket->lock`、`mm->lock`、各种 wq lock 等）——longjmp 跳过 unlock → 永久死锁
2. IRQ critical section / `local_irq_disable` 段——跳过恢复 → IRQ 永久关
3. 持有必须按序释放的嵌套锁/资源（如 VFS 锁链）——跳过中间释放 → 锁序破坏
需要持锁读用户内存的场合（如 futex 持 `bucket->lock` 读 futex 值），**改用内核直映射读**（`user_va_to_phys`+`Phy_To_Virt`，见矩阵 #47）或先校验再读，不用 `_ft`。

---

## 5. 逐 syscall 审计矩阵（71 项全覆盖）

> 列：**#** syscall / 用户参数(方向) / 长度来源 / 可阻塞 / 最终解引用函数 / 方案 / 失败回滚 / 测试#
> 方案：**D**=无指针不处理；**A**=strnlen_user+strdup；**B**=定长拷内核+bounce+`_ft`；**C**=大缓冲 bounce+`_ft` 分块；**R**=沿用 user_write_range（lock）

| # | syscall | 用户参数(方向) | 长度 | 阻塞 | 最终解引用 | 方案 | 失败回滚 | 测试 |
|---|---------|---------------|------|------|-----------|------|---------|------|
| 0 | putchar | — | — | N | — | D | — | — |
| 1 | write | buf(读) | rdx | **Y**(pipe/socket) | fd_write→vfs_write / pipe_write / netconn_write | C：入口check + `copy_from_user_ft` 分块 bounce，喂内核给 VFS/lwIP | fault→返回已写字节（offset 只在成功后推进） | H1 |
| 2 | exit | — | — | N | — | D | — | — |
| 3 | brk | — | — | N | — | D | — | — |
| 4 | getpid | — | — | N | — | D | — | — |
| 5 | exec | path(读)+argv/envp(读) | 无界→VFS_NAME_MAX / 三上限 | N(建新 mm) | sys_exec argv/envp 遍历 + `str_offset[128]` | A(strnlen_user+strdup) + **A' argv/envp 有界深拷贝**（MAX_ARGV=128/STRLEN=4096/TOTAL=65536，超→-E2BIG；**建新 pml4 前完成**） | 深拷贝失败→-EFAULT/-E2BIG，旧空间未动 | H8 |
| 6 | read | buf(写) | rdx | **Y**(tty/pipe/socket) | fd_read→vfs_read / pipe_read / netconn_recv | C：入口check + 底层写内核 bounce + `copy_to_user_ft` 回写；分块 offset 成功后推进 | fault→返回已提交字节，offset 停在 fault 分块前（数据不丢） | H1 |
| 7 | open | path(读) | 无界 | N | strdup + **O_CREAT 分支旧指针** | A + §5.2 修 O_CREAT | — | H3 |
| 8 | close | — | — | N | — | D | — | — |
| 9 | dup | — | — | N | — | D | — | — |
| 10 | dup2 | — | — | N | — | D | — | — |
| 11 | fork | — | — | N | — | D | — | — |
| 12 | **waitpid** | status(写) | 固定 4 | **Y** | do_waitpid `*user_status`（task.c:993，schedule 后） | **B**：入口 `check_user_range(status,4,true)`（NULL 合法）+ 最终 `copy_to_user_ft` | fault→child 已收割（阻塞 wait 已发生），返回 -EFAULT（child 不泄漏，不崩） | H6 |
| 13 | **pipe** | fds[2](写) | 固定 8 | N | do_pipe `memcpy(user_fds,...)`（file.c:766） | **B**：`copy_to_user_ft` 写回 | **fault→只 `fd_close(rfd)+fd_close(wfd)`（file.c:758-761 同款）——最后 close 经 `file_free` 自动 `pipe_free`（file.c:64），不显式 `pipe_free`，否则 double-free** | H10 |
| 14 | chdir | path(读) | 无界 | N | strdup + **kfree 后 path[0]** | A + §5.2 修 path[0] | — | H3 |
| 15 | getcwd | buf(写) | strlen(cwd)+1 | N | `memcpy(buf,cwd,len)`（trap.c:1491） | B：内核拷 → `copy_to_user_ft` | — | H4 |
| 16 | stat | path(读)+buf(写) | 固定 stat | N | vfs_stat→**用户 buf**（trap.c:1515） | A(path) + B：内核 struct stat → `copy_to_user_ft` | — | H4 |
| 17 | fstat | buf(写) | 固定 stat | N | vfs_stat→用户 buf | B：内核 struct → `_ft` | — | H4 |
| 18 | lseek | — | — | N | — | D | — | — |
| 19 | fcntl | —（全 int cmd） | — | N | 无指针 deref | D（核实：OS01 只 F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL，全 int） | — | — |
| 20 | ioctl | arg(读写混合，per-req) | per-request | N(TCSETS 等) | tty_ioctl termios（tty.c:314-321）/TIOCSPGRP pgrp（tty.c:333-349）/pty termios（file.c:765） | **B**：per-request bounce——TCGETS/TIOCGPGRP 写方向 `_ft`；TCSETS/TIOCSPGRP 读方向 `copy_from_user_ft` 拷内核再操作 | — | H9 |
| 21 | getdents64 | buf(写) | count | N | vfs_getdents→**用户 buf**（vfs.c:102 memcpy） | **B**：内核 bounce（count 夹紧到 UACCESS_BOUNCE_SIZE）→ `copy_to_user_ft`；offset 成功后推进 | fault→已提交字节 | H4 |
| 22 | access | path(读) | 无界 | N | vfs_lookup | A | — | H3 |
| 23 | unlink | path(读) | 无界 | N | vfs | A | — | H3 |
| 24 | mkdir | path(读) | 无界 | N | vfs | A | — | H3 |
| 25 | rmdir | path(读) | 无界 | N | vfs | A | — | H3 |
| 26 | rename | path×2(读) | 无界 | N | vfs | A | — | H3 |
| 27 | truncate | path(读) | 无界 | N | vfs | A | — | H3 |
| 28 | ftruncate | — | — | N | — | D | — | — |
| 29 | time | tloc(写，**可 NULL**) | 固定 8 | N | `*tloc=0`（trap.c:1875） | B：NULL 合法，`_ft` 写 | — | H4 |
| 30 | gettimeofday | tv/tz(写，可 NULL) | 固定 | N | 字段写（trap.c:1884-1891） | B：NULL 合法，`_ft` | — | H4 |
| 31 | nanosleep | req(读)+rem(写，可 NULL) | 固定 | **Y** | req 字段（trap.c:2032）+ **rem 字段 blocker_wait 后**（2052） | B：req `copy_from_user_ft` 拷内核；rem 可 NULL，**阻塞后 `copy_to_user_ft`** | — | H2 |
| 32 | chmod | —（**stub**） | — | N | 无 deref（trap.c:2062） | **D**（stub，无路径解引用） | — | — |
| 33 | fchmod | —（stub） | — | N | — | D | — | — |
| 34 | times | buf(写，可 NULL) | 固定 tms | N | `memset(buf,...)`（trap.c:2076） | B：NULL 合法，`_ft` | — | H4 |
| 35 | uname | buf(写) | 固定 utsname | N | `memset`+`strcpy`（trap.c:2088-2093） | B：内核 struct → `_ft`（strcpy 不再直写用户） | — | H4 |
| 36 | getppid | — | — | N | — | D | — | — |
| 37 | umask | — | — | N | — | D | — | — |
| 38 | kill | —（ints） | — | N | — | D | — | — |
| 39 | signal | act(读)+oldact(写，可 NULL) | 固定 | N | 字段读写（trap.c:2175-2191） | B：act `copy_from_user_ft` 拷内核；oldact `_ft` 回写 | — | H5 |
| 40 | sync | — | — | N | — | D | — | — |
| 41 | reboot | —（int） | — | N | — | D | — | — |
| 42 | sigprocmask | set(读)+oldset(写，可 NULL) | 固定 | N | `*set`/`*oldset`（trap.c:2208,2219） | B：`copy_from_user_ft` + `_ft` | — | H5 |
| 43 | sigreturn | 用户帧(读) | 固定 sigframe | N | user_va_to_phys + Phy_To_Virt 读帧（trap.c:2238-2259） | **特殊**：自上下文帧，CS 校验 ring-3（2244），直 map 读不越页 fault（物理连续），用户只能破坏自己上下文 → 不崩 | — | — |
| 44 | mmap | —（range） | length | N | do_mmap（vma.c:527 已有范围校验） | D + **do_mmap 强制 USER_MIN_ADDR** | — | — |
| 45 | mprotect | —（range） | length | N | do_mprotect（vma.c:306-308） | D（已有校验） | — | — |
| 46 | munmap | —（range） | length | N | do_munmap（vma.c:273） | D（已有校验） | — | — |
| 47 | futex | uaddr(读写) | 固定 4 | **Y**(WAIT) | do_futex_wait/wake（futex.c:48,97） | **B'（特殊）**：入口 `check_user_range(uaddr,4,rw)`+`USER_MIN_ADDR` 一致性；**持 `bucket->lock` 内的值读取保持现状**——已是 `user_va_to_phys`+`Phy_To_Virt` **内核直映射读**（不可能 #PF，futex.c:40-49），且 **WAIT 无阻塞后重读**（wake 后只清 wq，futex.c:58-66）→ **不用 `_ft`**（禁止持锁用 `_ft`，§4 硬约束） | — | H7 |
| 48 | poll | fds[](读写) | nfds | **Y** | do_poll（poll.c:481 现只查起点） | B：入口 `check_user_range` + pollfd 数组拷内核 + 回写 `_ft` | — | H7 |
| 49 | ppoll | —（**ENOSYS**） | — | N | — | D（stub） | — | — |
| 50 | select | fd_set×3+timeout(读写) | nfds | **Y** | do_select（select.c:124-376 已有 range） | B：统一 `check_user_range` + 回写 `_ft` | — | H7 |
| 51 | pselect6 | 同上+sigmask | — | **Y** | do_pselect6 | B：同上 | — | H7 |
| 52 | socket | —（ints） | — | N | — | D | — | — |
| 53 | bind | sockaddr(读) | 固定 | N | memcpy 内核栈（trap.c:2444，已 range） | B：统一 `check_user_range`（行为不变） | — | H7 |
| 54 | connect | sockaddr(读) | 固定 | **Y** | memcpy 内核栈（trap.c:2377） | B：统一 `check_user_range`（已拷内核，无后续用户 deref） | — | H7 |
| 55 | listen | —（int） | — | N | — | D | — | — |
| 56 | accept | —（`do_accept(fd,NULL,NULL)`） | — | **Y** | 无用户 deref（简化实现） | **D**（无 deref；真 sockaddr 出参是 P5 工作） | — | — |
| 57 | sendto | buf(读)+sockaddr(读) | rdx/r9 | **Y** | netconn_write 用户 buf + sockaddr 已拷内核 | C：**分块 bounce** `copy_from_user_ft`→内核 ≤16KB→netconn_write_partly（lwIP 不触用户内存）；sockaddr 沿用拷内核 | fault→已写字节 | H7 |
| 58 | recvfrom | buf(写)+sockaddr/len(写) | rdx | **Y** | netconn_recv→用户 buf + **do_recvfrom 后 addr/len 写回**（trap.c:2431,2433） | C：数据 `_ft` 回写 + **写回 `_ft`**（post-block） | sockaddr 写 fault→返回已收字节（ret≥0）addr 不填，不崩 | H7 |
| 59 | setsockopt | optval(读) | r8 | N | do_setsockopt | B：`copy_from_user_ft` 拷内核（现 range） | — | H7 |
| 60 | getsockopt | optval+optlen(写) | — | N | do_getsockopt | B：`_ft` 回写 | — | H7 |
| 61 | getsockname | sockaddr+len(写) | 固定 | N | do_getsockname | B：`_ft` 回写 | — | H7 |
| 62 | getpeername | —（**无 case，未实现**） | — | N | — | D（未实现） | — | — |
| 63 | getifaddr | —（无参） | — | N | do_getifaddr() 无参 | D（无用户 arg） | — | — |
| 64 | shutdown | —（int） | — | N | — | D | — | — |
| 65 | clock_gettime | ts(写) | 固定 | N | 字段写（trap.c:1911-1912） | B：`_ft` 写 | — | H4 |
| 66 | getrandom | buf(写) | rsi | N | **user_write_range_begin**（vma.c:522） | **R**：沿用 lock 方案，walker 升级跨级权限；devfs random 读（devfs.c:131）同 | lock 内 fault 不可能（持锁防 munmap）；`_ft` 不涉及 | H4 |
| 67 | setpgid | —（ints） | — | N | — | D | — | — |
| 68 | getpgid | —（ints） | — | N | — | D | — | — |
| 69 | setsid | —（ints） | — | N | — | D | — | — |
| 70 | getsid | —（ints） | — | N | — | D | — | — |

**矩阵统计**（71 项，stat 双角色 A+B 重叠一次）：**D=33**（含 chmod/fchmod/ppoll stub、accept 简化无 deref、sigreturn 特殊、getpeername 未实现）、**A=10**（路径字符串，含 stat 的 path 侧）、**A'=1**（exec argv/envp 深拷贝）、**B=24**（定长结构体/out-buffer，含 stat 的 buf 侧）、**C=4**（read/write/sendto/recvfrom 大缓冲）、**R=1**（getrandom 沿用 user_write_range）。**post-block 用户触点 7 处**：waitpid status、nanosleep rem、pipe read、pipe write、socket rx、socket tx、recvfrom addr 写回——全部 `_ft`/bounce；另有 poll/select 阻塞后 fd_set 回写。

### 5.2 旧指针替换清单（strdup 后窗口仍在）

| 位置 | 现状 | 替换为 |
|------|------|--------|
| chdir（trap.c:1446-1458） | `kfree(path_copy)` 后用原始 `path[0]`/`strlen(path)`/`memcpy(path)` | 用 `path_copy`，释放移到所有使用之后 |
| open O_CREAT（trap.c:1280-1282） | `strlen(path)`+`memcpy(pbuf,path,...)` 用原始用户 path | 用 `path_copy` |
| 全部 Cat A | 无 strnlen_user 定界（无界扫描） | 统一 strnlen_user 定界 + strdup |

---

## 6. 错误码、边界与回滚语义

- 校验失败 / `_ft` fault → `-EFAULT`（统一）；`strnlen_user` fault → `-EFAULT`；超 `VFS_NAME_MAX` → `-ENAMETOOLONG`（同 trap.c:1280）
- **`_ft` 无短计数**：fault 恰 `-EFAULT`，不返回部分正计数；调用方将 dst 视为垃圾
- **分块 syscall 级短读写**（read/write/getdents64/sendto/recvfrom）：`UACCESS_BOUNCE_SIZE`（64KB）分块，**offset 只在 `_ft` 拷贝成功后推进**。**fault 语义统一**：`submitted == 0`（第一块即 fault）→ 返回 `-EFAULT`（**非 0**，避免与 EOF 混淆）；`submitted > 0` → 返回已提交短计数，offset 停在 fault 分块前（**数据不丢、可重试**）。`_ft` 本身无短计数，分块层提供 syscall 级短计数
- **waitpid status fault**：child 已收割（阻塞 wait 已发生），返回 `-EFAULT`；child 不泄漏、不崩（接受"wait 已消费"语义）
- **pipe 写 fds fault**：只 `fd_close(rfd)+fd_close(wfd)`（最后 close 自动 `pipe_free`，file.c:64），不显式 `pipe_free`（double-free）
- **recvfrom sockaddr 写 fault**：返回已收字节（ret≥0），addr 不填
- `len == 0` → 成功 no-op；`NULL` 合法处（time/gettimeofday/times/sigprocmask/signal/nanosleep rem）保持合法
- 不改变任何正常路径返回码（184/0 systest 原样通过）

---

## 7. 数据流（VFS bounce 可执行模型）

### 7.1 read 大缓冲（阻塞 + 同步统一）

```
SYS_read: check_user_range(buf, size, true)
  └─ fd_read 分块循环（BOUNCE=64KB）：
       n = vfs_read(node, offset, min(remaining, BOUNCE), kbuf)   // FS 写内核 bounce
       if copy_to_user_ft(user+off, kbuf, n) < 0 →
           submitted==0 ? -EFAULT : 返回已提交字节（offset 未推进，数据不丢）
       offset += n; off += n
   tty/pipe/socket：底层写内核 bounce 后同样 `_ft` 回写（§5.1 触点）
```
**关键**：FS 回调永不解引用用户指针（一律写 kbuf）；用户写只在 `_ft` 一处。fault 不会发生在 FS 回调内部。

### 7.2 write 大缓冲

```
SYS_write: check_user_range(buf, size, false)
  └─ fd_read 对称：copy_from_user_ft(kbuf, user+off, chunk) → vfs_write(node, offset, chunk, kbuf)
       fault → submitted==0 ? -EFAULT : 返回已写字节（offset 未推进）
   pipe/socket：copy_from_user_ft 搬内核 bounce → pipe/lwIP（§5.1）
```

### 7.3 getdents64 / stat / fstat

```
getdents64: 内核 bounce(count 夹到 BOUNCE) ← vfs_getdents → copy_to_user_ft 回写
stat/fstat: 内核 struct stat ← vfs_stat → copy_to_user_ft 回写
```

### 7.4 fault path 全链

```
敌意指针 → check_user_range（USER_MIN_ADDR/addr_limit/溢出/跨级权限）→ -EFAULT（多数情形在此拦下）
漏网（未映射但 in-range / 阻塞期被 munmap）→ copy_*_ft → #PF → cr2<addr_limit && fault_jmp
  → do_page_fault longjmp → -EFAULT（或分块层返回已提交字节）
```

---

## 8. 测试

### 8.1 kernel selftest（`kernel/test/test_uaccess.c`，~90 行）

**上下文**：boot 期 init_task（addr_limit=内核高半区，main.c:306），无用户映射。因此：
- **走查器直接测 `arch_user_range_accessible(pgtbl, ...)`**（非 `check_user_range`——后者硬编码 `current->mm`），pgtbl 传 **selftest 自建合成 pml4**：

| 合成页表组合 | 断言 |
|------|------|
| 未映射 | false |
| present+user+RW | read/write 均 true |
| present+user+RO（COW 模拟） | read true、**write false** |
| **上层 pml4/pdp U/S=0 但叶 U/S=1** | **false**（跨级累积） |
| **上层 RW=0 但叶 RW=1，writable** | **false**（跨级累积） |
| 2MB 大页 present+user+RW | true |
| 部分映射（跨 2 页第二页未映射） | false |

- **地址数学**（`check_user_range`，无需映射）：NULL→false、0x1→false、≥addr_limit→false、len 溢出→false、len==0→true
- **longjmp 路径**：**临时覆盖 `current->addr_limit` 为用户值** → `copy_to_user_ft(低地址未映射, ksrc, 16)` → #PF（cr2<临时 addr_limit）→ longjmp → `-EFAULT` → 恢复。不依赖用户映射
- `copy_to_user_ft` 跨 2 页第二页未映射 → `-EFAULT` **非部分正计数**；`copy_from_user_ft`/`strnlen_user`（页尾 NUL/无 NUL/fault）同构

### 8.2 systest hostile 组（`user/systest.c`，~100 行，~15 断言）

**真实用户路径 E2E**（真实 mm 的 user task，fork 子进程跑、父断言子正常退出内核存活）：

| 用例 | 断言 |
|------|------|
| `read(fd,0x1,16)` / `write(fd,0x1,16)` | -EFAULT |
| `open(NULL,0)` / `chdir(0x1000)` / `access(NULL,...)` | -EFAULT |
| `read(fd,buf,1<<40)` | -EFAULT |
| munmap 后 read/write 该区 | -EFAULT |
| **`waitpid` 传坏 status**（子先退） | -EFAULT 不崩（H6） |
| **`pipe` 传坏 fds**（0x1/未映射） | -EFAULT **且无 fd/pipe 泄漏**（fd 计数回查）（H10） |
| **nanosleep 阻塞中被信号打断 → rem 写**（E2E 时序） | -EFAULT/EINTR 不崩 |
| **阻塞 pipe 读中信号 handler munmap 缓冲** | 不崩（`_ft` 兜底） |
| exec 坏 argv 元素 / 无 NUL 字符串 | -EFAULT（深拷贝） |
| 路径页尾 NUL、下一页未映射 | 正常 open（不过度拒绝） |
| ioctl 坏 arg（TCGETS 到 0x1） | -EFAULT（H9） |
| poll/select 坏 fd_set/超长 nfds | -EFAULT（H7） |

### 8.3 回归

- 184/0 systest 全量原样通过；网络 harness；QEMU ash
- applet user-fault 复查（`docs/applet-verification.md` §💥：sum 已脱险，**nl 仍** user-fault 崩溃，纯 libc 缺陷则出范围）

---

## 9. 文件清单与工作量（≈700-800 行，2-3 天）

| 文件 | 变更 |
|------|------|
| 新 `kernel/memory/uaccess.c` | 三原语 + `strnlen_user` + `check_user_range` + `UACCESS_BOUNCE_SIZE` 分块辅助（~220 行） |
| 新 `kernel/include/kernel/uaccess.h` | 声明 + `USER_MIN_ADDR`/`MAX_ARGV`/`MAX_ARG_STRLEN`/`MAX_ARG_TOTAL`/`UACCESS_BOUNCE_SIZE`（~60 行） |
| `kernel/include/kernel/arch/mmu.h` | +`arch_user_range_accessible`（跨级累积，x86 4KB+2MB ~40 行 + aarch64 stub） |
| `kernel/memory/vma.c` | `user_write_range_begin` walker 升级跨级权限（复用/内联 `arch_user_range_accessible`）；`do_mmap` 强制 `USER_MIN_ADDR`（~25 行） |
| `kernel/include/kernel/task.h` | +`fault_jmp`（**`make clean`**） |
| `kernel/arch/x86_64/trap.c` | `do_page_fault` 挂钩 + ~30 handler 接入 + SYS_exec argv/envp 深拷贝 + chdir/open O_CREAT 修（~150 行） |
| `kernel/sched/task.c` | `do_waitpid` status `_ft` 写回 + `sys_exec` 深拷贝后 argv/envp（~30 行） |
| `kernel/fs/file.c` | **VFS bounce 分块层**（fd_read/fd_write/getdents 重构）+ pipe r/w bounce + socket rx `_ft`/tx 分块 + **do_pipe 回滚**（~90 行） |
| `kernel/tty/tty.c` | tty_read bounce + `_ft`（~10 行） |
| `kernel/fs/select.c` | fd_set 回写 `_ft`（~4 行） |
| `kernel/sync/futex.c` | 仅入口 `check_user_range`+`USER_MIN_ADDR` 一致性（保留内核直映射读，**不加 `_ft`**）~6 行 |
| 新 `kernel/test/test_uaccess.c` | 合成页表 + 临时 addr_limit + 地址数学（~90 行） |
| `user/systest.c` | hostile 组（~100 行） |
| `docs/syscall.md` + `docs/roadmap.md` | 文档同步 |

---

## 10. 非目标（YAGNI）

- 不做 SMAP/SMEP、exception table/get_user_pages/partial-copy 短计数、aarch64 uaccess（stub fail-closed）
- **不改 VFS/FS 回调签名**（保持 fs read/write 写内核缓冲的约定，bounce 在 fd_read/fd_write 层）
- **不重构 lwIP**（socket 用 bounce 分块隔离）
- **不实现 getpeername/accept 出参**（D 类，P5 工作）
- 不碰内存泄露/越界写的完整审计（「内存安全」层次，本次 DoS 目标）

---

## 11. 风险与边界

| 风险 | 缓解 |
|------|------|
| `__builtin_setjmp` freestanding 行为 | 单测实测；异常再评估手写 asm 备选 |
| longjmp 跨 #PF 帧 | 中断帧残留栈 RSP 下无害；`cr2<addr_limit` 门控 |
| `task_t` 变更 | **`make clean && make`** |
| `current==task_from_tss()` | **Step 0 实证**，不等则钩子用 `task_from_tss()` |
| 信号 handler munmap 竞态 | 阻塞触点 `_ft` 兜底；Step 0 验证"信号只在返回用户态投递"（entry.S check_signal ~92-102） |
| `_ft` 期间 schedule() 合规性 | 纪律（原语内无调度点）+ cr2 门控 + Step 0 |
| **走查器 vs `_ft` 关系** | 所有用户解引用走 bounce/`_ft`；走查器漏判最坏 `-EFAULT` 不崩 |
| **VFS bounce 分块** | offset 仅在 `_ft` 成功后推进 → fault 数据不丢；`UACCESS_BOUNCE_SIZE=64KB` 内核栈外（kmalloc） |
| **do_pipe 回滚** | `_ft` 写失败 → 只 `fd_close×2`（最后 close 经 file_free 自动 `pipe_free`，file.c:64）；**不显式 `pipe_free`**（double-free） |
| **futex 持锁读** | 持 `bucket->lock` 内**不用 `_ft`**（§4 硬约束：longjmp 绕过 unlock → 死锁）；保持 `user_va_to_phys`+`Phy_To_Virt` 内核直映射读（不可能 #PF）。仅入口补 `check_user_range`+`USER_MIN_ADDR` |
| USER_MIN_ADDR 布局耦合 | `do_mmap` 强制同约束保不变量；走查器仍逐页验证 |
| exec argv/envp 上限 | 超 → `-E2BIG`；深拷贝在旧空间释放前完成 |
| socket TX lwIP 中间态 | 分块 bounce 首选（lwIP 不触用户内存）；setjmp 包 netconn_write 否决（pbuf 泄漏） |
| 误伤内核 bug 被吞 | cr2 门控 + 184/0 回归 |
| 性能 | 走查≤len/4096 页循环 + 每拷贝一次 setjmp（~几十 ns）；非热路径 |

---

## 12. 实施顺序（依赖驱动）

```
Step 0: 语义验证（无代码）——① 信号只在返回用户态投递（entry.S check_signal ~92-102）；
        ② current == task_from_tss()（do_page_fault IST 0 中临时打印实证）；不等则钩子用 task_from_tss()
Step 1: 基础设施 — uaccess.h + mmu.h arch_user_range_accessible(跨级,4KB+2MB) + task.h fault_jmp
        + vma.c user_write_range walker 升级 + do_mmap USER_MIN_ADDR 强制
Step 2: 原语 + do_page_fault 挂钩（cr2 门控）
Step 3: kernel selftest test_uaccess（合成页表测 walker + 临时 addr_limit 测 longjmp）→ 原语门禁
Step 4: Cat A 路径字符串（strnlen_user+strdup 推广）+ chdir/open O_CREAT 旧指针替换
Step 5: Cat A' exec argv/envp 有界深拷贝（trap.c + task.c sys_exec）
Step 6: Cat B 定长结构体/out-buffer（waitpid/pipe/ioctl/time/nanosleep/signal/sigprocmask/
        uname/getdents/stat/fstat/getcwd/clock_gettime/poll/select/socket 族 → 全 _ft/bounce；
        futex 仅入口 check_user_range，持锁读保持内核直映射，不加 _ft）
Step 7: Cat C 大缓冲 VFS bounce 分块层（fd_read/fd_write/socket rx/tx）+ 阻塞触点 + do_pipe 回滚
Step 8: systest hostile 组 + 回归（184/0 + 网络 + ash）+ applet user-fault 复查
Step 9: 文档同步（syscall.md / roadmap.md）
```

依赖：Step 0/3 先于 4-7；Step 4/5/6 独立可并行（各自 commit）；Step 7 依赖 2/3（`_ft` 就绪）。

**Commit message 模板**（含 make clean 警示）：
```
fix(uaccess): syscall boundary DoS hardening — <step 主题>

- <改动要点：如 "add cross-level arch_user_range_accessible walker">
- ⚠️ task.h struct 变更 → 必须 `make clean && make`（AGENTS.md，旧 .o 静默崩）
- 验证：kernel selftest test_uaccess / systest 184/0 hostile 组
```

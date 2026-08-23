# OS01 syscall 边界审计设计文档

- 日期：2026-08-23
- 状态：v3 待用户 review（尚未进入实现）
- 目标：**DoS 防护** —— 任何 syscall 收到敌意用户指针（NULL / 低地址 / 内核区间 / 未映射 / 超长 size 越界）返回 `-EFAULT`，**永不导致内核崩溃**；顺带关闭路径字符串的 TOCTOU 主窗口。roadmap P1「syscall 边界审计」项。
- 修订记录：
  - v1：初版（brainstorming 产出，方案 A：PTE 走查校验 + TOCTOU strdup + 窄容错拷贝原语）
  - v2：并入首轮 review——Step 0、`current->fault_jmp` 统一、arch_pte_flags 4KB+2MB、Cat A 列表 ✅ 标注、无短计数测试、applet 名单、`_ft` schedule 风险条、commit 模板
  - v3：并入二轮 review（4 个 P0 + 2 个澄清）——
    - 🟢 §3 **有效权限跨级累积**：走查器改为 `arch_user_range_accessible(pgtbl,addr,len,rw)`，跨 pml4→pdp→pd→pt 累积 `present && U/S && RW`，非只查叶 PTE（x86 各级表项 AND 语义）
    - 🟢 §3 加 **USER_MIN_ADDR**（=USER_CODE_ADDR 0x400000，最低用户映射，task.h:352 栈 0x800000 之下全未映射），修 §7.2 示例矛盾
    - 🟢 §5 **exec argv/envp 有界深拷贝**（数组项数/每项长度/总字节三上限）+ `stat(path)` 补入 Cat A + chdir/open O_CREAT 旧指针替换
    - 🟢 §5 路径方案改 **fault-tolerant `strnlen_user`**（逐字节容错读，遇 NUL 即停，只拷实际长度），不整段校验 VFS_NAME_MAX（防页尾 NUL 合法路径被过度拒绝）
    - 🟢 §4/§5 **所有用户内存拷贝走 `_ft`**（写侧权威兜底，走查器作为早期快速拒绝 + 语义过滤，走查器 bug 也只导致 -EFAULT 非崩溃）
    - 🟢 §8.1 selftest 改**合成页表**测走查器 + **临时 addr_limit 覆盖**测 longjmp 路径（boot 上下文无用户映射的事实约束）
    - 🟢 §12 Step 0 加 `current == task_from_tss()` 实证前置条件

---

## 1. 背景与动机

### 1.1 威胁现状

OS01 是扁平地址空间（**未使能 SMAP/SMEP**，head.S/trampoline.S 只设标准 CR4 位），内核直接 `memcpy`/`strdup`/逐字节操作用户指针。**没有任何 `copy_from_user`/`copy_to_user` 抽象**（roadmap 的"copy_from_user 全路径核查"是目标术语，不是现状）。

**关键事实：内核态 #PF = 内核崩溃。** `do_page_fault`（`kernel/arch/x86_64/trap.c:423`）对 `!(regs->cs & 3)`（内核态 fault，trap.c:429）直接打全寄存器 dump + TASKLIST dump 后停摆。因此任何用户进程传坏指针触发内核态 #PF，就是整机 DoS。busybox applet 吃用户输入，是现实攻击面（`docs/applet-verification.md` §「💥 崩溃」：nl 仍 user-fault 崩溃）。

### 1.2 根因（审计前置探查确认的现状）

现存边界校验**三种形态、不统一、有漏洞**：

| 形态 | 位置 | 缺陷 |
|------|------|------|
| **仅查起点** `(uint64_t)ptr >= addr_limit` | trap.c:1145(write) / 1240(read) / 1254(open) / 1202-1212(exec)；futex.c:48,97；file.c:766；poll.c:481 | **放行 NULL 和低地址**（0 < addr_limit 通过）→ 内核态 #PF → 崩溃；不查长度越界 |
| **完整范围查** `syscall_user_range_ok`（trap.c:948：`addr!=0 && addr<limit && len<=limit-addr`） | **只有 socket 系列在用**（trap.c:2373-2464）；select.c:124-376 有类似 range 检查；tty.c:333-346；vma.c:273,306-308,527-528；task.c:992 | 但**未映射 in-range 地址仍能过**（只查地址 < limit，不查页表）→ 内核态 #PF → 崩溃 |
| **完全不查** | 大量 handler 直接解引用用户指针 | 同 NULL 漏洞 |

**范围检查 ≠ 映射检查**：`addr < addr_limit` 只证明地址在用户区间，不证明页面存在/可写。`arch_virt_to_phys`（`kernel/include/kernel/arch/mmu.h:38`）能走查 4 级页表返回物理地址或 0，但**只返回叶 PTE 的物理地址，不解释权限，也不累积跨级有效权限**——正是要补的。

**x86 有效权限是跨级 AND 组合**：CPU 对 `pml4→pdp→pd→pt` 每一级的 `present / U/S / RW` 位做有效权限 AND。任一级 U/S=0 → 超级用户页（用户访问被拒）；任一级 RW=0 → 写被拒。**只查叶 PTE 会漏掉上层清权限的情况**（上层 U/S=0 但叶 U/S=1，内核直 memcpy 仍可访问——错误地允许了内核访问只读/超级用户页）。

**TOCTOU 现状**：只有 exec（trap.c:1218）、open（trap.c:1263）、stat（trap.c:1500）、chdir（trap.c:1433）对路径 `strdup` 到内核堆；**但 chdir 在 `kfree(path_copy)` 后仍用原始 `path[0]`**（trap.c:1425 附近）、**open 的 O_CREAT 分支仍用 `strlen(path)`/`memcpy(pbuf,path,...)`**（trap.c:1280-1282）——strdup 后 TOCTOU/fault 窗口仍在。其余路径 syscall（unlink/mkdir/rmdir/rename/truncate/access/chmod）**在阻塞操作中直接解引用用户路径**。且现有 `strdup(path)` **无界**——path 无 NUL 终止会扫过页边界 → fault。

**exec 的 argv/envp 深解引用**（task.c:1353-1373）：`while (argv[s_argc] != NULL) s_argc++;` **无界遍历指针数组**，每个元素 `strlen(argv[i])` + `memcpy` **无界扫用户字符串**；`str_offset[128]` 固定数组（>128 项 → 内核栈溢出隐患）。当前 trap.c:1207-1215 只校验数组**首地址**，坏元素指针或无 NUL 元素 → 内核 #PF。**深拷贝必须在切换/释放旧地址空间前完成**。

**阻塞读写的用户缓冲触点在阻塞后**（信号 handler 可在阻塞期间 munmap 缓冲 → 唤醒后写/读用户内存 → #PF）：
- 读侧（写用户缓冲）：`tty_read` 排水循环（tty.c，`schedule()` 后）；`pipe_read_internal` 字节循环 `dst[total++]`（file.c:399）；socket RX `memcpy(buf,data,copy)`（file.c:525,543，`netconn_recv` 可阻塞）
- 写侧（读用户缓冲）：`pipe_write_internal` 字节循环 `src[total++]`（file.c:570，`schedule()` 后）；socket TX `netconn_write(s->conn, buf,...)`（lwIP 内部拷贝用户指针，可阻塞）
- select/poll 反向映射写回用户 fd_set（select.c，`do_poll_core` 可阻塞后）

**同步路径确认**：VFS/ext2/fat/tmpfs/devfs 读是 **AHCI busy-wait 同步读**（ahci.c:67「busy-wait with jiffies timeout」），无 schedule → 无调度点在拷贝中间；但为统一与兜底，**所有用户内存拷贝仍走 `_ft`**（见 §5 纪律）。

### 1.3 目标层次（已确认）

**DoS 防护**（brainstorming 用户确认），非 Linux 级 uaccess：
- 敌意指针**永不**导致内核态 #PF
- 校验失败一律 `-EFAULT`（与现有 socket/select 一致）
- 不做：SMAP/SMEP 使能、get_user_pages、异常表 fixup 全套、partial-copy 短计数语义

---

## 2. 总体架构

新增 uaccess 层，syscall 全部走它：

```
用户指针 ──→ syscall_check_user_range(addr,len,rw)   // 快速早期拒绝 + 语义过滤（第一道防线）
         ──→ strnlen_user(addr,max)                  // 字符串：逐字节容错读，遇 NUL 即停
         ──→ copy_to_user_ft / copy_from_user_ft     // 容错拷贝（一切用户内存拷贝的权威兜底，第二道防线）
```

**核心纪律**：
1. 校验后立即拷贝/使用，二者之间**不得有调度点**；
2. **一切用户内存拷贝（含同步 out-buffer）都走 `_ft` 变体**——走查器是早期快速拒绝 + 语义过滤，`_ft` 是权威兜底（即使走查器漏掉上层权限位，`_ft` 的 fault 恢复也返回 `-EFAULT` 而非崩溃）；
3. 一切**可能阻塞后触用户内存**的地方用 `_ft` 变体兜底（信号 handler munmap 竞态）。

新增文件：
- `kernel/memory/uaccess.c`（~180 行）：三原语 + `strnlen_user`
- `kernel/include/kernel/uaccess.h`（~50 行）：声明 + arch 钩子

---

## 3. 校验原语 `syscall_check_user_range(addr, len, writable)` → bool

```c
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);
```

四重检查，任一失败返回 false（handler 转 `-EFAULT`）：

1. `addr != 0` 且 `len > 0`（`len == 0` → 直接 true，Linux 空操作约定）
2. **`addr >= USER_MIN_ADDR`** —— 挡 NULL/低地址。`USER_MIN_ADDR = USER_CODE_ADDR = 0x400000`（trap.c:765 / task.c:1048），用户最低映射是代码基址；用户栈在 0x800000（task.h:352），其下 0x600000 为栈 guard 未映射，**0x400000 之下无任何合法用户映射** → 一切低地址直接拒绝（含 `0x1`），与 §7.2 示例一致
3. `addr < current->addr_limit` 且 `len <= current->addr_limit - addr` —— 溢出安全，挡内核区间 + 超长 size 越过边界（`addr_limit` 定义 task.h:131，用户态 0x00007FFFFFFFFFFF）
4. 逐页走查 `[addr, addr+len)` 的**有效权限**：`arch_user_range_accessible(current->mm->pml4, addr, len, writable)`。任一页有效权限不满足 → false

**架构钩子（跨级有效权限，核心修正）**：`arch_user_range_accessible(void *pgtbl, uint64_t addr, uint64_t len, bool writable) → bool`（`kernel/include/kernel/arch/mmu.h`），复用 `arch_virt_to_phys` 的 4 级走查骨架，但**在每一级累积有效权限**：

```
walk(pgtbl, va, writable):
    perms = { present=true, user=true, rw=true }   // 初始全允许
    for level in pml4 → pdp → pd → pt:
        if !entry.present:      return UNMAPPED        // 缺页
        perms.present &= true
        perms.user     &= entry.U_S                    // 任一级 U/S=0 → 超级用户页
        perms.rw       &= entry.RW                     // 任一级 RW=0 → 写保护
        if huge page (pd: 2MB / pdp: 1GB): break       // 大页在此级终结
    return perms.present && perms.user && (!writable || perms.rw)
```

- x86_64 实现：present=bit0、RW=bit1、U/S=bit2，按上述跨级累积。**OS01 只创建 4KB 页和 2MB 大页**（vmm.c 映射走 `pml2[...] = phys & PAGE_2M_MASK`，无 1GB 页创建；`PAGE_1G_SHIFT` 仅用于索引计算）→ 处理 4KB + 2MB 两粒度，1GB 分支防御性返回 UNMAPPED（不创建即不可达）
- aarch64：stub 返回 false（**fail-closed**）——YAGNI，不写 ARM 代码（roadmap P2 前不动）
- 参数化 `pgtbl`：正常路径传 `current->mm->pml4`；selftest 传**合成页表**（见 §8.1），不依赖当前任务 mm

**COW 页特例（必须拒绝）**：writable=true 时，有效 RW=0（含 COW/RW-clear，`PAGE_COW` bit10 或 RW 清除）→ **拒绝**——内核直写 COW/RO PTE 不会触发用户态拷贝机制，会直接 #PF。内核不参与用户 COW 语义。

**错误路径**：任何 false → handler `regs->rax = -EFAULT`。

**地位说明**：`check_user_range` 是**早期快速拒绝 + 语义过滤**（廉价拦截 NULL/0x1/内核区间/明显越界/权限不符），**不是崩溃保证的唯一来源**——真正的保证由 `_ft` 拷贝提供（§4）。走查器若有 bug（漏上层权限），最坏是 `_ft` 兜底返回 `-EFAULT`，**不会崩溃**。

---

## 4. 容错拷贝原语 `copy_to_user_ft` / `copy_from_user_ft` / `strnlen_user`

```c
ssize_t copy_to_user_ft(void *dst, const void *src, size_t n);    // n==0 → 0
ssize_t copy_from_user_ft(void *dst, const void *src, size_t n);  // n==0 → 0
int    strnlen_user(const void *user_addr, size_t max);           // ≤max，遇 NUL 即停；fault → -EFAULT
```

**机制**：GCC `__builtin_setjmp`/`__builtin_longjmp`（编译器 builtin，零手写汇编，**规避 memory 记录的 `setjmp-frame-pointer-bug`**——那是 libc setjmp 用 `[RBP+8]` 读返回地址，builtin 由编译器直接发射不碰帧指针约定；内核已 `-ffreestanding`，builtin 可用）。

```c
ssize_t copy_to_user_ft(void *dst, const void *src, size_t n) {
    if (n == 0) return 0;
    __builtin_setjmp_buffer jb;
    jmp_buf_t *old = current->fault_jmp;            // 统一用 current->fault_jmp
    current->fault_jmp = (jmp_buf_t *)&jb;
    if (__builtin_setjmp(jb) == 0) {
        memcpy(dst, src, n);
        current->fault_jmp = old;
        return (ssize_t)n;
    }
    current->fault_jmp = old;                       // fault 路径
    return -EFAULT;
}
```

`copy_from_user_ft` 同构（memcpy 参数对调）。`strnlen_user` 同构（arm fault_jmp → 逐字节扫到 NUL/max → 解除；fault → `-EFAULT`）。

**字段访问一致性**：copy_ft 与 `do_page_fault` 钩子**都统一用 `current->fault_jmp`**（非 `task->`）。`current` = `get_current_task()` = `rsp & ~(STACK_SIZE-1)`（task.h:285-291），基于当前内核栈推导；#PF 用 IST 0（`set_trap_gate(14,0,page_fault)`，trap.c:2519）跑在任务内核栈上 → `current` 正确解析为 faulting task，与 dump 用的 `task_from_tss()` 一致。**但 `current == task_from_tss()` 的等价性以 Step 0 实证为前置条件**（§12 Step 0），不凭注释断言。

**`do_page_fault` 挂钩**（trap.c:429 内核态分支**顶部**，panic dump 之前）：

```c
if (cr2 < current->addr_limit && current->fault_jmp)
    __builtin_longjmp(current->fault_jmp, 1);
```

**关键守卫**：`cr2 < addr_limit` 门控——只重定向用户区间 fault（本原语的拷贝目标/源）；真内核 bug（cr2 ≥ addr_limit）仍走原有 dump，绝不吞掉。

**`task_t` 变更**：`kernel/include/kernel/task.h` 加 `void *fault_jmp` 字段（**结构体变更 → 必须 `make clean`**，AGENTS.md）。

**生命周期纪律**：武装到解除之间**无 schedule**（纯 memcpy/字节拷贝，无锁竞态、无中断态不一致）→ 栈纪律安全；单线程（无 clone，P5 才有）无并发解映射者。longjmp 后中断帧残留在内核栈 RSP 之下（无害，下次使用覆盖）。

**错误语义**：fault → 整个操作 `-EFAULT`（**不做** Linux 短计数；调用方将 dst 视为垃圾）。`strnlen_user` fault → `-EFAULT`（调用方按错误处理）。

---

## 5. 站点接入策略（55 个指针解引用 handler → 4 类）

| 类 | 站点 | 处理 |
|----|------|------|
| **A 路径字符串** | open ✅(trap.c:1249) / exec ✅(1194) / chdir ✅(1422) / unlink ✅(1741) / mkdir ✅(1760) / rmdir ✅(1781) / rename ✅(1800) / truncate ✅(1828) / access ✅(1698) / chmod ✅(2061) / **stat ✅(1494，路径输入，补入)** —— 全部 `grep "case SYS_"` 核实存在 | `strnlen_user(path, VFS_NAME_MAX)`（**逐字节容错读，遇 NUL 即停**，只拷实际长度；内核无 PATH_MAX，用 `VFS_NAME_MAX`=256，vfs.h:12，超限 → `-ENAMETOOLONG` 同 trap.c:1280）→ `strdup` 到内核堆 → **后续全部操作内核副本**。**不整段校验 VFS_NAME_MAX 范围**（防"字符串在页尾 NUL 结束、下一页未映射"的合法路径被过度拒绝） |
| **A' exec argv/envp 有界深拷贝** | task.c:1353-1373（无界遍历 + 每元素 strlen/memcpy + `str_offset[128]` 隐患） | **三上限**：数组项数 ≤ `MAX_ARGV`（128）、每项长度 ≤ `MAX_ARG_STRLEN`（4096）、总字节 ≤ `MAX_ARG_TOTAL`（65536）。trap.c SYS_exec handler 在 path strdup 后、进入 sys_exec 前完成：`strnlen_user` 读每项长度（fault → -EFAULT）→ `copy_from_user_ft` 拷每项到内核堆 → 深拷贝后的内核 argv/envp 传给 sys_exec。**必须在切换/释放旧地址空间前完成**（sys_exec 内建新 pml4 前） |
| **B 定长结构体 + 同步 out-buffer** | timespec / sockaddr / fd_set / sigset / stat.buf / optval / addrlen / getcwd / getdents64 等 | `check_user_range`（按方向 rw，语义过滤）→ **拷贝一律走 `copy_from_user_ft`/`copy_to_user_ft`**（非裸 memcpy——`_ft` 是权威兜底）。统一 select.c:124-376、socket trap.c:2373-2464 已有的零散 range 检查到本原语（行为不变，只统一入口 + 拷贝原语） |
| **C 大缓冲 in-place** | read / write fd 数据 | read: 入口 `check_user_range(buf,size,true)` → 最终写用户缓冲一律 `copy_to_user_ft`（VFS 同步路径也走，统一纪律）。write: 入口 `check_user_range(buf,size,false)` → pipe/socket 阻塞后读用户缓冲用 `copy_from_user_ft` |
| **D 无指针** | fd 号 / 纯整数值（getpid/kill/waitpid/brk/setpgid…） | 不动 |

### 5.1 阻塞读写最终触点明细（`_ft` 接线点）

| 触点 | 位置 | 变体 |
|------|------|------|
| `tty_read` 排水 | tty.c | 内核 **bounce buffer**（≤4096B）先搬 ring/canon 到内核，再单次 `copy_to_user_ft` → fault 不丢已消费数据、原子 |
| `pipe_read_internal` 字节循环 | file.c:399 `dst[total++]` | 同上 bounce buffer（PIPE_SIZE≤4096 天然适配），循环内先写内核，尾部单次 `copy_to_user_ft` |
| `pipe_write_internal` 字节循环 | file.c:570 `src[total++]` | 先 `copy_from_user_ft` 从用户搬 PIPE_SIZE 进内核 bounce，再逐字节入 pipe |
| socket RX `memcpy(buf,data,copy)` | file.c:525,543 | 直接 `copy_to_user_ft`（数据已在内核 netbuf，一次性 memcpy；fault 丢该 netbuf 数据，可重试，可接受） |
| socket TX `netconn_write(s->conn,buf,...)` | file.c（FD_SOCKET 分支） | **首选**：分块 bounce（`copy_from_user_ft` 到内核缓冲 ≤16KB → `netconn_write_partly` 喂内核缓冲）→ lwIP 永不触用户内存，无 lwIP 中间态风险。**备选**：setjmp 包整次 `netconn_write`（简单，但 fault 长跳会使 lwIP pbuf 链半填充泄漏）。spec 决策：**首选分块 bounce** |
| select/poll fd_set 写回 | select.c 反向映射 | `copy_to_user_ft`（`do_poll_core` 阻塞后） |

### 5.2 旧指针替换清单（strdup 后 TOCTOU/fault 窗口仍存在的点，必须替换为内核副本）

| 位置 | 现状 | 替换为 |
|------|------|--------|
| chdir（trap.c:1425 附近） | `kfree(path_copy)` 后仍用原始 `path[0]` 判断绝对/相对路径 | 用 `path_copy[0]`；path_copy 的释放移到所有使用之后 |
| open O_CREAT 分支（trap.c:1280-1282） | `strlen(path)` + `memcpy(pbuf, path, ...)` 用原始用户 path | 用 `path_copy`（已在 handler 开头 strdup） |
| 其余路径 syscall（unlink/mkdir/rmdir/rename/truncate/access/chmod） | 直接 `strdup` 后即用，但无 strnlen_user 定界（无界扫描窗口） | 统一走 §5 Cat A 流程 |

---

## 6. 错误码与边界语义

- 校验失败 / `_ft` fault → `-EFAULT`（统一；与现有 socket/select 一致）
- **`_ft` 无短计数语义**：fault 时返回恰为 `-EFAULT`，**绝不返回部分正计数**；即使 fault 前已拷贝部分字节，返回值仍是 `-EFAULT`，调用方将 dst 视为垃圾（不承诺 dst 未被污染——memcpy 中途 fault 时前段字节确实已写入）
- `strnlen_user` fault → `-EFAULT`；超 `VFS_NAME_MAX` → `-ENAMETOOLONG`（同 trap.c:1280 现有语义）
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
SYS_read 入口: check_user_range(buf, len, true) ──通过──► fd_read
  ├─ VFS: vfs_read 同步 → copy_to_user_ft(buf, ...)         （统一走 _ft）
  ├─ pipe: 阻塞 → 唤醒 → bounce 内核 → copy_to_user_ft(buf)
  └─ socket: netconn_recv 阻塞 → copy_to_user_ft(buf, data, n)
```

### 7.2 fault path（敌意指针）

`read(fd, 0x1, 4)`：
```
check_user_range(0x1, 4, true) → 0x1 < USER_MIN_ADDR(0x400000) → -EFAULT（不触内核内存）
```

`read(fd, valid_start, 1<<40)`（size 越界）：
```
len(1<<40) > addr_limit - addr → -EFAULT
```

`read(fd, in_range_unmapped, 4)`（页未映射）：
```
check_user_range 走查 → 无 present → -EFAULT（即使走查有漏，copy_to_user_ft 也会兜底）
```

阻塞读中信号 handler munmap 缓冲 → 唤醒后写：
```
copy_to_user_ft 武装 setjmp → memcpy → #PF (cr2 < addr_limit && fault_jmp)
  → do_page_fault 挂钩 __builtin_longjmp → 原语返回 -EFAULT
```

---

## 8. 测试

### 8.1 kernel selftest（新 `kernel/test/test_uaccess.c`，~80 行）

**上下文事实**：`selftest_run_all()` 在 boot 期、init_task（addr_limit=内核高半区 0xffff800000000000）上下文运行（main.c:306），**没有当前任务的用户映射**。因此：

- **走查器单元测试针对合成页表**：`arch_user_range_accessible(pgtbl, ...)` 的 `pgtbl` 参数化——selftest 自建一个小型 scratch pml4（分配页 → 铺 pml4→pdp→pd→pt，构造已知 U/S/RW 组合），**不依赖当前任务 mm**：

| 合成页表组合 | 断言 `check_user_range(addr,len,writable)` |
|------|------|
| 未映射地址 | false |
| present + user + RW | read=true, write=true |
| present + user + RO（模拟 COW，RW 清） | read=true, **write=false** |
| 上层 pml4/pdp U/S=0 但叶 U/S=1 | **false**（跨级累积：上层超级用户页） |
| 上层 RW=0 但叶 RW=1，writable=true | **false**（跨级累积） |
| 2MB 大页（pd 级 present+user+RW） | true |
| 部分映射（range 跨 2 页，第二页未映射） | false |

- **地址数学检查**（无需映射）：`NULL`→false、`0x1`→false（USER_MIN_ADDR）、`≥addr_limit`→false、`len 溢出`→false、`len==0`→true

- **longjmp 路径**（boot 上下文可确定性触发）：**临时覆盖 `current->addr_limit` 为用户值** → arm fault_jmp → `copy_to_user_ft(低地址未映射, ksrc, 16)` → #PF（cr2 < 临时 addr_limit）→ longjmp → 断言 `-EFAULT` → 恢复 addr_limit。**不依赖用户映射，只依赖 fault 重定向机制**

| 用例 | 断言 |
|------|------|
| `copy_to_user_ft(userbuf, ksrc, n)` 正常 | n |
| `copy_to_user_ft(低地址未映射, ksrc, 16)`（临时 addr_limit 覆盖） | `-EFAULT`（**longjmp 路径**） |
| `copy_to_user_ft(跨 2 页目标，第二页未映射，n=8192)` | `-EFAULT`，**绝不返回部分正计数**（无短计数语义） |
| `copy_from_user_ft` 对称 + `strnlen_user`（页尾 NUL / 无 NUL / fault） | 同上 |

### 8.2 systest hostile 组（`user/systest.c`，~80 行，~10-15 断言）

**真实用户路径 E2E**（有真实映射的 user task）：fork 子进程跑敌意用例，父 waitpid 断言子正常退出（非信号）、内核存活。覆盖走查器 + `_ft` 在真实用户 mm 下的行为：

| 用例 | 断言 |
|------|------|
| `read(fd, (void*)0x1, 16)` | -EFAULT |
| `write(fd, (void*)0x1, 16)` | -EFAULT |
| `open(NULL, 0)` / `chdir((void*)0x1000)` | -EFAULT |
| `read(fd, buf, 1<<40)` | -EFAULT |
| `munmap` 一块区域后 `read(fd, 那块区域, 16)` | -EFAULT |
| `write(fd, 未映射指针, 16)` | -EFAULT |
| 阻塞 pipe 读中（子进程延时写）信号 handler munmap 缓冲 → 唤醒后写 | 不崩（`_ft` 兜底 → -EFAULT/EINTR） |
| exec 传坏 argv 元素指针 / 无 NUL 字符串 | -EFAULT（有界深拷贝） |
| 路径在页尾 NUL 结束、下一页未映射 | 正常 open（不过度拒绝） |

### 8.3 回归

- 184/0 systest 全量原样通过
- 网络 harness（make test-network）+ QEMU ash 手工回归
- **复查 applet user-fault 崩溃**（`docs/applet-verification.md` §「💥 崩溃（内核 user-fault 杀任务）」）：原 4 处 cut/nl/expand/sum，2026-08-22 重测 **sum 已脱险（printf 修复），nl 仍在同一类 libc 缺陷上 user-fault 崩溃**（固定 RIP，指向 libc 内部函数）。本审计若发现这些是坏指针触发内核 #PF 同源，修完即愈——逐处确认；若 nl 是纯 libc 内部缺陷（非坏指针），则不属于本审计范围，保持 ❌ 记录

---

## 9. 文件清单与工作量（≈500-600 行，2 天）

| 文件 | 变更 |
|------|------|
| 新 `kernel/memory/uaccess.c` | 三原语 + `strnlen_user` + `check_user_range`（~180 行） |
| 新 `kernel/include/kernel/uaccess.h` | 声明 + `USER_MIN_ADDR`/`MAX_ARGV`/`MAX_ARG_STRLEN`/`MAX_ARG_TOTAL` 常量（~50 行） |
| `kernel/include/kernel/arch/mmu.h` | +`arch_user_range_accessible`（跨级有效权限累积，x86 4KB+2MB ~40 行 + aarch64 stub） |
| `kernel/include/kernel/task.h` | +`fault_jmp`（**`make clean`**） |
| `kernel/arch/x86_64/trap.c` | `do_page_fault` 挂钩 ~12 行 + ~30 handler 接入 + **SYS_exec argv/envp 有界深拷贝** + chdir/open O_CREAT 旧指针替换 |
| `kernel/sched/task.c` | `sys_exec` 签名/调用改为内核深拷贝后的 argv/envp（~20 行） |
| `kernel/fs/file.c` | pipe r+w bounce + socket rx `_ft` / tx 分块 bounce ~30 行 |
| `kernel/tty/tty.c` | tty_read bounce + `copy_to_user_ft` ~10 行 |
| `kernel/fs/select.c` | fd_set 写回 `_ft` ~4 行 |
| `kernel/sync/futex.c` | 统一到 `check_user_range` ~6 行 |
| 新 `kernel/test/test_uaccess.c` | 合成页表 + 临时 addr_limit + 地址数学（~80 行） |
| `user/systest.c` | hostile 组 ~80 行 |
| `docs/syscall.md` + `docs/roadmap.md` | 文档同步（P1 项标记完成） |

---

## 10. 非目标（YAGNI）

- **不做 SMAP/SMEP 使能**（真硬件防线，独立项；无 uaccess 抽象时的硬件兜底，后续单独评）
- **不做 Linux 式 exception table / get_user_pages / partial-copy 短计数**（DoS 目标不需要；`__builtin_setjmp` 已覆盖 fault 恢复）
- **不做 `copy_from_user`/`copy_to_user` 全站替换为裸校验**（大缓冲 in-place 保持直接操作，但最终触用户内存一律 `_ft`）
- **不修 getdents64/stat 等同步 out-buffer 的潜在竞态**（统一走 `_ft`，无调度点，安全）
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
| `current == task_from_tss()` 等价性 | **Step 0 实证**（非注释断言）：#PF 入口打印两者，确认一致；若 IST 0 语义与假设不符，改走查器/钩子用 `task_from_tss()` |
| 信号 handler munmap 竞态 | 阻塞触点 `_ft` 兜底；**依赖"信号只在返回用户态时投递"语义**（handler 不能在 memcpy 中途跑）——**Step 0 验证此语义成立**（entry.S `check_signal` 块，~92-102：`arch_do_signal_delivery` 只在返回用户态路径调用） |
| **`_ft` 武装期间 schedule() 合规性** | 纪律强制"武装↔解除之间无 schedule"（纯 memcpy/字节拷贝）；若违反，`fault_jmp` 跨调度残留——`jb` 是栈上局部，任务迁移/栈复用后失效 → 后续任意用户区间 #PF 误 longjmp 到失效缓冲 → 未定义行为。缓解：① 纪律（原语内部无调度点）② `cr2 < addr_limit` 门控 ③ Step 0 信号只在返回用户态投递 |
| **走查器与 `_ft` 的关系** | 走查器是早期拒绝 + 语义过滤，**正确性保证由 `_ft` 提供**：所有用户内存拷贝走 `_ft`，走查器漏判最多导致 `_ft` 兜底 `-EFAULT`，不会崩溃。同步 out-buffer 也不例外（§5 纪律 2） |
| **USER_MIN_ADDR 与用户布局耦合** | `USER_MIN_ADDR = USER_CODE_ADDR = 0x400000` 依赖"0x400000 之下无合法映射"（代码基址 + 0x800000 栈 + 0x600000 guard）。若未来引入低地址映射（vsyscall 等），须同步更新。走查器仍会逐页验证，USER_MIN_ADDR 只是快速预过滤 |
| **exec argv/envp 有界深拷贝上限** | `MAX_ARGV=128`/`MAX_ARG_STRLEN=4096`/`MAX_ARG_TOTAL=65536` 超出 → `-E2BIG`（Linux 同语义）。深拷贝在旧地址空间释放前完成（sys_exec 建新 pml4 前），无 UAF |
| socket TX lwIP 中间态 | 首选分块 bounce（lwIP 永不触用户内存）；备选 setjmp 包 netconn_write 有 pbuf 泄漏（已否决为主方案） |
| 误伤：内核 bug 被 `_ft` 吞 | `cr2 < addr_limit` 门控 + 单测覆盖正常路径返回码不变（184/0 回归） |
| 性能 | 每 syscall 一次走查（≤len/4096 次循环，通常 1-2 页）+ 每拷贝一次 setjmp（~几十 ns）可忽略；`_ft` 统一后无裸 memcpy 热路径 |

---

## 12. 实施顺序（依赖驱动）

```
Step 0: 语义验证（无代码）——
        ① "信号只在返回用户态时投递"：读 kernel/arch/x86_64/entry.S check_signal 块
           （~92-102），arch_do_signal_delivery 只在返回用户态路径调用 → 阻塞读被
           信号打断先 EINTR/处理 handler，唤醒后的用户缓冲写由 _ft 兜底，无中间态。
        ② "current == task_from_tss()"：do_page_fault（IST 0，trap.c:2519 set_trap_gate
           第 2 参=IST index=0 → 任务内核栈）中临时打印两者，确认等价；若不等，钩子改用
           task_from_tss()。此步是 _ft 正确性 + 钩子正确性的前提。
Step 1: 基础设施 — uaccess.h + mmu.h arch_user_range_accessible(跨级累积,4KB+2MB) +
        task.h fault_jmp
Step 2: 原语 + do_page_fault 挂钩（含 cr2 门控）
Step 3: kernel selftest test_uaccess.c（合成页表 + 临时 addr_limit + 地址数学）→ 先于一切站点接入
Step 4: Cat A 路径字符串（strnlen_user + strdup 推广）+ chdir/open O_CREAT 旧指针替换
Step 5: Cat A' exec argv/envp 有界深拷贝（trap.c handler + task.c sys_exec 改造）
Step 6: Cat B 定长结构体统一（全走 _ft）
Step 7: Cat C 大缓冲 + 阻塞触点 _ft（pipe/tty/socket rx/tx/select）
Step 8: systest hostile 组 + 回归（184/0 + 网络 + ash）+ applet user-fault 复查
Step 9: 文档同步（syscall.md / roadmap.md）
```

依赖：Step 0/3 必须先于 4-7（语义前提 + 原语正确性门禁）；Step 4/5/6 相互独立可并行（各自独立 commit，Step 5 涉及 task.c 需单独验证 exec）。

**Commit message 模板**（含 make clean 警示，防旧 .o 静默崩）：

```
fix(uaccess): syscall boundary DoS hardening — <step 主题>

- <改动要点：如 "add arch_user_range_accessible cross-level permission walk">
- ⚠️ task.h struct 变更 → 必须 `make clean && make`（AGENTS.md，旧 .o 静默崩）
- 验证：kernel selftest test_uaccess / systest 184/0 hostile 组
```

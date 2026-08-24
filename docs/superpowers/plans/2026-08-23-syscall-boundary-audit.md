# OS01 syscall 边界审计（DoS 防护）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 使任何 syscall 收到敌意用户指针（NULL / 低地址 / 内核区间 / 未映射 / 超长 size 越界）一律返回 `-EFAULT`，永不导致内核态 #PF 崩溃；顺带关闭路径字符串 TOCTOU 主窗口；信号投递写栈与 sigreturn 读帧跨页正确。

**Architecture:** 新增 uaccess 层三机制分工——① `syscall_check_user_range`/`arch_user_range_accessible`（跨级有效权限走查，早期拒绝+语义过滤）；② `copy_to_user_ft`/`copy_from_user_ft`/`strnlen_user`（Clang `__builtin_setjmp`/`longjmp` 容错拷贝，权威兜底）；③ `user_write_range_begin/end` 沿用 lock 方案（getrandom/devfs 非阻塞写）。所有用户内存解引用（含信号投递写栈、sigreturn 读帧）一律经这三机制，不允许裸解引用。`do_page_fault` 内核态分支加 `cr2<addr_limit && fault_jmp` 长跳挂钩。VFS 读写在 fd_read/fd_write 层分块 bounce（FS 回调永不解引用用户指针，offset 只在 `_ft` 成功后推进）。

**Tech Stack:** Clang `-target x86_64-unknown-none`，`-ffreestanding -fno-stack-protector`，`-O2`(release)/`-O0`(DEBUG=1)。QEMU 验证（KERNEL_SELFTEST / systest）。内核 `-O2` 默认，`make DEBUG=1` 用 `-O0`。

**Spec:** `docs/superpowers/specs/2026-08-23-syscall-boundary-audit-design.md`（v8 + 测试补强，已获批）

## Global Constraints

- **编译器是 Clang，非 GCC**；`__builtin_longjmp` 的值参数**必须是编译期常量**（内核 hook 恒用 `1`）
- setjmp buffer 类型：`typedef void *os01_jmp_buf[8]`（64B，已验证 -O2/-O0 编译通过）
- `task.h` 结构体变更（`fault_jmp`）→ **必须 `make clean && make`**，旧 .o 静默崩（AGENTS.md）
- `_ft`/`strnlen_user` **禁止在持有 spinlock / IRQ critical section / 持有必须解锁的资源时使用**（longjmp 绕过解锁 → 死锁）。**例外**：`_ft` **res 变体**（`copy_*_ft_res`）通过 `fault_cleanup` 回调在 fault 路径显式释放资源/预约，可用于需要跨用户拷贝持有内核状态（如 pipe `read_busy` 预约）的场景
- **所有用户内存解引用一律经 uaccess 原语**（`_ft`/`strnlen_user`/`copy_from_user_ft`）；**禁止裸 `memcpy`/`strdup` 读写用户指针**（fault_jmp 未 armed 时 #PF → PF-KRN panic）
- `syscall_check_user_range` **对 `!current->mm || !current->mm->pml4` fail-closed**（检查顺序：`len==0`→true → 算术拒绝 → mm 检查 → walker；boot 上下文 `init_mm.pml4` 未设置，selftest 依赖此 fail-closed）
- **pipe 读端并发：内核强制的 `read_busy` 预约序列化**（非调用者契约）——`fork`/`dup` 共享读端 + 默认 SMP 下两个独立 task 可能并发读；`read_busy` 预约 + `_ft` fault 路径 cleanup 释放，杜绝 tail 竞态
- 用户布局：`USER_MIN_ADDR = USER_CODE_ADDR = 0x400000`；栈 0x800000（2MB），0x600000 guard 未映射
- 路径长度用 `VFS_NAME_MAX`（256，vfs.h:12），内核无 `PATH_MAX`
- exec argv/envp 三上限：`MAX_ARGV=128` / `MAX_ARG_STRLEN=4096` / `MAX_ARG_TOTAL=65536`
- `UACCESS_BOUNCE_SIZE = 64KB`（kmalloc，非栈上）
- 分块层语义：`submitted == 0` → `-EFAULT`（非 0）；`submitted > 0` → 短计数
- 验证命令：内核侧 `make clean && make`（debug: `make clean && make DEBUG=1`）+ QEMU `KERNEL_SELFTEST`；用户侧 `make` + QEMU systest / busybox

---

## Task 0: Step 0 语义验证（无代码，实现前置）

**Files:** 无（只读验证）
**Interfaces:** 无

- [ ] **Step 1: 语义验证（无代码）——确认"信号只在返回用户态时投递"**
  读 `kernel/arch/x86_64/entry.S` `check_signal` 块（~92-102）：确认 `arch_do_signal_delivery` 只在 `ret_from_intr`/`do_resched` 返回用户态路径调用（`RESTORE_ALL` 前）。结论记录：信号 handler 不可能在 `_ft` 的 memcpy 中途运行（投递需 iret 回用户态）。
  验证：`sed -n '80,115p' kernel/arch/x86_64/entry.S`

- [ ] **Step 2: 验证 `current == task_from_tss()`（一次性、抓串口证据，不进回归）**
  在 `do_page_fault` 内核态分支（trap.c:429）临时加：`serial_printk("PF-VERIFY: current=%p tss=%p eq=%d\n", current, task_from_tss(), current==task_from_tss());`（DEBUG 构建）。
  **⚠️ 注意：此刻（Task 2 之前）内核态 #PF 仍会 panic/halt**——触发方式是用 QEMU 跑一次故意坏指针 `read(fd, 0x1, 4)`（systest 临时用例或 QEMU 内手工），内核 dump 前打印 `PF-VERIFY`，从**串口输出抓证据**后内核 halt。这是**一次性、仅抓证据的 QEMU 运行**，不是正常回归命令；跑完删掉临时 print 与临时用例。
  若 `eq=1`：钩子用 `current->fault_jmp`；若 `eq=0`：Task 2 钩子改用 `task_from_tss()->fault_jmp`。
  验证：`make clean && make DEBUG=1` + 一次性 QEMU，串口 grep `PF-VERIFY`

- [ ] **Step 3: 确认 selftest 上下文**
  确认 `selftest_run_all()`（main.c:306）在 init_task（addr_limit=内核高半区）上下文跑，无用户映射 → 决定 Task 3 的测试策略（合成 pml4 + 临时 addr_limit 覆盖 + 当前 pml4 受控映射）。

- [ ] **Step 4: 记录验证结论（无空提交）**
  把 Step 1-3 的结论（信号投递时机、`current==task_from_tss` 实证结果、selftest 上下文）追加到 spec §12 Step 0 段或本 plan Task 0 注释，随 Task 1 的 commit 一并提交（不单独空提交）。

---

## Task 1: uaccess 基础设施（uaccess.h + 跨级走查器 + task.h + vma.c + do_mmap）

**Files:**
- Create: `kernel/include/kernel/uaccess.h`
- Modify: `kernel/include/kernel/arch/mmu.h`（+`arch_user_range_accessible`）
- Modify: `kernel/include/kernel/task.h`（+`fault_jmp` 字段）
- Modify: `kernel/memory/vma.c`（`user_write_range_begin` walker 升级 + `do_mmap` USER_MIN_ADDR）

**Interfaces:**
- Produces:
  - `#define USER_MIN_ADDR 0x400000UL`、`#define UACCESS_BOUNCE_SIZE (64*1024)`、`#define MAX_ARGV 128`、`#define MAX_ARG_STRLEN 4096`、`#define MAX_ARG_TOTAL 65536`、`typedef void *os01_jmp_buf[8]`
  - `bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);`（声明）
  - `ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n, void (*on_fault)(void *), void *arg);`、`ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n, void (*on_fault)(void *), void *arg);`（声明，Task 2 实现）
  - `bool arch_user_range_accessible(void *pgtbl, uint64_t addr, uint64_t len, bool writable);`（mmu.h inline）
  - `task_t.fault_jmp`（`void **`）、`task_t.fault_cleanup`（`void (*)(void*)`）、`task_t.fault_cleanup_arg`（`void *`）

- [ ] **Step 1: 写失败测试（编译门）——新建 uaccess.h 头骨架 + task.h 字段**
  先只加 `uaccess.h` 的宏/类型/声明 + `task.h` 的 `void **fault_jmp; void (*fault_cleanup)(void *); void *fault_cleanup_arg;` 字段，不加实现。
  ```bash
  make clean && make
  ```
  预期：编译通过（结构体变更，任务内无需运行，仅确认不破坏构建）。注意 `fault_jmp`/`fault_cleanup`/`fault_cleanup_arg` 未初始化——`memset(tsk, 0, sizeof(task_t))` 的初始化点已置 NULL（task.c:1099 等），无需额外。

- [ ] **Step 2: 实现 `arch_user_range_accessible`（跨级有效权限）**
  在 `kernel/include/kernel/arch/mmu.h`（x86_64 分支）加：

  ```c
  // Cross-level effective-permission walk: true iff every page in [addr,addr+len)
  // is present + user-accessible + (writable? RW set), ANDing perms across
  // pml4->pdp->pd->pt (x86 semantics: any level U/S=0 -> supervisor page,
  // any level RW=0 -> read-only).  4KB + 2MB pages (OS01 creates no 1GB).
  static inline bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                                 uint64_t len, bool writable)
  {
      uint64_t *pml4 = (uint64_t *)pgtbl;
      if (addr + len < addr) return false;      // self-guard: addr+len overflow (not just caller)
      uint64_t end = addr + len;
      for (uint64_t va = addr & ~0xFFFULL; va < end; ) {
          uint64_t l4 = (va >> 39) & 0x1FF;
          if (!(pml4[l4] & 1)) return false;
          bool user = !!(pml4[l4] & 4), rw = !!(pml4[l4] & 2);
          uint64_t *pml3 = (uint64_t *)((pml4[l4] & ~0xFFFULL) + ARCH_PAGE_OFFSET);
          uint64_t l3 = (va >> 30) & 0x1FF;
          if (!(pml3[l3] & 1)) return false;
          user = user && !!(pml3[l3] & 4); rw = rw && !!(pml3[l3] & 2);
          if (pml3[l3] & 0x80) return false;          // 1GB: defensive (not created)
          uint64_t *pml2 = (uint64_t *)((pml3[l3] & ~0xFFFULL) + ARCH_PAGE_OFFSET);
          uint64_t l2 = (va >> 21) & 0x1FF;
          if (!(pml2[l2] & 1)) return false;
          user = user && !!(pml2[l2] & 4); rw = rw && !!(pml2[l2] & 2);
          if (pml2[l2] & 0x80) {                       // 2MB huge page
              if (!user || (writable && !rw)) return false;
              va = (va & ~0x1FFFFFULL) + 0x200000ULL;
              continue;
          }
          uint64_t *pml1 = (uint64_t *)((pml2[l2] & ~0xFFFULL) + ARCH_PAGE_OFFSET);
          uint64_t l1 = (va >> 12) & 0x1FF;
          if (!(pml1[l1] & 1)) return false;
          user = user && !!(pml1[l1] & 4); rw = rw && !!(pml1[l1] & 2);
          if (!user || (writable && !rw)) return false;
          va += 0x1000;
      }
      return true;
  }
  ```
  aarch64 分支加 stub：`static inline bool arch_user_range_accessible(...) { (void)pgtbl;(void)addr;(void)len;(void)writable; return false; }`（fail-closed）。
  若 `PAGE_Present`/`PAGE_R_W`/U/S 宏名不同，用字面位：present=bit0、RW=bit1、U/S=bit2（与 mmu.h 现有 `arch_virt_to_phys` 的 `& 1` 一致）。

- [ ] **Step 3: 升级 `user_write_range_begin`（vma.c:522）用跨级走查器 + USER_MIN_ADDR**
  替换 vma.c:535-548 的叶 PTE 循环为：

  ```c
  int user_write_range_begin(uint64_t addr, size_t len)
  {
      if (current->mm == NULL)
          return 0;
      if (addr == 0 || addr < USER_MIN_ADDR ||
          addr >= current->addr_limit || len > current->addr_limit - addr)
          return -EFAULT;
      spin_lock(&current->mm->lock);
      uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
      if (!arch_user_range_accessible(user_pml4, addr, len, true)) {
          spin_unlock(&current->mm->lock);
          return -EFAULT;
      }
      return 0;   // lock held
  }
  ```
  （保持 `user_write_range_end` 不变。需要 `#include <kernel/uaccess.h>`。行为变化：跨级权限 + USER_MIN_ADDR + 拒 COW——与 spec 一致。）

- [ ] **Step 4: `do_mmap` 强制 `USER_MIN_ADDR`**
  在 `kernel/memory/vma.c` 的 `do_mmap` 入口（addr==0 检查附近，vma.c:527 前）加：
  ```c
  if (addr != 0 && addr < USER_MIN_ADDR)   // MAP_FIXED 或显式地址低于用户最低映射
      return -EINVAL;
  ```
  （保持 vma.c:527 现有 `addr==0 || addr>=addr_limit || len>addr_limit-addr` 检查。）

- [ ] **Step 5: 构建 + 回归**
  ```bash
  make clean && make
  ```
  预期：构建通过。QEMU 启动，KERNEL_SELFTEST 全过（现有 selftest 不受影响），systest 回归（getrandom/devfs 仍工作——`user_write_range_begin` 行为等价）。

- [ ] **Step 6: Commit**
  ```bash
  git add kernel/include/kernel/uaccess.h kernel/include/kernel/arch/mmu.h \
          kernel/include/kernel/task.h kernel/memory/vma.c
  git commit -m "feat(uaccess): cross-level user range walker + USER_MIN_ADDR infra

  - arch_user_range_accessible: cross-level AND of present/U/S/RW (4KB+2MB)
  - user_write_range_begin upgraded to cross-level walker + USER_MIN_ADDR
  - do_mmap enforces USER_MIN_ADDR (keeps 'nothing below 0x400000 mapped')
  - task_t.fault_jmp field; uaccess.h macro/type declarations

  ⚠️ task.h struct change -> make clean && make (AGENTS.md)"
  ```

---

## Task 2: `_ft` 容错拷贝原语 + do_page_fault 挂钩

**Files:**
- Create: `kernel/memory/uaccess.c`
- Modify: `kernel/arch/x86_64/trap.c`（`do_page_fault` 挂钩，trap.c:429 内核态分支顶部）

**Interfaces:**
- Consumes: `os01_jmp_buf`（Task 1）、`task_t.fault_jmp`/`fault_cleanup`/`fault_cleanup_arg`（Task 1）、`USER_MIN_ADDR`（Task 1）、`arch_user_range_accessible`（Task 1）
- Produces: `ssize_t copy_to_user_ft(void*,const void*,size_t)`、`ssize_t copy_from_user_ft(void*,const void*,size_t)`、`int strnlen_user(const void*,size_t)`、`bool syscall_check_user_range(uint64_t,uint64_t,bool)`（实现）、**`ssize_t copy_to_user_ft_res(void*,const void*,size_t,void(*)(void*),void*)`、`ssize_t copy_from_user_ft_res(void*,const void*,size_t,void(*)(void*),void*)`**（on-fault cleanup 变体，Task 8 pipe 预约依赖）

- [ ] **Step 1: 写 `kernel/memory/uaccess.c` 全部原语**

  ```c
  #include <kernel/uaccess.h>
  #include <kernel/task.h>       // current, fault_jmp, addr_limit
  #include <kernel/memory.h>     // Phy_To_Virt
  #include <errno.h>

  // ── Fault-tolerant user copy (Clang __builtin_setjmp/longjmp) ──
  // Longjmp value MUST be a compile-time constant (Clang constraint).
  // do_page_fault redirects user-range #PF to the armed buffer (see trap.c).
  ssize_t copy_to_user_ft(void *dst, const void *src, size_t n)
  {
      if (n == 0) return 0;
      os01_jmp_buf jb;
      void **old = current->fault_jmp;
      current->fault_jmp = (void **)jb;         // array decays to void**
      if (__builtin_setjmp((void **)jb) == 0) {
          __builtin_memcpy(dst, src, n);
          current->fault_jmp = old;
          return (ssize_t)n;
      }
      current->fault_jmp = old;                 // fault path
      return -EFAULT;
  }

  ssize_t copy_from_user_ft(void *dst, const void *src, size_t n)
  {
      if (n == 0) return 0;
      os01_jmp_buf jb;
      void **old = current->fault_jmp;
      current->fault_jmp = (void **)jb;
      if (__builtin_setjmp((void **)jb) == 0) {
          __builtin_memcpy(dst, src, n);
          current->fault_jmp = old;
          return (ssize_t)n;
      }
      current->fault_jmp = old;
      return -EFAULT;
  }

  // ── res variants: on-fault cleanup callback ──────────────────────
  // For holding kernel state across a user copy (e.g. pipe read_busy
  // reservation): the cleanup runs on the LONGJMP path (setjmp != 0
  // branch, back on the original kernel stack) so the reservation/lock
  // is released even when the copy faults.  NULL on_fault == plain _ft.
  ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
  {
      if (n == 0) return 0;
      os01_jmp_buf jb;
      void **old = current->fault_jmp;
      void (*old_cb)(void *) = current->fault_cleanup;
      void *old_arg = current->fault_cleanup_arg;
      current->fault_jmp = (void **)jb;
      current->fault_cleanup = on_fault;
      current->fault_cleanup_arg = arg;
      if (__builtin_setjmp((void **)jb) == 0) {
          __builtin_memcpy(dst, src, n);
          current->fault_jmp = old;
          current->fault_cleanup = old_cb;
          current->fault_cleanup_arg = old_arg;
          return (ssize_t)n;
      }
      if (on_fault) on_fault(arg);          // release reservation on the fault path
      current->fault_jmp = old;
      current->fault_cleanup = old_cb;
      current->fault_cleanup_arg = old_arg;
      return -EFAULT;
  }
  ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                                void (*on_fault)(void *), void *arg)
  { /* identical, memcpy args swapped */ }

  // Bounded fault-tolerant string scan: stop at NUL or max.  Fault -> -EFAULT.
  int strnlen_user(const void *user_addr, size_t max)
  {
      os01_jmp_buf jb;
      void **old = current->fault_jmp;
      current->fault_jmp = (void **)jb;
      if (__builtin_setjmp((void **)jb) == 0) {
          const char *p = (const char *)user_addr;
          size_t i = 0;
          while (i < max && p[i] != '\0') i++;
          current->fault_jmp = old;
          return (int)i;                        // ==max -> caller decides ENAMETOOLONG
      }
      current->fault_jmp = old;
      return -EFAULT;
  }

  // ── Range validation (fast reject + semantic filter; _ft is the authority) ──
  // Order matters: len==0 -> true (no mm needed); arithmetic rejects; then
  // fail-closed on missing mm/pml4 (boot ctx: init_mm.pml4 is unset) so the
  // walker is NEVER invoked with a null table.
  bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
  {
      if (len == 0) return true;
      if (addr == 0 || addr < USER_MIN_ADDR) return false;
      if (addr >= current->addr_limit || len > current->addr_limit - addr)
          return false;
      if (current->mm == NULL || current->mm->pml4 == NULL)
          return false;                       // fail-closed: no user address space
      uint64_t *pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
      return arch_user_range_accessible(pml4, addr, len, writable);
  }
  ```

- [ ] **Step 2: `do_page_fault` 挂钩（trap.c:429 内核态分支顶部，dump 之前）**
  在 `if (!(regs->cs & 3)) {` 后**第一行**加（先读 `cr2`，已在 trap.c:425-426 读取到 `cr2` 局部变量）：
  ```c
  // uaccess _ft recovery: user-range fault while a copy buffer is armed.
  if (cr2 < current->addr_limit && current->fault_jmp)
      __builtin_longjmp(current->fault_jmp, 1);
  ```
  （放在 `serial_printk("PF-KRN: ...")` 之前。`current` 宏 + IST 0 已在 Step 0 实证。）

- [ ] **Step 3: 编译 + 回归**
  ```bash
  make clean && make
  ```
  预期：构建通过；QEMU 启动 selftest/systest 全过（原语未被调用，无行为变化）。

- [ ] **Step 4: Commit**
  ```bash
  git add kernel/memory/uaccess.c kernel/arch/x86_64/trap.c
  git commit -m "feat(uaccess): fault-tolerant copy primitives + do_page_fault hook

  - copy_to_user_ft/copy_from_user_ft/strnlen_user via Clang __builtin_setjmp
    (os01_jmp_buf[8], constant longjmp value 1)
  - syscall_check_user_range: USER_MIN_ADDR + addr_limit + cross-level walk
  - do_page_fault: cr2 < addr_limit && fault_jmp -> __builtin_longjmp(1)"
  ```

---

## Task 3: kernel selftest `test_uaccess`

**Files:**
- Create: `kernel/test/test_uaccess.c` + 注册到 `kernel/test/selftest.c`

**Interfaces:**
- Consumes: `arch_user_range_accessible`（Task 1）、`copy_to_user_ft`/`copy_from_user_ft`/`strnlen_user`/`syscall_check_user_range`（Task 2）、`Phy_To_Virt`/`Virt_To_Phy`/`alloc_pages`（内核内存）
- Produces: selftest 注册函数 `void selftest_uaccess(void);`

- [ ] **Step 1: 写失败测试——合成页表构建 + walker 断言**
  自建 scratch pml4（分配页作 pml4/pdp/pd/pt），构造已知 U/S/RW 组合。**合成页表只用于 `arch_user_range_accessible()` 测试**（walker 接收 `g_pml4` 参数，不依赖当前 CR3）——`copy_*_ft` 走的是**当前 CR3**，不能用合成表（见 Step 2）。

  ```c
  // kernel/test/test_uaccess.c
  #include <kernel/selftest.h>
  #include <kernel/uaccess.h>
  #include <kernel/memory.h>
  #include <kernel/pmm.h>
  #include <kernel/task.h>

  // ── Synthetic pml4 builder (walker-only) ────────────────
  static uint64_t *g_pml4;

  static uint64_t *alloc_page_pml4(void) {
      struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
      return (uint64_t *)Phy_To_Virt(pg->phy_address);
  }
  static uint64_t *pt_for(uint64_t va) { /* create pml4->pdp->pd->pt chain on demand */ }
  static void map_4k(uint64_t va, uint64_t phys, uint32_t flags) { /* walk to pml1 leaf */ }
  static void map_2m(uint64_t va, uint64_t phys, uint32_t flags) { /* walk to pml2 | PAGE_PS */ }
  ```
  （`pt_for`/`map_*` 用 `arch_virt_to_phys` 同款走查索引，参考 mmu.h:38-56 的 l4/l3/l2/l1 计算。）

  断言（走查器直接测，pgtbl 传合成表）：
  | 用例 | `arch_user_range_accessible(g_pml4, ...)` 断言 |
  |---|---|
  | VA 0x500000（未映射） | false |
  | 0x400000 4KB present+user+RW | read/write 均 true |
  | 0x400000 4KB present+user+RO（RW 清） | read true、write false |
  | 0x400000 上层 pml4 U/S=0 但叶 U/S=1 | false |
  | 0x400000 上层 RW=0 但叶 RW=1 | read true、write false |
  | 0x800000 2MB present+user+RW | true |
  | [0x600000, 0x600ff0+32) 跨两 4KB 页 | true |

  ```c
  void selftest_uaccess(void) {
      /* build synthetic pml4 with the combos above, then: */
      SELFTEST_ASSERT(!arch_user_range_accessible(g_pml4, 0x500000, 4096, false));
      SELFTEST_ASSERT(arch_user_range_accessible(g_pml4, 0x400000, 4096, true));
      /* ... all rows ... */
  }
  ```
  （SELFTEST_ASSERT 宏参考 `kernel/test/selftest.c` 既有测试写法。）

- [ ] **Step 2: `copy_*_ft` 跨页测试——在当前地址空间建立受控映射（非合成表）**
  `copy_to_user_ft`/`copy_from_user_ft` 通过**当前 CR3** 访问虚拟地址，合成表无效。因此在 boot 上下文**修改当前 pml4**：把两个**不连续物理页**（phys A、phys A+3·PAGE_SIZE）映射到相邻虚拟页 `0x600000`/`0x601000`（当前 pml4 用户区，`< addr_limit`），测试后解除映射。

  ```c
  /* address math: check_user_range in boot ctx (init_task, no user mm) */
  SELFTEST_ASSERT(!syscall_check_user_range(0, 4, true));            /* NULL */
  SELFTEST_ASSERT(!syscall_check_user_range(0x1, 4, true));          /* < USER_MIN_ADDR */
  SELFTEST_ASSERT(!syscall_check_user_range(0xffff800000000000ULL, 4, true)); /* kernel range */
  SELFTEST_ASSERT(!syscall_check_user_range(0x400000, (1ULL<<40), true));      /* overflow */
  SELFTEST_ASSERT(syscall_check_user_range(0x400000, 0, true));               /* len==0 */

  /* longjmp path: temp addr_limit override + low unmapped copy */
  uint64_t saved_limit = current->addr_limit;
  current->addr_limit = 0x00007FFFFFFFFFFFULL;
  char ksrc[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  ssize_t rc = copy_to_user_ft((void *)0x1000, ksrc, 16);   /* unmapped low -> #PF -> longjmp */
  current->addr_limit = saved_limit;
  SELFTEST_ASSERT(rc == -EFAULT);                           /* longjmp path works */

  /* cross-page tests need the live CR3 (boot ctx: current->mm==&init_mm but
     init_mm.pml4 NOT set, task.h:232 — never use current->mm->pml4 here). */
  uint64_t *cur_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)arch_get_page_table());
  /* Map two 4KB pages at 0x600000/0x601000 via ensure_pt (hierarchy-probing:
     the guard region may lack PML1 or be a 2MB PDE — ensure_pt creates/splits
     and records in the ctx; we then save the returned SLOT and write through
     it; restore in reverse creation order via the two ctxs).  NO get_leaf/
     set_leaf (they assumed PML1 exists). */
  struct test_map_ctx cta = {0}, ctb = {0};
  uint64_t *slot_a = ensure_pt(cur_pml4, 0x600000, &cta);
  uint64_t saved_a = *slot_a;
  uint64_t *slot_b = ensure_pt(cur_pml4, 0x601000, &ctb);
  uint64_t saved_b = *slot_b;
  uint64_t pa = alloc_4k_page(), pb = alloc_4k_page();      /* 4KB allocator, pmm.h:102 */
  SELFTEST_ASSERT(pa != 0 && pb != 0);                      /* alloc failure = hard fail */
  int tries = 0;
  while ((pa >> 12) + 1 == (pb >> 12) && tries++ < 16)      /* require non-adjacent */
      { free_4k_page(pa); pa = alloc_4k_page(); }
  SELFTEST_ASSERT((pa >> 12) + 1 != (pb >> 12));            /* REQUIRED gate: if still
      adjacent after retries, the cross-page-correctness test CANNOT run — FAIL the
      selftest (record FAIL in log), do NOT silently continue mapping. */

  /* no short count (prove cross-page no partial count): FIRST map page A only,
     confirm 0x601000 unmapped, then a copy spanning the boundary faults on the
     2nd page -> -EFAULT (NOT +N partial).  If page A were unmapped too, it would
     fault on page 1 and not prove cross-page semantics — order matters: */
  *slot_a = pa | PAGE_Present | PAGE_R_W | PAGE_USER;
  arch_flush_tlb_page(0x600000);
  SELFTEST_ASSERT(*slot_b == 0);                           /* 2nd page genuinely unmapped */
  current->addr_limit = 0x00007FFFFFFFFFFFULL;
  rc = copy_to_user_ft((void *)0x600ff0, ksrc, 32);         /* 0x600ff0..0x601010: page A mapped,
      page B unmapped -> faults on page 2 -> -EFAULT (no short count) */
  current->addr_limit = saved_limit;
  SELFTEST_ASSERT(rc == -EFAULT);

  /* cross-page correctness with non-contiguous phys (deterministic success path): */
  *slot_b = pb | PAGE_Present | PAGE_R_W | PAGE_USER;
  arch_flush_tlb_page(0x601000);
  uint8_t *kA = (uint8_t *)Phy_To_Virt(pa);
  uint8_t *kB = (uint8_t *)Phy_To_Virt(pb);
  memset(kA, 0xAA, 4096); memset(kB, 0xBB, 4096);
  ssize_t n = copy_to_user_ft((void *)0x600ff0, ksrc, 32);  /* spans into 0x601000 */
  SELFTEST_ASSERT(n == 32);
  SELFTEST_ASSERT(memcmp(kA + 0xff0, ksrc, 16) == 0);       /* first 16B in page A */
  SELFTEST_ASSERT(memcmp(kB, ksrc + 16, 16) == 0);          /* last 16B in page B */

  /* _ft_res: on-fault cleanup MUST run on the longjmp path (Task 8 pipe
     reservation depends on this).  Success path must NOT run it.  Runs
     while page A is still mapped (before the restore below). */
  static int cleanup_ran = 0;
  static void cb_cleanup(void *arg) { (void)arg; cleanup_ran = 1; }
  cleanup_ran = 0;
  current->addr_limit = 0x00007FFFFFFFFFFFULL;
  rc = copy_to_user_ft_res((void *)0x2000, ksrc, 16, cb_cleanup, NULL); /* unmapped -> fault */
  current->addr_limit = saved_limit;
  SELFTEST_ASSERT(rc == -EFAULT);
  SELFTEST_ASSERT(cleanup_ran == 1);      /* cleanup ran on the fault path */
  cleanup_ran = 0;
  rc = copy_to_user_ft_res((void *)0x600000, ksrc, 16, cb_cleanup, NULL); /* page A mapped -> ok */
  SELFTEST_ASSERT(rc == 16);
  SELFTEST_ASSERT(cleanup_ran == 0);      /* success path: no cleanup */

  /* restore: leaf slots first, then reverse-creation-order restore of the two
     ctxs (LIFO: ctb created after cta -> restore ctb FIRST, then cta) which puts
     back parent PDE/l2/l3 slots, frees test-only tables, flushes TLB. */
  *slot_a = saved_a; *slot_b = saved_b;                     /* restore leaf entries */
  arch_flush_tlb_page(0x600000); arch_flush_tlb_page(0x601000);
  restore_pt(&ctb);                                         /* LIFO: later-created first */
  restore_pt(&cta);
  free_4k_page(pa); free_4k_page(pb);        /* free_4k_page(phys), pmm.h:103 */

  /* strnlen_user */
  /* page-tail NUL: string at 0x600ffc "AB" NUL at 0x600ffe (next page unmapped) -> returns 2 */
  /* no NUL within max -> returns max; unmapped -> -EFAULT */
  ```
  说明：`ensure_pt` 是**层级探测的叶子 PTE 槽提供者**（参考 `arch_virt_to_phys` mmu.h:38-56 的 l4/l3/l2/l1 索引计算）。**0x600000/0x601000 是 guard/未映射区，不能假定 PML1 槽已存在**。具体实现（写入 Task 3 代码）：

  ```c
  /* ── hierarchy-probing leaf helpers (guard/unmapped region) ── */
  struct test_map_ctx {                 /* records everything created, for restore */
      uint64_t va;
      uint64_t saved_l3, saved_l2, saved_pde;   /* original parent slots (0 = absent) */
      uint64_t *new_l3, *new_l2, *new_pml1;     /* test-only tables we created */
      int    created;                   /* 1 if we created/split anything */
      int    split_2m;                  /* 1 if we split a 2MB PDE -> new_pml1 */
  };
  /* ensure_pt: walk l4->l3->l2, creating missing intermediates (alloc_4k_page,
     RECORD parent slot originals + created tables).  **If l2 is a 2MB huge PDE,
     do NOT just replace it**: first read the PDE's phys base + permission bits,
     allocate a PML1 table and INITIALIZE ALL 512 PTEs to map each 4KB subpage of
     the original 2MB (preserving the whole mapping), THEN modify only the two
     test slots.  Records the original PDE for restore. */
  static uint64_t *ensure_pt(uint64_t *pml4, uint64_t va, struct test_map_ctx *ctx) {
      uint64_t l2val = pml2[l2];                       /* read original PDE */
      if (l2val & PAGE_PS) {                           /* 2MB huge page: SPLIT */
          uint64_t *pml1 = alloc_4k_page_as_tbl();
          uint64_t base  = l2val & ~(uint64_t)0x1FFFFF;   /* 2MB phys base (bits 21+) */
          /* ⚠️ 位级修正：不得把 l2val 的物理地址字段(21+)或 PAT 直接 OR 进新表项。
             PAT: 2MB PDE 是 bit 12, 4KB PTE 是 bit 7 -> 必须重映射; 非叶 table entry
             不继承 huge-page PAT。 拆成独立 flags: */
          uint64_t leaf_flags  = (l2val & (0x7F | 0x100))          /* P(0),RW(1),US(2),
                                       PWT(3),PCD(4),A(5),D(6) + G(8) — 无 PS(7)、无物理位 */
                               | ((l2val & 0x1000ULL) >> 5)        /* PAT: PDE bit12 -> PTE bit7 */
                               | (l2val & 0x8000000000000000ULL);  /* XD(63) */
          uint64_t table_flags = (l2val & (0x7F | 0x100))          /* 仅非叶允许的权限/cache/NX;
                                       清 PS、PAT、物理地址字段 */
                               | (l2val & 0x8000000000000000ULL);  /* XD(63) */
          for (int i = 0; i < 512; i++)                   /* preserve ALL 512 4KB maps:
                                                             the other 511 must keep working */
              pml1[i] = base + ((uint64_t)i << 12) | leaf_flags;
          ctx->split_2m = 1; ctx->saved_pde = l2val;
          pml2[l2] = Phy(pml1) | table_flags;             /* table entry: phys 恰为 pml1, 无物理位混入 */
          ctx->new_pml1 = pml1;
          /* selftest 断言（防本次错误复发）:
             SELFTEST_ASSERT((pml2[l2] & PAGE_ADDR_MASK) == Phy(pml1));   // 表指针恰为 pml1
             // 未覆盖的相邻 4KB 子页仍翻译回原物理地址 (证明 511 个映射保留):
             uint64_t probe = arch_virt_to_phys(cur_pml4, 0x600000 + 0x2000);
             SELFTEST_ASSERT(probe == base + 0x2000); */
      } else if (!(l2val & 1)) { ctx->saved_l2 = l2val; ctx->new_l2 = alloc_4k_page_as_tbl(); pml2[l2] = Phy(new_l2)|flags; }
      /* similarly probe/create l3 (and l4 can't be absent for kernel) */
      return &pml1[l1];                                /* the leaf slot */
  }
  /* restore_pt(ctx): 恢复顺序必须与创建顺序相反 (LIFO)：先恢复本 ctx 的叶项/
     l2 槽，再（若本 ctx split）恢复 saved_pde + free new_pml1，最后恢复上层父槽
     + free 测试表 + flush TLB。 两个测试 VA 同在 0x600000..0x7fffff 的同一 2MB
     区间 → 只有一个 ctx 会 split；但若两次 ensure_pt 各建了部分层级，按"后创建
     先恢复"逆序调用 restore。 所有失败路径先 restore_pt 再失败。 */
  static void restore_pt(struct test_map_ctx *ctx) { ... }
  ```
  用例顺序（所有失败路径先 `restore_pt` 再失败）：
  1. `slot_a = ensure_pt(0x600000)` → 保存 `saved_a = *slot_a` → `*slot_a = pa | flags`；
  2. no-short-copy 用例（0x601000 未映射，跨页 fault 在第二页）；
  3. `slot_b = ensure_pt(0x601000)` → 保存 `saved_b` → `*slot_b = pb | flags` → 跨页成功用例；
  4. `_ft_res` cleanup 用例（页 A 仍映射）；
  5. `restore_pt` 统一恢复两个 ctx（父项 + 叶槽 + TLB）+ free pa/pb。
  4KB 页用 `alloc_4k_page()`/`free_4k_page(phys)`（pmm.h:102-103）——`alloc_pages` 单位是 2MB 且 `free_pages` 要 number 参数，不适用。非连续用**实际物理页框比较断言**（`(pa>>12)+1 != (pb>>12)`），不写死 `+3*PAGE` 假设；相邻则 bounded 重试，仍不满足则**必需门禁失败**（记录 FAIL，不静默跳过）。

- [ ] **Step 3: 注册到 selftest.c**
  在 `kernel/test/selftest.c` 加 `extern void selftest_uaccess(void);` + 在 `selftest_run_all()` 调用表加 `selftest_uaccess();`（参考 test_timer.c 等注册方式）。

- [ ] **Step 4: 跑测试（RED→GREEN）**
  ```bash
  make clean && make   # OS01_SELFTEST=1 已默认（kernel/Makefile:114）
  make run             # 或 qemu，grep [selftest]
  ```
  预期：先只有框架（断言失败/未实现报错），逐步填断言到全 PASS。**Step 2 的 longjmp 用例是本任务核心门禁**——若 QEMU 下 `copy_to_user_ft((void*)0x1000,...)` 触发 #PF 后内核死锁或崩溃，说明挂钩/门控有误，回到 Task 2 修。

- [ ] **Step 5: Commit**
  ```bash
  git add kernel/test/test_uaccess.c kernel/test/selftest.c
  git commit -m "test(uaccess): selftest — synthetic pml4 walker + longjmp path + cross-page

  Deterministic cross-page correctness with two non-contiguous physical
  pages (user space can't control phys alloc; systest can't guarantee it).
  Longjmp path via temp addr_limit override (boot ctx has no user mm)."
  ```

---

## Task 4: Cat A 路径字符串 + 旧指针替换

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`（open/exec/chdir/stat/access/unlink/mkdir/rmdir/rename/truncate 的 path 处理 + chdir path[0] + open O_CREAT 旧指针）

**Interfaces:**
- Consumes: `strnlen_user`/`copy_from_user_ft`（Task 2）
- Produces: 统一模式 `strnlen_user(path, VFS_NAME_MAX)` 定长 → `kmalloc` + `copy_from_user_ft`（后续 Task 5 复用）

**模式（所有 Cat A handler 统一）**：

```c
// 原: (uint64_t)path >= current->addr_limit → -EFAULT;  char *path_copy = strdup(path);
// 新 (strdup 是裸读用户内存 — 同纪律禁止; 改 strnlen_user 定长 + copy_from_user_ft 单次容错拷):
int plen = strnlen_user(path, VFS_NAME_MAX);
if (plen < 0) { regs->rax = -EFAULT; break; }        // fault
if (plen >= VFS_NAME_MAX) { regs->rax = -ENAMETOOLONG; break; }
char *path_copy = kmalloc(plen + 1);
if (!path_copy) { regs->rax = -ENOMEM; break; }
if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {   // 含 NUL, 单次容错拷贝
    kfree(path_copy);
    regs->rax = -EFAULT;
    break;
}
```
说明：`strnlen_user` 逐字节容错读（页尾 NUL 合法）确定 `plen`；`copy_from_user_ft` 把 `[path, path+plen+1]` 一次拷进内核堆（含 NUL）——**无 strdup 裸读窗口**。**后续所有对路径的操作一律用 `path_copy`，释放移到所有使用之后。**

- [ ] **Step 1: 给 open/exec/stat/chdir 补 strnlen_user + 全路径替换（含 §5.2）**
  逐 handler：
  - `SYS_open`（trap.c:1249-1365）：strnlen_user 定长 + kmalloc + copy_from_user_ft；**O_CREAT 分支（1280-1282）的 `strlen(path)`/`memcpy(pbuf, path, ...)` 改用 `path_copy`**（parent_path 解析在拷贝之后做，用 path_copy）。
  - `SYS_exec`（1194-1227）：path 定长 + 内核拷贝（argv/envp 深拷贝在 Task 5）。
  - `SYS_chdir`（1422-1468）：strnlen_user + 内核拷贝；**`kfree(path_copy)` 移到 `path[0]`/`strlen(path)`/`memcpy(new_cwd, path, ...)`（1446-1458）全部使用之后，且这些改用 `path_copy`**。
  - `SYS_stat`（1494-1523）：path 定长 + 内核拷贝（buf 写回在 Task 6）。
  验证：`make clean && make` + QEMU：`cd /bin; ls` 正常、相对/绝对路径、超长路径 `-ENAMETOOLONG`、坏路径 `-EFAULT`。

- [ ] **Step 2: access/unlink/mkdir/rmdir/rename/truncate 定界**
  逐个套上述模式（这些 handler 现直接 `strdup(path)` 无定界）。`SYS_rename` 两个路径都处理。超长 → `-ENAMETOOLONG`（rename 两个都查）。
  验证：QEMU `touch/rm/mkdir/rmdir/mv/truncate` busybox 命令正常。

- [ ] **Step 3: 回归 + Commit**
  ```bash
  make clean && make && make run    # systest 184/0 + ash 手工
  git add kernel/arch/x86_64/trap.c
  git commit -m "fix(uaccess): Cat A path strings — strnlen_user bounded copy + stale-pointer fixes

  chdir path[0] after kfree, open O_CREAT strlen/memcpy on original user
  path now use kernel path_copy (TOCTOU/fault windows closed). No unbounded
  strdup scan (page-tail NUL paths no longer over-rejected)."
  ```

---

## Task 5: Cat A' exec argv/envp 有界深拷贝

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`（SYS_exec handler，Task 4 之后）
- Modify: `kernel/sched/task.c`（`sys_exec` 签名/调用）

**Interfaces:**
- Consumes: `strnlen_user`/`copy_from_user_ft`（Task 2）、`MAX_ARGV`/`MAX_ARG_STRLEN`/`MAX_ARG_TOTAL`（Task 1）
- Produces: 内核堆 argv/envp 数组传给 `sys_exec`（新签名：`int64_t sys_exec(const char *path, pt_regs_t *regs, const char *const *argv, const char *const *envp)`，argv/envp 已是内核指针数组 + 字符串全在内核堆）

- [ ] **Step 1: 写失败测试——systest 加 exec 敌意 argv 用例（Task 9 的 H8，先加框架）**
  在 `user/systest.c` 加：
  ```c
  // H8: exec with hostile argv element / no-NUL string -> -EFAULT, no crash
  static void test_exec_hostile_argv(void) {
      char *argv_ok[] = { "/bin/spin", NULL };
      char *argv_bad[] = { "/bin/spin", (char *)0x1000, NULL };   // bad element ptr
      int64_t r1 = exec("/bin/spin", argv_bad, NULL);
      CHECK3(r1 < 0, "exec_hostile_argv", "bad element -> <0");
      /* no-NUL string: element pointing at a mapped page without NUL within 4096 */
      ...
  }
  ```
  跑：QEMU 下断言失败（现实现可能崩或 -EFAULT 视情况）。预期 RED。

- [ ] **Step 2: 实现深拷贝（trap.c SYS_exec，path strdup 之后、sys_exec 之前）**

  ```c
  // Bounded deep-copy of argv/envp (arrays + each string) to kernel heap,
  // BEFORE sys_exec builds the new pml4 / frees the old space.
  // Returns 0, -EFAULT, -E2BIG, -ENOMEM.  *out_argv/*out_envp get kmalloc'd
  // arrays of kmalloc'd strings; caller frees with free_deep_argv().
  static int deep_copy_argv(const char *const *user_argv,
                            const char *const *user_envp,
                            char ***out_argv, char ***out_envp)
  {
      size_t ac = 0, ec = 0, total = 0;
      if (user_argv) {
          while (user_argv[ac] != NULL) {           // unbounded scan of array (user)
              /* must fault-tolerantly count: use strnlen_user on each? NO — array itself
                 is user memory; bound the scan by MAX_ARGV and fault-recover. */
              ac++;
              if (ac > MAX_ARGV) return -E2BIG;
          }
      }
      /* The array scan itself reads user pointers — wrap in copy_from_user_ft
         per element pointer, or pre-validate array range with check_user_range.
         Concrete approach: */
      ...
  }
  ```
  **实现要点（必须可编译、无占位）**：
  - **数组项数扫描**：`user_argv`/`user_envp` 是用户内存中的指针数组。逐项 `copy_from_user_ft(&ptr, &user_argv[i], sizeof(ptr))`（fault → `-EFAULT`），每项校验 `ptr == NULL` 结束 或 `ptr >= USER_MIN_ADDR && ptr < addr_limit`（坏元素 → `-EFAULT`），计数 > `MAX_ARGV` → `-E2BIG`。
  - **每项字符串**：`strnlen_user(ptr, MAX_ARG_STRLEN)`（fault → `-EFAULT`；=max → `-E2BIG`），`total += len+1` 累积，> `MAX_ARG_TOTAL` → `-E2BIG`；`kmalloc(len+1)` + `copy_from_user_ft(kstr, ptr, len+1)`（fault → `-EFAULT`，并释放已拷部分）。
  - 失败路径释放已分配（`kfree` 循环），返回错误。
  - 成功：`*out_argv = kmalloc((ac+1)*sizeof(char*))`，填内核字符串指针 + NULL 结尾；envp 同。`sys_exec(path_copy, regs, *out_argv, *out_envp)` 后释放（`free_deep_argv`）。
  - **在 `sys_exec` 建新 pml4 / 释放旧地址空间之前完成**（当前调用点 trap.c:1224 在 `sys_exec(path_copy,...)` 处，深拷贝在其前；sys_exec 内不再触碰用户 argv/envp，task.c:1353-1373 的遍历/strlen/memcpy 全改操作内核副本）。

- [ ] **Step 3: sys_exec 改造（task.c:1353-1373）**
  现有 `while (argv[s_argc] != NULL)` + `strlen(argv[i])` + `memcpy(KSTACK(...), argv[i], len)` 现在处理的是内核副本（Task 5 传入），不再解引用用户内存。核对：`argv`/`envp` 参数在 sys_exec 内全部操作内核堆字符串。`str_offset[128]` 上限与 `MAX_ARGV` 一致（≥128 项由深拷贝侧拒绝，不触发栈溢出）。

- [ ] **Step 4: 跑测试（GREEN）+ 回归**
  ```bash
  make clean && make && make run    # systest：H8 通过；正常 exec /bin/* 全工作
  ```
  回归：QEMU ash 跑 `ls/echo/cat`（argv 正常）、`/bin/spin`、systest 全量。

- [ ] **Step 5: Commit**
  ```bash
  git add kernel/arch/x86_64/trap.c kernel/sched/task.c user/systest.c
  git commit -m "fix(uaccess): exec argv/envp bounded deep-copy (MAX_ARGV/STRLEN/TOTAL)

  Deep-copy arrays+strings to kernel heap before sys_exec builds new pml4
  (no UAF on old space). Bad element ptr / no-NUL / over-limit -> -EFAULT/-E2BIG.
  sys_exec now operates on kernel copies only."
  ```

---

## Task 6: Cat B 定长结构体 / out-buffer（waitpid/pipe/ioctl/time/nanosleep/signal/sigprocmask/uname/getdents/stat.buf/fstat/getcwd/clock_gettime/futex/poll/select/socket 族）

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`、`kernel/sched/task.c`（waitpid）、`kernel/fs/file.c`（do_pipe 回滚）、`kernel/tty/tty.c`（ioctl termios/pgrp bounce）、`kernel/fs/select.c`、`kernel/sync/futex.c`

**Interfaces:**
- Consumes: `syscall_check_user_range`/`copy_to_user_ft`/`copy_from_user_ft`（Task 2）
- Produces: Cat B 统一模式——读方向 `copy_from_user_ft` 拷内核栈 → 操作内核副本 → 写方向 `copy_to_user_ft` 回写；NULL 合法处保持。

**通用模式（写 out-buffer）**：
```c
if (buf) {   // NULL 合法处
    ssize_t r = copy_to_user_ft(buf, &kval, sizeof(kval));
    if (r < 0) { regs->rax = -EFAULT; break; }
}
```

- [ ] **Step 1: waitpid status 清理顺序（task.c:987-1006）——本任务最关键**
  替换 do_waitpid 的写 status 段：
  ```c
  if (child) {
      ssize_t status_rc = 0;
      if (user_status) {
          int status = (int)exit_code;
          status_rc = copy_to_user_ft(user_status, &status, sizeof(status));
      }
      // Unconditional reclamation — NEVER return inside the _ft failure branch.
      if (child->thread)           kfree(child->thread);
      if (child->fpu_save)         kfree(child->fpu_save);
      if (child->stack_alloc_base) kfree(child->stack_alloc_base);
      debug_task("waitpid: pid=%d reaped child %d (exit=%d)\n", ...);
      return (status_rc < 0) ? -EFAULT : child_pid;
  }
  ```
  `user_status` 非 NULL 时入口已 `check_user_range(status, 4, true)`（trap.c SYS_waitpid），NULL 合法。
  验证：QEMU systest 既有 waitpid 用例全过（184/0 不变）+ H6（坏 status → -EFAULT 且 child 被回收无泄漏）。

- [ ] **Step 2: pipe 写回 + 回滚（file.c do_pipe:733-772）**
  替换 `memcpy(user_fds, fds, sizeof(fds))`（766 之后）：
  ```c
  int fds[2] = { rfd, wfd };
  if (copy_to_user_ft(user_fds, fds, sizeof(fds)) < 0) {
      // Rollback: close both fds only — the last close auto-frees the pipe
      // via file_free (file.c:64).  Do NOT pipe_free() explicitly (double-free).
      fd_close(current->files, rfd);
      fd_close(current->files, wfd);
      return -EFAULT;
  }
  ```
  删除现有 `if ((uint64_t)user_fds >= current->addr_limit) return -EFAULT;`（泄漏路径）。`user_fds` NULL/低地址由 `_ft` 兜底（fault → -EFAULT → 回滚）。
  验证：H10（坏 fds → -EFAULT 且 fd 计数无泄漏：`/proc/<pid>/fd/` 或 getdents 回查）。

- [ ] **Step 3: 其余 Cat B 站点统一（trap.c/select.c/tty.c）**
  逐个套模式（每处：读方向 `copy_from_user_ft` 拷内核栈 → 操作 → 写方向 `copy_to_user_ft`）：
  - `time`（1871）：`tloc` 可 NULL，`copy_to_user_ft(tloc, &zero, 8)`（zero=0）
  - `gettimeofday`（1880）：`tv`/`tz` 可 NULL，各 `_ft` 写
  - `clock_gettime`（1895）：`tp` 必填，`copy_to_user_ft(tp, &kts, sizeof(kts))`
  - `times`（2072）：`buf` 可 NULL，`copy_to_user_ft`
  - `uname`（2081）：`copy_to_user_ft(buf, &kuts, sizeof(kuts))`（内核构造 kuts，不再 memset/strcpy 直写用户）
  - `getcwd`（1470）：`copy_to_user_ft(buf, cwd, len)`（len= strlen(cwd)+1，`check_user_range(buf, size, true)` 入口）
  - `stat.buf`（1494）：`vfs_stat(node, &kstat)`（内核 struct）→ `copy_to_user_ft(buf, &kstat, sizeof(kstat))`；`fstat` 同
  - `getdents64`（1668）：**内核 bounce + offset 成功后提交**（`vfs_getdents` 生成目录项时就推进传入 offset，若拷贝失败用户没拿到目录项但偏移已前进——必须改为）：
    ```c
    uint64_t cap = min(count, (uint64_t)UACCESS_BOUNCE_SIZE);
    uint8_t *kbuf = kmalloc(cap);
    if (!kbuf) { regs->rax = -ENOMEM; break; }
    uint64_t next_offset = f->offset;
    int n = vfs_getdents(f->node, (struct linux_dirent64 *)kbuf, (unsigned)cap, &next_offset);
    ssize_t rc = (n >= 0) ? copy_to_user_ft(buf, kbuf, n) : n;
    kfree(kbuf);
    if (n >= 0 && rc >= 0) {
        f->offset = next_offset;      // commit offset ONLY on successful copy
        regs->rax = n;
    } else {
        regs->rax = (n < 0) ? -EIO : -EFAULT;   // offset NOT advanced on failure
    }
    ```
  - `nanosleep`（2026）：`req` 读用 `copy_from_user_ft(&kreq, req, sizeof(kreq))`；`rem` 写（可 NULL）在 `blocker_wait` 后 `copy_to_user_ft`
  - `signal`（2152）：`act` `copy_from_user_ft` 拷内核 `struct sigaction` → 校验 → 装；`oldact` `copy_to_user_ft`
  - `sigprocmask`（2196）：`set` `copy_from_user_ft`、`oldset` `copy_to_user_ft`
  - `ioctl`（1646 + tty.c:314-349）：per-request bounce——TCGETS/TIOCGPGRP 写方向 `copy_to_user_ft`；TCSETS/TIOCSPGRP 读方向 `copy_from_user_ft` 拷内核再操作（tty_ioctl 内改造）
  - `poll`（poll.c:481）：`copy_from_user_ft(fds 内核副本)` → do_poll → `copy_to_user_ft` 回写 revents；`check_user_range(fds, nfds*sizeof(pollfd), rw)` 入口
  - `select`/`pselect6`（select.c:124-376）：统一 `check_user_range` + 回写 `copy_to_user_ft`（现有 range 检查保留入口，拷贝改 `_ft`）
  - **futex**（futex.c:48,97）：仅入口 `check_user_range(uaddr, 4, rw)`（trap.c:2326 起点检查升级）；**持 `bucket->lock` 内的值读取改用 `arch_virt_to_phys`（4KB/2MB 完整叶子翻译）**：`uint64_t phys = arch_virt_to_phys(user_pml4, (uint64_t)uaddr); if (!phys){unlock;-EFAULT;} void *kaddr=(void*)Phy_To_Virt(phys); int v=*(volatile int*)kaddr;`（弃 `user_va_to_phys` 2MB-only）
  - socket 族（53/54/57/58/59/60/61）——**具体桥接**（不能笼统"bounce"）：
    - `bind`（trap.c:2438）：`struct sockaddr_in a; if (!check_user_range(rsi,sizeof(a),false)){-EFAULT;} if (copy_from_user_ft(&a,(void*)rsi,sizeof(a))<0){-EFAULT;} do_bind(fd, a.sin_addr, os01_ntohs(a.sin_port));`
    - `connect`（2371）：同 bind（拷内核栈后再 do_connect）
    - `setsockopt`（2456）：**optlen 必须设 ABI 上限 + 处理 0**（敌意 `r8` 可造成超大 kmalloc/DoS）：
      ```c
      #define SOCKOPT_MAX 4096
      uint64_t optlen = regs->r8;
      if (optlen == 0 || optlen > SOCKOPT_MAX) { regs->rax = -EINVAL; break; }
      if (!check_user_range(regs->r10, optlen, false)) { regs->rax = -EFAULT; break; }
      void *optval = kmalloc(optlen);
      if (!optval) { regs->rax = -ENOMEM; break; }
      if (copy_from_user_ft(optval, (void *)regs->r10, optlen) < 0) { kfree(optval); regs->rax = -EFAULT; break; }
      regs->rax = do_setsockopt(fd, lv, opt, optval, optlen);
      kfree(optval);
      ```
    - `getsockname`（2464）：**现直接读写 addr_ptr/addrlen_ptr** → 改本地内核 `sockaddr_in`+`uint32_t`：`struct sockaddr_in kaddr; uint32_t klen = sizeof(kaddr); ret = do_getsockname(fd, &kaddr, &klen); if (ret>=0){ if(copy_to_user_ft((void*)rsi,&kaddr,sizeof(kaddr))<0){-EFAULT;} if(copy_to_user_ft((void*)rdx,&klen,sizeof(klen))<0){-EFAULT;} }`
    - `getsockopt`（2477）：**先安全读用户 optlen 到本地，再按受限长度分配和回写**（`r8` 指向用户 optlen，不能直接当长度用）：
      ```c
      uint32_t klen = 0;
      if (!check_user_range(regs->r8, sizeof(uint32_t), false) ||
          copy_from_user_ft(&klen, (void *)regs->r8, sizeof(klen)) < 0) {
          regs->rax = -EFAULT; break;
      }
      if (klen > SOCKOPT_MAX) { regs->rax = -EINVAL; break; }
      void *kopt = kmalloc(klen ? klen : 1);
      if (!kopt) { regs->rax = -ENOMEM; break; }
      regs->rax = do_getsockopt(fd, lv, opt, kopt, &klen);
      if (regs->rax >= 0) {
          if (copy_to_user_ft((void *)regs->r10, kopt, klen) < 0 ||
              copy_to_user_ft((void *)regs->r8, &klen, sizeof(klen)) < 0)
              regs->rax = -EFAULT;
      }
      kfree(kopt);
      ```
    - `sendto`（2382）：sockaddr `copy_from_user_ft` 拷内核（同 bind）；**buf 走 Task 8 的 socket TX 分块 bounce**
    - `recvfrom`（2402）：**buf 写回 + 地址写回都 `_ft`**——`do_recvfrom` 收数据到内核 bounce 或直接到 `_ft`；`addr_ptr`/`addrlen_ptr`（trap.c:2431,2433）post-block 写回 `copy_to_user_ft`；**只有 `_ft` 成功才推进/释放 netbuf（rx_off）**

- [ ] **Step 4: 构建 + 回归**
  ```bash
  make clean && make && make run
  ```
  QEMU：systest 184/0 全过（含 select/poll/waitpid/futex/信号 既有用例）+ busybox ash 常用命令 + socktest（网络回归：getpeername/sockaddr 路径）。

- [ ] **Step 5: Commit（可拆多个小 commit，每子组独立）**
  ```bash
  git add kernel/arch/x86_64/trap.c kernel/sched/task.c kernel/fs/file.c \
          kernel/tty/tty.c kernel/fs/select.c kernel/sync/futex.c
  git commit -m "fix(uaccess): Cat B fixed-struct/out-buffer sites — _ft bounce

  waitpid cleanup order (reclaim child unconditionally after copy result),
  do_pipe rollback (fd_close x2, no explicit pipe_free), time/gettimeofday/
  clock_gettime/times/uname/getcwd/stat/fstat/getdents/nanosleep/signal/
  sigprocmask/ioctl/poll/select/socket family -> check_user_range +
  copy_*_ft. futex: entry check + arch_virt_to_phys full leaf translation
  (was 2MB-only user_va_to_phys, wrong page for 4KB/COW)."
  ```

---

## Task 7: 信号投递写栈 + sigreturn 读帧（§5.3 + #43，跨页对称）

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`（`arch_do_signal_delivery` 780-924 + `SYS_sigreturn` 2235-2266）

**Interfaces:**
- Consumes: `syscall_check_user_range`/`copy_to_user_ft`/`copy_from_user_ft`（Task 2）
- Produces: 信号读（sigreturn）+ 写（投递）跨页正确闭环

- [ ] **Step 1: `arch_do_signal_delivery` 写栈改 `_ft`（§5.3）**
  替换 trap.c:895-905（`user_va_to_phys` + `Phy_To_Virt` + memcpy 连续写）：
  ```c
  // 2. Compute aligned user RSP (unchanged, trap.c:892)
  size_t total = sizeof(frame) + 8;
  uint64_t new_rsp = ((regs->rsp - total - 8) & ~15UL) + 8;

  // 3. Fast reject + authoritative write via user VIRTUAL addresses.
  if (!syscall_check_user_range(new_rsp, total, true))
      continue;                          // keep pending, retry
  if (copy_to_user_ft((void *)new_rsp, &tramp, 8) < 0)
      continue;                          // keep pending — do NOT touch regs
  if (copy_to_user_ft((void *)(new_rsp + 8), &frame, sizeof(frame)) < 0)
      continue;                          // keep pending — do NOT touch regs

  // 4. Rewrite pt_regs (trap.c:908-914 unchanged)
  regs->rdi = sig; regs->rip = (uint64_t)handler; regs->rsp = new_rsp; ...
  ```
  删除 `user_pml4`/`frame_phys`/`kstack`/`memcpy(kstack,...)`/`memcpy(kstack-8,...)`。投递路径无持锁（返回用户态路径），`_ft` 合规。

- [ ] **Step 2: `SYS_sigreturn` 读帧改 `_ft`（#43）**
  替换 trap.c:2238-2259 的 `user_va_to_phys`+`Phy_To_Virt`+`kframe` 连续读：
  ```c
  struct sigframe frame;
  if (!syscall_check_user_range(regs->rsp, sizeof(frame), false) ||
      copy_from_user_ft(&frame, (void *)regs->rsp, sizeof(frame)) < 0) {
      regs->rax = -EFAULT;
      break;                             // other regs untouched
  }
  if ((frame.cs & 3) != 3) { regs->rax = -EINVAL; break; }
  current->blocked = frame.blocked;
  regs->r15 = frame.r15; regs->r14 = frame.r14; ...   // all from kernel-stack frame
  regs->rip = frame.rip; regs->cs = frame.cs; regs->rflags = frame.rflags;
  regs->rsp = frame.rsp; regs->ss = frame.ss;
  ```

- [ ] **Step 3: 写 E2E 测试（systest，Task 9 框架先加）**
  - **投递写栈 E2E**（§5.3）：子进程 `mmap` 一 4KB 页（可写），**内联汇编保存原 RSP、设 RSP 到页尾附近、`kill(self, SIGUSR1)`、syscall 返回后立即恢复 RSP**；装 SIGUSR1 handler（`SA_RESTART` 无，直接 handler 置 flag）。断言：不崩溃、handler 未错误进入（frame 写 fault → 保持 pending，返回原 RSP）；恢复后（RSP 回到真实栈）再触发一次信号 → handler 正常执行。
  - **sigreturn 跨页 E2E**（#43，H11）：子进程 mmap 两个相邻 4KB 页，构造 sigframe（已知 RIP 指向一个安全地址/loop），把 frame 放跨页边界（`mmap_base + 0xff0`），`syscall(SYS_sigreturn)`，断言：正确恢复到该 RIP 或返回 `-EFAULT`，绝不崩溃。
  两用例都 fork 子进程，父 `waitpid` 断言子正常退出（非信号）→ 证明内核不崩。

- [ ] **Step 4: 跑测试 + 回归**
  ```bash
  make clean && make && make run
  ```
  QEMU：systest（信号既有用例 + 新 E2E）+ ash 交互 Ctrl-C（信号投递正常）。

- [ ] **Step 5: Commit**
  ```bash
  git add kernel/arch/x86_64/trap.c user/systest.c
  git commit -m "fix(uaccess): signal delivery write + sigreturn read via _ft

  arch_do_signal_delivery: check_user_range + copy_to_user_ft to user VIRTUAL
  addresses (was Phy_To_Virt contiguous write — wrong physical page for 4KB
  cross-page); any write failure keeps signal pending, regs untouched.
  sigreturn: copy_from_user_ft reads whole frame via virtual addresses,
  restore uses kernel-stack frame only. Read+write cross-page paths symmetric."
  ```

---

## Task 8: Cat C 大缓冲 VFS bounce 分块层 + 阻塞触点 + do_pipe（Task 6 已含 do_pipe 回滚，此处只 VFS bounce + socket rx/tx）

> **相对 spec 的偏差（已按 review 收敛）**：pipe/PTY 读的并发正确性通过**内核强制的 `read_busy` 预约序列化**（`fork`/`dup` 共享读端 + 默认 SMP 下单-reader 契约不成立）；`_ft_res` 变体的 fault 路径 cleanup 显式释放预约（longjmp 不遗留）。spec §6 "pipe 经 bounce 无数据丢失" 语义由"peek 不消费 + fault 时 tail 不动"满足。为此 `task_t` 新增 `fault_cleanup`/`fault_cleanup_arg` 字段 + `copy_*_ft_res` 原语（Task 1/2）。

**Files:**
- Modify: `kernel/fs/file.c`（`fd_read`/`fd_write` 分块 bounce + socket rx `_ft` / tx 分块 bounce + **pipe `read_busy` 预约序列化**）
- Modify: `kernel/include/kernel/file.h`（`pipe_t` 加 `read_busy` + `read_busy_lock` + `read_busy_wq` 字段）

**Interfaces:**
- Consumes: `UACCESS_BOUNCE_SIZE`（Task 1）、`copy_to_user_ft`/`copy_from_user_ft`/`syscall_check_user_range`（Task 2）、`copy_to_user_ft_res`/`copy_from_user_ft_res`（Task 2）
- Produces: `fd_read`/`fd_write` 分块语义（offset 只在 `_ft` 成功后推进；`submitted==0` → `-EFAULT`）；pipe 读端并发由 `read_busy` 预约序列化 + `_ft_res` fault cleanup 释放

- [ ] **Step 1: `fd_read` 分块 bounce（file.c:485-567）**

  ```c
  // Pattern: FS writes kernel bounce, then _ft copies to user. Offset
  // advances ONLY after a successful _ft copy. submitted==0 -> -EFAULT.
  // Applies to FD_VFS/FD_DEV (vfs_read), FD_PIPE/PTY (pipe_read_internal),
  // FD_SOCKET (netconn_recv) — each path's "copy into user buf" becomes:
  ssize_t put_user_chunk(void *user, const void *kbuf, size_t n,
                         uint64_t *offset, uint64_t *committed)
  {
      ssize_t rc = copy_to_user_ft(user, kbuf, n);
      if (rc < 0)
          return (*committed == 0) ? -EFAULT : (ssize_t)*committed;
      *offset += n; *committed += n;
      return 0;
  }
  ```
  - **VFS/DEV 同步读**：`kbuf = kmalloc(UACCESS_BOUNCE_SIZE)`；循环 `n = vfs_read(f->node, f->offset, min(size-committed, BOUNCE), kbuf)` → `put_user_chunk`；n==0 结束；`kfree`。
  - **pipe/PTY 读——`read_busy` 内核预约 + `_ft_res` cleanup（并发正确 + fault 清理预约）**：`fork`/`dup` 共享读端 + 默认 SMP 下两个独立 task 可能并发读同一 pipe——**单-reader 契约不成立**（非强制）。内核强制的预约序列化 + fault 路径显式释放：
    ```c
    /* pipe_t: + int read_busy; + wait_queue_t read_busy_wq;   (per-pipe) */
    /* acquire: wait until !read_busy, then set it (kernel-enforced — a
       second reader BLOCKS, it does not race tail) */
    static void pipe_read_reserve(pipe_t *p) {
        for (;;) {
            uint64_t f = spin_lock_irqsave(&p->read_busy_lock);
            if (!p->read_busy) { p->read_busy = 1; spin_unlock_irqrestore(&p->read_busy_lock, f); return; }
            /* block on read_busy_wq until the active reader finishes (its
               normal or fault cleanup releases read_busy + wakes us) */
            spin_unlock_irqrestore(&p->read_busy_lock, f);
            wait_queue_add(&p->read_busy_wq); schedule(); ...  /* wait + re-check */
        }
    }
    static void pipe_read_release(void *arg) {           /* lock-protected IDEMPOTENT:
        only clear+wake when read_busy==1, so double-release (fault cb + out_release)
        is safe by construction, not by comment */ 
        pipe_t *p = arg;
        uint64_t f = spin_lock_irqsave(&p->read_busy_lock);
        if (p->read_busy) {
            p->read_busy = 0;
            spin_unlock_irqrestore(&p->read_busy_lock, f);
            wake_all(&p->read_busy_wq);                  /* wake outside the lock */
        } else {
            spin_unlock_irqrestore(&p->read_busy_lock, f);
        }
    }
    /* in pipe_read_internal — SINGLE out_release exit (all paths, not just
       page-fault: EOF / -EINTR / read-write state change / wait anomalies all
       release the reservation, since copy_to_user_ft_res only handles the
       page-fault longjmp path): */
    struct pipe_read_rsv { pipe_t *p; int *released; };
    static void pipe_read_release_cb(void *arg) {        /* _ft_res fault callback */
        struct pipe_read_rsv *r = arg;
        *r->released = 1;                                /* mark released — out_release
                                                           must NOT double-release */
        pipe_read_release(r->p);                         /* read_busy=0 + wake (idempotent) */
    }
    int64_t pipe_read_internal(pipe_t *p, void *buf, uint64_t size) {
        uint8_t bounce[PIPE_SIZE];
        struct pipe_read_rsv rsv = { p, &(int){0} };
        int released = 0;  rsv.released = &released;
        pipe_read_reserve(p);                            /* acquire read_busy (may block) */
        int64_t result = 0;
        for (;;) {
            /* ... block-for-data loop (reservation held) ... */
            /*  EOF:            result = 0;      goto out_release; */
            /*  -EINTR:         result = -EINTR; goto out_release; */
            /*  data available: break; */
        }
        uint64_t f = spin_lock_irqsave(&p->lock);
        size_t avail = pipe_avail(p);
        memcpy(bounce, ring + p->tail, avail);           /* peek, no consume */
        spin_unlock_irqrestore(&p->lock, f);
        ssize_t rc = copy_to_user_ft_res(buf, bounce, avail,
                                         pipe_read_release_cb, &rsv);
        if (rc < 0) { result = -EFAULT; goto out_release; }   /* fault cb set released=1 */
        f = spin_lock_irqsave(&p->lock);
        p->tail = (p->tail + avail) % PIPE_SIZE;         /* COMMIT FIRST */
        pipe_wake_writers(p);
        spin_unlock_irqrestore(&p->lock, f);
        result = (int64_t)avail;
    out_release:
        if (!released) pipe_read_release(p);             /* every path exits here ONCE;
                                                           fault cb already released -> skipped */
        return result;
    }
    /* 时序：commit 后才释放（out_release 在 commit 后）→ reader 2 只能预约 commit 之后
       tail，无竞争。fault 路径 cb 释放一次（无 commit、tail 未动）+ released 标志防止
       out_release 二次释放。EOF/-EINTR/状态变化全部经 out_release 释放 → read_busy 不泄漏。 */
    ```
    **并发正确**：`read_busy` 使同时最多一个 read 在 peek→copy→commit 窗口内 → tail 无竞态（第二个 reader 阻塞等待预约释放，不竞争）。**fault 清理**：`copy_to_user_ft_res` 的 longjmp 路径运行 `pipe_read_release`（回到原内核栈，正常 IRQ 态，可安全 spin_lock）→ 预约不泄漏；tail 未动 → 数据不丢。**无裸 memcpy、无 `_ft` 持锁**。`pipe_read_internal` 的字节循环（file.c:399-466 `dst[total++]`）改为该模式。
    **替代方案（若不想加 `_ft_res`）**：显式收窄语义并在 fd/pipe 层阻止共享读端——但 `cmd1 | cmd2` 管道依赖 fork 继承读端，**不可行**。故选 `_ft_res` 预约方案。
  - **tty_read**（tty.c，`schedule()` 后写 buf）：同 socket RX——最终排水（canon/ring → buf）在无锁处，**`copy_to_user_ft` 后置**（post-block）；`_ft` 失败 → `-EFAULT`，已排水的数据留在 tty 缓冲下次可重读（tty ring 不因失败丢失）。
  - **socket RX**（file.c:513-563）：`copy_to_user_ft(buf, data, copy)` **成功后**才 `s->rx_off += copy` / 推进 netbuf / 释放（525-535、543-555 现有"先 memcpy 后推进"改为先 `_ft` 后推进；`_ft` 失败 → 返回 `-EFAULT`，rx_off/netbuf 原样保留可重试）。
  - 块间 fault：返回 `committed`（前块已提交字节，offset 已推进）；首块 fault：`-EFAULT`。

- [ ] **Step 2: `fd_write` 分块 bounce（file.c:651-710）**

  ```c
  // Read user into kernel bounce, then hand kernel to VFS/lwIP.
  ssize_t get_user_chunk(void *kbuf, const void *user, size_t chunk,
                         uint64_t *committed)
  {
      ssize_t rc = copy_from_user_ft(kbuf, user, chunk);
      if (rc < 0)
          return (*committed == 0) ? -EFAULT : (ssize_t)*committed;
      return (ssize_t)chunk;
  }
  ```
  - **VFS/DEV 同步写**：`kbuf = kmalloc(BOUNCE)`；循环 `chunk = min(size-off, BOUNCE)` → `get_user_chunk` → `vfs_write(node, f->offset, chunk, kbuf)` → offset += 实际写字节（短写由 vfs_write 返回值定）。
  - **pipe/PTY 写**（pipe_write_internal：file.c:570）：`copy_from_user_ft(kbuf, user, chunk)` **先于持锁**（无锁，fault-safe）→ 锁内 kbuf→ring + 推进 head → 解锁（现有 `src[total++]` 改为搬内核 bounce 后入 ring）。写侧天然"先拷后消费"，无丢数据问题。
  - **socket TX**：**分块 bounce**——`copy_from_user_ft(kbuf, user+off, min(remaining, 16KB))` → `netconn_write_partly(conn, kbuf, chunk, ...)`（lwIP 永不触用户内存）；`netconn_write` 保持阻塞语义，用户数据已搬内核。备选（不推荐）：setjmp 包整次 netconn_write（pbuf 泄漏风险，否决）。

- [ ] **Step 3: 构建 + 回归**
  ```bash
  make clean && make && make run
  ```
  QEMU：systest 184/0 + **网络 harness（make test-network）** + ash `cat` 大文件/管道 + busybox `dd`/`cp` 大文件（验证分块）+ wget（socket TX bounce）。

- [ ] **Step 4: Commit**
  ```bash
  git add kernel/fs/file.c
  git commit -m "feat(uaccess): Cat C VFS bounce chunk layer — FS never derefs user ptr

  fd_read/fd_write chunk through UACCESS_BOUNCE_SIZE kernel buffers;
  offset advances only after successful _ft copy (submitted==0 -> -EFAULT,
  else short count, data not lost). socket tx chunked bounce (lwIP never
  touches user memory)."
  ```

---

## Task 9: systest hostile 组（完整 E2E）

**Status:** ✅ done — un-parked 2026-08-24. The exec-ENOEXEC crash was a
`do_page_fault` demand-paging align bug (`PAGE_4K_ALIGN` rounds up;
fixed to `cr2 & PAGE_4K_MASK`). See
`2026-08-24-fix-task9-exec-enoexec-leak.md`. Full result: `227 passed, 0 failed`.

**Files:**
- Modify: `user/systest.c`（+hostile 组，~15 断言）

**Interfaces:**
- Consumes: 全部原语（Tasks 1-8 已完成），验证端到端
- Produces: 回归门禁（H1-H11）

- [ ] **Step 1: 写全部 hostile 用例（fork 子进程跑敌意 syscall，父断言子正常退出 + 内核存活）**

  | 用例 | 断言 |
  |---|---|
  | H1 `read(fd,0x1,16)` / `write(fd,0x1,16)` | -EFAULT |
  | H2 `nanosleep` 阻塞中被信号打断 → rem 写 | -EFAULT/EINTR 不崩 |
  | H3 `open(NULL,0)` / `chdir(0x1000)` / `access(NULL,...)` | -EFAULT |
  | H4 `read(fd,buf,1<<40)` / `getcwd(buf, 1<<40)` / 超长 | -EFAULT |
  | H5 `signal`/`sigprocmask` 坏 act/set/oldset 指针 | -EFAULT |
  | H6 `waitpid` 坏 status（子先退） | -EFAULT 不崩 |
  | H7 poll/select/connect/socket 坏指针 + 超长 nfds | -EFAULT |
  | H8 exec 坏 argv 元素 / 无 NUL 字符串 | -EFAULT |
  | H9 ioctl 坏 arg（TCGETS 到 0x1） | -EFAULT |
  | H10 `pipe` 坏 fds + fd 计数回查 | -EFAULT 无泄漏 |
  | H11 sigreturn 跨页（Task 7 已建）+ 信号投递写栈 E2E | 不崩/正确 |
  | munmap 后 read/write 该区 | -EFAULT |
  | 路径页尾 NUL、下一页未映射 | 正常 open（不过度拒绝） |

  每个用例：**child 必须断言真实返回值**（不只"不崩"）——敌意调用在 child 里做，child 检查返回值后按结果退出：
  ```c
  int64_t pid = fork();
  if (pid == 0) {
      int r = (int)read(fd, (void *)0x1, 16);     // H1 例
      _exit(r == -EFAULT ? 0 : 1);                // 必须是 -EFAULT；返回 0/其他 → 子退出 1 → 父断言失败
  }
  int st; waitpid(pid, &st, 0);
  CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0,
         "h1_read_bad_ptr", "read(0x1) == -EFAULT, child survives");
  ```
  这样错误地返回成功（如 0）也会暴露——不是"不崩就过"。**H1/H3/H4/H6/H9/H10** 断言 `== -EFAULT`；H8 断言 `< 0`；H2/H7/H11 断言 `-EFAULT` 或 `-EINTR`/`-EAGAIN`（按 syscall 语义）。

- [ ] **Step 2: 跑测试（GREEN）**
  ```bash
  make clean && make && make run
  ```
  预期：184 + hostile 组全过；内核无 PF-KRN dump。**任一用例崩溃 → 回到对应 Task 修**。

- [ ] **Step 3: Commit**
  ```bash
  git add user/systest.c
  git commit -m "test(uaccess): systest hostile-pointer group (H1-H11, 15+ asserts)

  fork children call syscalls with NULL/low/unmapped/kernel-range/oversized
  pointers; parent asserts child exits normally (no signal) => kernel survives.
  pipe rollback leak check via fd count. Path page-tail NUL not over-rejected."
  ```

---

## Task 10: 回归 + applet 复查 + 文档同步

**Files:**
- Modify: `docs/syscall.md`、`docs/roadmap.md`

**Interfaces:**
- Consumes: 全部

- [ ] **Step 1: 全量回归**
  ```bash
  make clean && make && make run        # systest 全量（184 + hostile）
  make test-network                     # 网络 harness
  ```
  QEMU ash 手工：`ls/cat/dd/cp/echo/pipe`、Ctrl-C 杀前台、wget、`exec /bin/tetris` 可玩。

- [ ] **Step 2: applet user-fault 复查**
  核对 `docs/applet-verification.md` §💥：`nl` 是否仍在 user-fault 崩溃；若内核侧已无 PF-KRN，确认 nl 是纯 libc 缺陷（保持 ❌）还是被本审计修复（改 ✅ 并记录）。

- [ ] **Step 3: 文档同步**
  - `docs/syscall.md`：补 uaccess 原语、`-EFAULT` 边界语义、hostile 测试说明。
  - `docs/roadmap.md`：P1「syscall 边界审计」标记 ✅，补实现摘要（commit 范围）。

- [ ] **Step 4: Commit**
  ```bash
  git add docs/syscall.md docs/roadmap.md
  git commit -m "docs: syscall boundary audit complete — P1 item done (systest N/N)"
  ```

---

## Self-Review（对照 spec）

- **Spec §3 校验原语** → Task 1（walker）+ Task 2（check_user_range）✅
- **Spec §4 _ft 原语 + Clang ABI** → Task 2（已验证类型/常量契约）✅
- **Spec §4 硬约束** → Global Constraints + Task 2 注释 + Task 7（投递路径无锁）✅
- **Spec §5 矩阵 71 项** → Task 4（A）/5（A'）/6（B）/8（C）/Task 6+7（特殊）；D 类不动 ✅
- **Spec §5.2 旧指针替换**（chdir path[0]、open O_CREAT）→ Task 4 Step 1 ✅
- **Spec §5.3 信号投递写栈** → Task 7 ✅
- **Spec §6 错误码/回滚**（waitpid 无条件回收、pipe 只 fd_close×2、submitted==0→-EFAULT、sigreturn -EFAULT）→ Task 6/7/8 ✅
- **Spec §7 VFS bounce 数据流** → Task 8 ✅
- **Spec §8.1 selftest**（合成页表 + 临时 addr_limit + 跨页非连续物理页）→ Task 3 ✅
- **Spec §8.2 systest hostile**（H1-H11 + 信号投递 E2E 的 RSP 保存/恢复）→ Task 7/9 ✅
- **Spec §8.3 回归 + applet** → Task 10 ✅
- **Spec §9 文件清单** → Tasks 1-10 覆盖全部 14 文件 ✅（额外：`kernel/include/kernel/file.h` pipe_t 字段 + `task_t` cleanup 字段）
- **Spec §12 实施顺序** → Task 0-10 与 Step 0-9 一一对应 ✅

**超出 spec 的补充（plan review 驱动，均已入 Task）**：① pipe 读端并发从"bounce+`_ft`"升级为 **`read_busy` 内核预约序列化 + `copy_*_ft_res` fault cleanup**（spec §5.1/§6 的 pipe 写法据此修订——并发 reader 正确性 + fault 预约清理）；② `check_user_range` fail-closed（boot 上下文安全）。

**类型一致性核查**：`os01_jmp_buf`（Task 1 定义，Task 2/3 使用）✅；`copy_to_user_ft`/`copy_from_user_ft` 签名 `(void*, const void*, size_t)->ssize_t` 全程一致 ✅；`strnlen_user(const void*, size_t)->int` 一致 ✅；`check_user_range(uint64_t, uint64_t, bool)->bool` 一致 ✅；`arch_user_range_accessible(void*, uint64_t, uint64_t, bool)->bool` 一致 ✅。

# 用户堆与 ELF 映射隔离设计（PT_LOAD 全 4 KiB）

> 日期：2026-10-01
> 状态：待审阅的设计；实现计划和代码变更须在本 spec 获批后另行开展。
> 范围：x86_64 用户态 ET_EXEC 装载、`brk`、VMA/页表、fork/exec/退出及相邻映射接口。

## 1. 意图与成功标准

当前 `elf_load()` 为 PT_LOAD 分配可写的 2 MiB 大页，`spawn_user_task()` 和 `sys_exec()` 从 `ALIGN_UP(end_code, 4 KiB)` 设置 break，却为整个剩余 16 MiB 窗口预建可写 heap VMA。`SYS_brk` 只改 `end_brk`。ELF 尾页所在的大页可能跨入未申请的堆；即使缩短 heap VMA，CPU 仍可通过已存在的大页 PTE 写入那里。

本设计的成功条件是：

1. 每个 PT_LOAD 覆盖的页均使用独立的 4 KiB 物理页和 PTE；ELF 与堆在 4 KiB 边界分离。当前 break 之外的完整堆页没有用户映射；收缩后解除整页映射，再增长时得到清零的页。
2. 现有 `malloc`、用户缓冲区系统调用、fork、exec、退出及 16 MiB 图形缓冲场景继续工作；包括 fork 后内核向 COW 堆页写入的情形。任何分配失败均不发布部分初始化的映像或破坏父进程地址空间。
3. 固定映射、`munmap` 和 `mprotect` 不能改动内核管理的 ELF、堆保留区或栈；VMA 与页表不会对同一物理页重复回收。

隔离粒度为 4 KiB。ELF 最后一页中 `p_memsz` 结束后的余字节、已申请堆最后一页中 break 之后的余字节仍属于同一可访问页；本设计不声称字节级越界检测。NX、全面按 ELF 段设置执行/写权限、PIE/ASLR、aarch64 用户态及 libc 分配器加固属于独立工作。

## 2. 现状约束

| 路径 | 当前行为及设计影响 |
|------|--------------------|
| `kernel/fs/elf.c:elf_load` | 逐个 PT_LOAD 使用 2 MiB 映射；`MAX_LOAD_PAGES` 跟踪回滚，成功映射不建 VMA。须改为跨页装载、同页段复用及按 4 KiB 页回滚，不再选择大页粒度。 |
| `kernel/sched/task.c:spawn_user_task/sys_exec` | 两处分别初始化 break、预建整个剩余窗口的 VMA；须汇成同一初始化契约，且在发布新映像前处理失败。 |
| `kernel/arch/x86_64/intr/trap.c:SYS_brk` | 当前仅检查范围并写 `end_brk`；需使 break、heap VMA 和叶子 PTE 同步变更。 |
| `kernel/memory/vma.c` | `vma_insert` 不检查重叠；`MAP_FIXED` 先调用 `do_munmap_locked`，`mprotect` 可改 VMA；保护区检查须在任何拆映射或设备回调前完成。 |
| 用户缓冲区写入 | `user_write_range_begin` 要求目标 PTE 已存在且可写，`copy_to_user_ft` 遇内核态缺页返回 `-EFAULT`；因此新增堆整页须在 `brk` 成功前预先映射，fork 后 COW 页还须在内核写入前私有化。 |
| `fork_mm_copy` | 2 MiB 页私有复制，4 KiB 可写页 COW，只读页直接共享；部分 OOM 路径继续共享页表或 `mm`。新的 ELF 4 KiB 页必须有明确的 fork 和释放规则。 |
| `vma_free_all` / `vmm_free_user_map` | 前者释放 VMA 内的 4 KiB 页，后者遍历页表释放余下映射；全部 ELF 页保持无 VMA，由后者单独拥有。 |

当前地址常量：`USER_CODE_ADDR = 0x400000`，`USER_PAGE_SIZE = 0x1000000`，`USER_STACK_BASE = 0x1400000`；现有 `brk` 最大返回值为 `0x13ff000`，在栈前保留 4 KiB 未映射页。保留当前 `SYS_brk` 约定：`brk(0)` 返回当前值；成功返回新值；失败返回负错误码。此次不改用户态 ABI。

## 3. 方案选择

| 方案 | 优点 | 代价与结论 |
|------|------|------------|
| A：将堆起点上移到下一个 2 MiB 边界 | ELF 装载器改动较少 | 每个进程可能损失接近 2 MiB 的有限堆窗口，仍须改 VMA、`brk` 和 fork；不选。 |
| B：全部 PT_LOAD 改 4 KiB 页 | 单一 ELF 页粒度，边界、稀疏段与回滚模型统一 | 增加 PTE 与 TLB 压力；稀疏段可减少物理页浪费。**选用。** |
| C：边界 4 KiB 化，安全的大页保留 | 可保留部分 ELF 大页的 TLB 优势 | 装载器、fork 和回滚都须处理混合页粒度，增加正确性分支；不选。 |

**选择 B。** 所有 PT_LOAD 都只建立 4 KiB 叶子 PTE，不保留 ELF 大页，也不把“以后恢复大页”作为本次交付条件。每覆盖 2 MiB 最多增加一个 4 KiB PTE 表和 512 个叶子项；只为实际覆盖的 ELF 页分配物理页。用户栈现有的 2 MiB 映射不属于 PT_LOAD，本设计不改变它。ELF 页权限沿用本次变更前的兼容语义；按 PF_W/PF_X 细化权限另列工作。

## 4. 地址模型与不变量

先完整预扫描程序头，再计算：

```text
elf_lo       = min(PT_LOAD.p_vaddr)
elf_end      = max(PT_LOAD.p_vaddr + PT_LOAD.p_memsz)   # 半开区间末端
heap_base    = ALIGN_UP(elf_end, 4 KiB)
heap_limit   = USER_CODE_ADDR + USER_PAGE_SIZE - 0x1000 = 0x13ff000
heap_present = [heap_base, ALIGN_UP(end_brk, 4 KiB))
heap_reserved= [heap_base, heap_limit)
stack        = [USER_STACK_BASE, USER_STACK_BASE + 2 MiB)
```

`heap_limit` 是 break 可达到的最大值，`[heap_limit, USER_STACK_BASE)` 始终是 4 KiB guard 页。`heap_base <= end_brk <= heap_limit`。初始 `end_brk = heap_base`，heap VMA 表示零长度范围；可使用显式的 `VM_HEAP` 标记或 `mm` 所属指针识别唯一 heap VMA，不能靠“列表中第一个匿名 VMA”猜测。若选择零长度 VMA，VMA 遍历和 fork 必须正确保留它，且 `vma_find` 不会命中它。

ELF 预扫描必须拒绝：无可装载段、`p_filesz > p_memsz`、程序头或文件数据越界、任何加法/向上取整溢出、段落入用户下界之外或越过 `heap_limit`、堆初始位置超过 `heap_limit`、入口点不在可执行 PT_LOAD 覆盖范围内、同一虚拟字节被不同段赋予冲突的文件内容。允许同一 4 KiB 页包含相邻段，按段区间填入数据并清零 BSS；共享页只分配一次，未被任何段覆盖的 4 KiB 页不映射。加载前完成所有可静态验证的检查；加载中遇到读错或 OOM 时回滚全部已建映射。

页表所有权以叶子页为准：heap VMA 范围内的 4 KiB 页由 VMA 清理；无 VMA 的 ELF 4 KiB 页由 `vmm_free_user_map` 清理；设备页与用户栈遵循各自现有规则。页表遍历遇到已清零的 PTE 必须跳过，不能重复释放。任何用户 PTE 不得同时归属于 ELF 与堆。映像切换后，旧映像资源仅在不能再被用户线程访问时释放。

## 5. 组件行为

### 5.1 ELF 装载与进程建立

`elf_load` 先形成经过验证的 PT_LOAD 页布局，再申请页表和数据页。装入每个 4 KiB 叶子前清零物理页，按文件区间复制，BSS 保持为零；跨页读写须按页边界拆分，多个段触及同一页时复用物理页。`MAX_LOAD_PAGES` 的 2 MiB 跟踪方式须替换为可覆盖全部 ELF 4 KiB 页的回滚机制。装载器维护本次创建的叶子映射清单或直接让待发布的独立 PGD 承担回滚；失败清理必须只执行一种所有权路径，避免装载器与调用者各释放一次。`elf_end` 从 `p_memsz` 计算，`heap_base` 向上对齐；ELF 映射不得含 `PAGE_HUGE` 叶子。

`spawn_user_task` 与 `sys_exec` 使用同一 heap 初始化函数：为新 `mm` 设置 `start_brk/end_brk`，创建唯一、初始零长度的 heap VMA；任何分配失败须释放待发布 PGD、VMA 与文件引用。`sys_exec` 保持旧映像可用直到新 ELF、heap VMA、用户栈及初始栈数据全部准备成功；之后一次性切换 `mm`/CR3，再清理旧映像。初始化代码不得忽略 heap VMA 分配失败。

### 5.2 `brk` 与堆页

`brk(0)` 在 `mm->lock` 保护下读当前值。设置 break 时检查整数范围和 `[heap_base, heap_limit]`。增长前，按 `[ALIGN_UP(old_brk, 4 KiB), ALIGN_UP(new_brk, 4 KiB))` 逐页分配、清零并映射；同页内增长无需新页。新页与必要的中间页表必须准备成功后才提交新 `end_brk` 和 heap VMA 终点。任何失败都撤销本次新增的 PTE/物理页并保持旧 break。这个预映射要求使 `malloc` 刚返回的缓冲区可直接用于 `read` 等内核写入系统调用。

收缩时，如果新 break 落在仍保留的页内，先将 `[new_brk, ALIGN_UP(new_brk, 4 KiB))` 清零；若该页为 COW，先取得私有副本，失败则旧 break 不变。随后撤除 `[ALIGN_UP(new_brk, 4 KiB), ALIGN_UP(old_brk, 4 KiB))` 的 PTE 并正确减少 COW 引用。完成跨 CPU TLB 失效后提交较小的 break/VMA 终点。用户态访问已撤除的整页必须得到缺页失败，不能因旧 VMA 或遗留大页重新获得可写页。仅页内超出 break 的地址仍可能可读写，这是第 1 节的明确限制。

`brk`、`munmap`、`mprotect`、`MAP_FIXED` 与会修改 PTE 的设备映射均遵循 `mm->lock` 的同一临界区。heap COW 的用户态缺页分支以保存中断状态的方式取得该锁，锁内复核 fault 地址处于当前已提交的 heap 范围、私有化页面并更新 PTE，释放锁后返回；不能在持锁时做文件 I/O、用户拷贝或会阻塞的分配。需核对 IST 上的锁序与所有退出路径，确保无递归或遗漏释放。heap 页已由 `brk` 预映射，普通无映射 fault 不得凭陈旧 VMA 创建新堆页。

内核向 fork 后的 COW 堆缓冲区写入时，用户态写 fault 不会发生。用户写入入口须先在 `mm->lock` 下检查目标区间，对其中的 COW 堆页执行私有化并刷新必要的 TLB，再进行原有权限检查和容错拷贝；OOM 时保持原页与引用计数不变并返回错误。此预处理也应覆盖 `syscall_check_user_range(..., writable=true)` 的预检路径，否则预检会在私有化前把合法堆缓冲区判为不可写。既有三阶段 pipe read 的资源预留与 `copy_to_user_ft_res` 故障回调语义仍需保留。

### 5.3 其他映射接口

`MAP_FIXED`、`munmap`、`mprotect` 只要请求的页对齐区间与 `[heap_base, heap_limit)` 相交，即返回错误并保持地址空间不变；对 ELF 已装载区与栈映射也采取相同的预检查。设备 mmap 回调只能在地址合法且与受保护区不相交后执行。检查必须先于 `do_munmap_locked`，防止 `MAP_FIXED` 先拆旧 VMA 再发现冲突。普通 mmap 的自动选址与显式 hint 不得落入保留区，算术溢出或越过用户上限时失败；非固定映射不能悄悄覆盖无 VMA 的 ELF 页。保护区用独立的 `mm` 范围元数据判断，不依赖当前 heap VMA 是否为空或 ELF 是否建 VMA。

### 5.4 fork/COW 与回收

fork 复制 break 值、保留区边界和 heap VMA。全部 ELF 4 KiB 页没有通用 VMA，采用物理页私有复制，包含只读页；用户栈现有大页仍按原路径私有复制。已驻留的可写 heap 页沿用 COW，但父/子 PTE、引用计数、TLB 必须成对更新。其他 4 KiB 只读匿名/文件页也私有复制，避免在没有共享所有权计数时由父子分别释放；既有可写 VMA 页保持 COW 语义。`VM_IO` 按设备现有共享规则处理。

fork 必须先完成子页表、VMA 和需私有复制的页的全部可失败分配，再改变父页表的写权限或增加 COW 引用。若后续仍可能失败，须有精确回滚并恢复父 PTE/refcount/TLB；不得使用当前 OOM 时共享父 PDE 或整个 `mm` 的退化路径。失败不创建可运行的子进程。

`exec` 和退出继续以“先 `vma_free_all`，后 `vmm_free_user_map`”清理：前者释放 heap 与普通 VMA 页并清零 PTE，后者释放无 VMA 的 ELF 页、其余用户页及页表。应验证 4 KiB ELF 页、空 heap VMA、COW heap 页、部分装载失败及 fork 失败的每一条清理路径均无泄漏、双重释放或活跃引用。页表撤销和写权限变化执行 SMP TLB 同步。

## 6. 错误语义与验证

ELF 格式或边界错误按现有调用者语义返回 `-ENOEXEC`；分配失败返回 `-ENOMEM`，保留原进程映像。`brk` 越界返回 `-ENOMEM`，低于 `start_brk` 返回 `-EINVAL`，分配失败返回 `-ENOMEM`；失败时查询 `brk(0)` 必须仍为旧值。映射 API 与受保护范围冲突返回 `-EINVAL`，且无部分副作用。实际实现可用内部错误码，但用户可见结果及原子性须保持一致。

| 层级 | 必须验证的行为 |
|------|----------------|
| host | ELF 跨 4 KiB 和原 2 MiB 边界、相邻段同页、稀疏段空洞、BSS、畸形/截断/溢出 ELF；所有 PT_LOAD 均为 4 KiB PTE、无 `PAGE_HUGE`，4 KiB 页回滚与所有权；`brk` 增缩及 OOM 原子性；fork 拷贝/COW 引用与失败回滚。测试真实边界计算和页表操作，避免只复刻实现公式。 |
| QEMU 用户态 | 初始 `heap_base` 后整页不可访问；`brk` 增长后页面可写且零填充，并能作为系统调用输出缓冲；收缩整页后访问失败，再增长重新清零；同页内收缩再增长不显露旧尾部数据。 |
| QEMU 隔离 | ELF 尾页与首个堆页互不影响；`MAP_FIXED`、`munmap`、`mprotect` 无法触碰 ELF/堆/栈保护区且失败后旧映射仍可用；非固定 mmap 与 heap 保留区不重叠。 |
| QEMU 生命周期 | 父子 heap COW 互不污染；fork 后将尚未由用户态写过的 heap 页直接作为 `read` 输出缓冲；ELF 尾页 fork 后独立；多次 fork/exec/退出与故障注入无双重释放、泄漏或共享可写页表。 |
| 集成 | `make clean`（若修改 `mm_t`/`vma_t` 等结构，强制执行）后运行 `make test-host`、`make OS01_SYSTEST=1 test-syscall`，另行运行内核自测；验证 init、BusyBox、terminal、Tetris 及 16 MiB 堆使用。 |

不在 syscall suite 中同时启用 `KERNEL_SELFTEST=1`；仓库说明指出它会干扰 systest 的 fork/exec/waitpid。以上均为实施后的验收门槛，撰写本 spec 不代表这些测试已经运行。

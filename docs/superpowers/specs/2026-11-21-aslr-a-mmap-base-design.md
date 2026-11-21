# ASLR-A：x86_64 用户态 mmap 基址随机化设计

| 字段 | 值 |
|---|---|
| 对应路线图项 | `docs/roadmap.md` P1 安全加固 — `ASLR-A：mmap 基址` |
| 日期 | 2026-11-21 |
| 适用范围 | x86_64 用户态地址空间建立与 `mmap` 选址（`kernel/include/arch/x86_64/{user_layout.h,aslr.h}`、`kernel/arch/x86_64/memory/{aslr.c,mmap.c}`、`kernel/arch/x86_64/sched/{exec.c,fork.c}`、对应测试与审计） |
| 前置依赖 | `kernel_random_get_strong()`（`kernel/random/random.c`）、x86_64 堆/ELF 保护窗口（`kernel/include/arch/x86_64/user_layout.h`、`mm_user_range_protected()`）、staged `spawn_user_task`/`sys_exec` 生命周期 |

---

## 1. 目标与非目标

### 1.1 目标

1. **每次新建 x86_64 用户地址空间时随机化 `mm->mmap_base`**：`spawn_user_task()`（含 PID 1 `/bin/init` 与所有内核拉起的用户进程）和 `sys_exec()` 在准备新 `mm_t` 时，从内核 STRONG-only 熵接口 `kernel_random_get_strong()` 独立获取随机字节并派生 4 KiB 页对齐的 `mm->mmap_base`。
2. **`fork` 继承既有布局**：`do_fork()` / `fork_mm_copy()` 原样继承父进程的 `mm->mmap_base`、已建 VMA 与页表布局，不消耗熵、不重随机化；仅当子进程后续 `exec` 时重新随机化。
3. **明确 STRONG 熵不可用时的 fail-closed 语义**：无 STRONG 熵（WEAK-only 或 NONE 池且硬件无 RDSEED）时，新地址空间建立直接以 `-EAGAIN` 失败并完整回滚，禁止静默回退到固定基址 `0x40000000` 或降级到 WEAK / 周期计数器伪熵。
4. **保留 `MAP_FIXED` 语义并加固地址窗口、冲突与溢出处理**：保留 `MAP_FIXED` 的精确选址与原子替换语义，同时封堵 `MAP_FIXED` 在 `addr == 0` 时绕过下界映射零页的漏洞，使 `do_mmap()` / `do_munmap()` / `do_mprotect()` 统一服从规范低半区页对齐上界，并使非固定间隙搜索严格限定在 `[search_start, effective_limit)` 内，正确规避任意位置的已有 VMA、拒绝溢出与窗口耗尽（不回绕到 `mmap_base` 以下）。

### 1.2 非目标

- **不实现 `ET_DYN` / PIE 加载偏置**（由后续 `ASLR-B` 单独设计与验收）。
- **不实现用户栈或堆基址随机化**：用户栈仍固定在 `[0x20400000, 0x20600000)`，堆仍从页对齐 `elf_end` 增长至 `0x203ff000`；完成 ASLR-A 不构成完整用户态 ASLR。
- **不扩展到 aarch64**：`kernel/memory/mm.c` 保持双架构共享与确定性默认值 `0x40000000`；aarch64 的 `user_mm.c` / `user_vm_policy.c` 零改动。
- **不做单次 `mmap()` 级离散打散**：同一地址空间内，非固定 `mmap` 仍从该地址空间的 `mm->mmap_base` 起向上 first-fit 查找空隙。
- **不复用 `AT_RANDOM` 载荷**：`mm->mmap_base` 与 `AT_RANDOM` 分别独立调用 `kernel_random_get_strong()`，互不派生。
- **不新增运行时开关或新 UAPI 标志**：不引入 `personality` / `ADDR_NO_RANDOMIZE` / `MAP_FIXED_NOREPLACE`，不改变 `mm_t` 结构体布局（复用已有 `uint64_t mmap_base` 字段）。

---

## 2. 候选方案比较与决策

### 2.1 随机化地址窗口与熵宽

| 方案 | 窗口范围 | 熵位（4 KiB 页） | 评估 |
|---|---|---|---|
| A. 32 位低址窗口 | `[0x40000000, 0x80000000)`（1 GiB） | 18 bit（262K 槽） | 熵宽不足，浪费 47 位规范用户地址空间，上方逼近旧固定测试区间 |
| **B. 64 位高规范低半区 28-bit 窗口（采用）** | **`[0x0000400000000000, 0x0000410000000000)`（64 TiB..65 TiB，1 TiB 跨度）** | **28 bit（`2^28 ≈ 2.68e8` 槽）** | 对齐 Linux x86_64 默认 `ARCH_MMAP_RND_BITS=28`；2 的幂槽位数零取模偏差；下方保留约 64 TiB 给现有固定区、低址 `MAP_FIXED` 与未来 PIE/栈随机化；上方保留约 63 TiB（至 `0x00007FFFFFFFF000`）供向上 first-fit 增长，正常负载不会耗尽 |
| C. 接近 128 TiB 顶部并改为自顶向下搜索 | `[0x00007ef000000000, 0x00007ff000000000)` | 28 bit | 需重写整个 `mmap_find_gap` 方向与全部向上遍历假设，增加无关复杂度且收益与方案 B 相同 |

采用 **方案 B**：保持既有向上 first-fit 方向，将随机基址窗置入 `[64 TiB, 65 TiB)`。页表开销方面，单个进程在 `l0 ∈ [128, 129]` 下首次触发缺页仅多分配 1 组 PUD/PMD/PT（12 KiB）；x86_64 全链路（`vmm_pt_walk`、`vmm_free_user_map`、`fork_mm_copy`、`arch_virt_to_phys`、`arch_user_range_accessible`、`uaccess.c`、`futex.c`、`/proc/<pid>/maps` 的 `%012lx` 与 `libc/unistd/mmap.c`）已原生覆盖 `l0 ∈ [0, 255]`。

### 2.2 随机化落点（`mm_alloc` vs x86 建像阶段）

- **否定在 `mm_alloc()` 中随机化**：`kernel/memory/mm.c` 由 x86_64 与 aarch64 双架构共用；且 `fork_mm_copy()` 先调 `mm_alloc()` 再 `memcpy(child_mm, parent_mm, sizeof(mm_t))`，若在 `mm_alloc()` 取熵，每次 `fork` 都会白白消耗 STRONG 熵并可能因熵不可用导致本应继承布局的 `fork` 失败，也会破坏 aarch64 隔离与现有宿主夹具的确定性。
- **采用 x86 专用建像初始化**：`mm_alloc()` 维持确定性默认值 `0x40000000`；新增 x86 私有模块 `kernel/arch/x86_64/memory/aslr.c`（头 `kernel/include/arch/x86_64/aslr.h`），仅在 x86_64 新地址空间建立路径（`spawn_user_task`、`sys_exec`）的 fallible 准备阶段显式调用 `x86_aslr_init_mmap_base()`。

### 2.3 STRONG 熵不可用时的失败语义

- **采用 Fail-Closed（返回 `-EAGAIN`）**：与 `docs/arch/entropy-source-facade.md` §0.2.3、§6 及 `setup_user_stack()` 的 `AT_RANDOM` STRONG-only 策略严格一致。
  - 若回退固定基址 `0x40000000`，会造成静默安全降级，使上层无法分辨进程是否受到 ASLR 保护。
  - 若降级使用 WEAK `get_random_bytes()` 或周期计数器，直接违反路线图和 entropy facade 禁止条款。
  - 由于同一次 `spawn_user_task` / `sys_exec` 中的 `setup_user_stack()` 已强制要求 `kernel_random_get_strong()`，fail-closed 不会破坏任何当前可启动的 STRONG 环境（UEFI `virtio-rng` GetRNG 或硬件 `RDSEED`），且在 WEAK/NONE 环境下保持一致的拒启行为（`init` fail-closed）。

### 2.4 随机基址上方空间耗尽策略

- **采用严格不回绕（返回 `-ENOMEM`）**：非固定 `mmap` 仅在 `[search_start, effective_limit)` 内搜索；找不到容纳 `length` 的空隙时直接返回 `-ENOMEM`，**禁止回绕**到 `mm->mmap_base` 以下或 `X86_USER_STACK_END`。
- 理由：在 63 TiB 上方窗口下，合法工作负载不会耗尽；不回绕可防止攻击者通过大映射或地址喷射迫使新映射回落到可预测的固定低地址（spray-to-wrap 绕过）。

---

## 3. x86_64 用户虚拟地址空间布局

全部 x86_64 运行期用户布局常量集中定义在 `kernel/include/arch/x86_64/user_layout.h`（零额外头文件依赖，仅用整数字面量宏，继续作为 x86 私有单源）。

### 3.1 常量定义

| 宏名 | 值 | 含义 | 状态 |
|---|---|---|---|
| `X86_USER_MIN_ADDR` | `0x400000UL` | 用户最低合法映射地址（4 MiB 空指针保护区上界） | 既有 |
| `X86_USER_HEAP_LIMIT` | `0x203ff000UL` | 堆上限（保护页下界） | 既有 |
| `X86_USER_STACK_BASE` | `0x20400000UL` | 2 MiB 用户栈基址 | 既有 |
| `X86_USER_STACK_SIZE` | `0x200000UL` | 用户栈大小（2 MiB） | 既有 |
| `X86_USER_STACK_END` | `0x20600000UL` | 用户栈排他上界（保护窗口右端点） | 既有 |
| `X86_USER_STACK_TOP` | `0x205ffff0UL` | 初始栈顶锚点 | 既有 |
| `X86_USER_ADDR_LIMIT` | `0x00007FFFFFFFFFFFULL` | x86_64 用户任务 `tsk->addr_limit` 单源常量（替代 `exec.c:427` 与 `fork.c:509` 的硬编码字面量；`fork.c` 需补充 `#include <arch/x86_64/user_layout.h>`） | 新增 |
| `X86_USER_MMAP_MAX_ADDR` | `0x00007FFFFFFFF000ULL` | `mmap`/`munmap`/`mprotect` 合法半开区间排他上界（`= X86_USER_ADDR_LIMIT & ~0xFFFULL`，留出顶页 `[0x7FFFFFFFF000, 0x800000000000)` 作为规范边界保护页） | 新增 |
| `X86_USER_MMAP_RND_BASE` | `0x0000400000000000ULL` | `mmap_base` 随机窗口起点（64 TiB） | 新增 |
| `X86_USER_MMAP_RND_BITS` | `28U` | `mmap_base` 4 KiB 页粒度随机位数 | 新增 |
| `X86_USER_MMAP_RND_SPAN` | `(1ULL << (X86_USER_MMAP_RND_BITS + 12U))` | 随机窗口跨度（`0x0000010000000000ULL` = 1 TiB） | 新增 |
| `X86_USER_MMAP_RND_END` | `(X86_USER_MMAP_RND_BASE + X86_USER_MMAP_RND_SPAN)` | 随机窗口排他终点（`0x0000410000000000ULL` = 65 TiB） | 新增 |

### 3.2 分区视图（半开区间）

| 虚拟地址区间 | 大小 | 用途与策略 |
|---|---|---|
| `[0x000000000000, 0x000000400000)` | 4 MiB | 空指针保护区：所有 `mmap`（含 `MAP_FIXED`）/ `munmap` / `mprotect` 一律 `-EINVAL` |
| `[0x000000400000, 0x0000203ff000)` | 512 MiB - 4 KiB | ELF 装载包络 + 堆预留区：`mm_user_range_protected()` 保护，禁止 `mmap`/`munmap`/`mprotect` |
| `[0x0000203ff000, 0x000020400000)` | 4 KiB | 堆→栈保护页：`mm_user_range_protected()` 保护 |
| `[0x000020400000, 0x000020600000)` | 2 MiB | 用户栈：`mm_user_range_protected()` 保护 |
| `[0x000020600000, 0x0000400000000000)` | ~64 TiB - 518 MiB | 常规低址区：允许显式 `MAP_FIXED`（如 `0x40000000`）；非固定随机进程不落入此区（留作未来 ASLR-B PIE / 栈随机化及遗留低址 `MAP_FIXED` 空间） |
| `[0x0000400000000000, 0x0000410000000000)` | 1 TiB | **`mm->mmap_base` 随机化窗口**（`2^28` 个 4 KiB 对齐槽位） |
| `[0x0000410000000000, 0x00007FFFFFFFF000)` | ~63 TiB - 4 KiB | 随机 `mmap_base` 上方的向上增长余量区；也允许显式 `MAP_FIXED` |
| `[0x00007FFFFFFFF000, 0x0000800000000000)` | 4 KiB | 规范用户空间顶保护页：拒绝 `mmap`（`-ENOMEM`）与 `munmap`/`mprotect`（`-EINVAL`） |
| `[0x0000800000000000, 0xFFFFFFFFFFFFFFFF]` | 非规范洞 + 内核半区 | 全部拒绝（`mmap` 越界 `-ENOMEM`、`munmap`/`mprotect` `-EINVAL`） |

### 3.3 编译期几何不变式

在 `kernel/arch/x86_64/memory/aslr.c` 中以 `_Static_assert` 强制钉死：

- `X86_USER_MMAP_RND_BASE >= X86_USER_STACK_END`
- `(X86_USER_MMAP_RND_BASE & 0xFFFULL) == 0`
- `X86_USER_MMAP_RND_BITS >= 20U && X86_USER_MMAP_RND_BITS <= 32U`
- `X86_USER_MMAP_RND_END > X86_USER_MMAP_RND_BASE`
- `X86_USER_MMAP_RND_END < X86_USER_MMAP_MAX_ADDR`
- `X86_USER_MMAP_MAX_ADDR <= X86_USER_ADDR_LIMIT`
- `(X86_USER_MMAP_MAX_ADDR & 0xFFFULL) == 0`
- `X86_USER_ADDR_LIMIT < 0x0000800000000000ULL`

---

## 4. 模块接口与熵处理契约

### 4.1 x86 私有接口（`kernel/include/arch/x86_64/aslr.h`）

```c
#ifndef _ARCH_X86_64_ASLR_H
#define _ARCH_X86_64_ASLR_H

#include <stdint.h>
#include <memory/mm.h>

uint64_t x86_aslr_derive_mmap_base(uint64_t raw_entropy);
int x86_aslr_init_mmap_base(mm_t *mm);

#endif /* _ARCH_X86_64_ASLR_H */
```

- **实现位置**：`kernel/arch/x86_64/memory/aslr.c`（由 `kernel/arch/x86_64/make.config` 的 `$(wildcard $(ARCHDIR)/memory/*.c)` 自动编入 x86 内核，绝不进入 aarch64 源清单）。
- **边界审计接线**：
  - `qemutests/mm_header_boundary.py`：在 `FORBIDDEN_MACROS` 新增 `_ARCH_X86_64_ASLR_H` 与 §3.1 的 6 个新增 `X86_USER_*` 宏，在 `FORBIDDEN_SYMBOLS` 新增 `x86_aslr_derive_mmap_base`、`x86_aslr_init_mmap_base`，防止 `<memory/*.h>` 公共头泄漏。
  - `mk/components/run.mk:170`（`test-aarch64-audit` ELF 符号排除门）：在禁止符号正则中追加 `x86_aslr_`。
  - `hosttests/cases/test_vmm_caller_audit.c:346-353`：在 aarch64 源清单排除断言中追加 `arch/x86_64/memory/aslr.c`。

### 4.2 函数契约

1. **`x86_aslr_derive_mmap_base(uint64_t raw_entropy)`**
   - 纯函数，无锁、无分配、无全局状态依赖。
   - 掩码 `mask = (UINT64_C(1) << X86_USER_MMAP_RND_BITS) - 1U`（即 `0x0FFFFFFFULL`），返回 `X86_USER_MMAP_RND_BASE + ((raw_entropy & mask) << 12U)`。
   - 保证返回值 4 KiB 对齐，严格落在 `[X86_USER_MMAP_RND_BASE, X86_USER_MMAP_RND_END)` 内。
   - 全零输入映射到 `X86_USER_MMAP_RND_BASE`（合法槽位，不跳过 0 槽以保持严格均匀分布）。

2. **`x86_aslr_init_mmap_base(mm_t *mm)`**
   - `mm == NULL` → 立即返回 `-EINVAL`，不调用 `kernel_random_get_strong()`。
   - 声明栈缓冲 `uint8_t entropy[32]`，调用 `kernel_random_get_strong(entropy)`。
   - 若返回 `false`（STRONG 熵不可用；注意 WEAK 回落路径下 `arch_random_get_entropy()` 可能已写入 DRBG 字节，故失败路径也必须显式擦除）：
     - 通过 `volatile uint8_t *` 显式擦除循环清零 `entropy[0..31]`（防止编译器死存储消除）。
     - **不修改 `mm->mmap_base`**（保持调用前原值）。
     - 返回 `-EAGAIN`。
   - 若返回 `true`：
     - 以显式小端字节移位从 `entropy[0..7]` 组装 `uint64_t raw`（无未对齐指针强转 UB）。
     - 立即通过 `volatile uint8_t *` 显式擦除全部 32 字节 `entropy`。
     - 写入 `mm->mmap_base = x86_aslr_derive_mmap_base(raw)`。
     - 返回 `0`。
   - **独立性保证**：`x86_aslr_init_mmap_base()` 与 `setup_user_stack()` 各自独立调用 `kernel_random_get_strong()`，严禁共享或截断 `AT_RANDOM` 缓冲。

---

## 5. 地址空间生命周期接线

| 路径 | 文件位置 | `mm->mmap_base` 行为 | 熵调用与失败语义 |
|---|---|---|---|
| `mm_alloc()` | `kernel/memory/mm.c` | 保持默认 `0x40000000` | 无熵调用（双架构共享，确定性） |
| `init_mm_setup()` | `kernel/init/userspace.c:49` | PID 0 内核 idle 任务，保持 `init_mm.mmap_base = 0` | 无熵调用 |
| `spawn_user_task()` | `kernel/arch/x86_64/sched/exec.c` | 在 `mm_init_user_heap(mm, mm->end_code)` 成功之后、`alloc_pages(...)` 分配用户栈之前调用 `x86_aslr_init_mmap_base(mm)` | 独立 STRONG 取熵；失败时释放 `tsk->files`（`files_unpin`）、`tsk->fpu_save`、`destroy_unpublished_user_mm(mm)`、`mm`/`thd`/`raw_alloc`，返回 `-EAGAIN`；不持 `task_list_lock`/`rq_lock`，不发布任务；同时统一 `tsk->addr_limit = X86_USER_ADDR_LIMIT` |
| `sys_exec()` | `kernel/arch/x86_64/sched/exec.c` | 在 `mm_init_user_heap(new_mm, new_mm->end_code)` 成功之后、`alloc_pages(...)` 之前调用 `x86_aslr_init_mmap_base(new_mm)` | 独立 STRONG 取熵；失败时调用 `destroy_unpublished_user_mm(new_mm)` 并 `kfree(new_mm)`，返回 `-EAGAIN`；`current->mm`（含原 `mmap_base`）、`CR3`、`comm` 与信号表完全不受影响 |
| `do_fork()` / `fork_mm_copy()` | `kernel/arch/x86_64/sched/fork.c` | 沿用既有 `memcpy(child_mm, parent_mm, sizeof(mm_t))` 原样继承 `parent_mm->mmap_base` | **不调用**熵接口、不重随机化；补充 `#include <arch/x86_64/user_layout.h>` 并统一 `tsk->addr_limit = X86_USER_ADDR_LIMIT` |

**顺序兼容性**：在 `spawn_user_task()` 与 `sys_exec()` 中，将 `x86_aslr_init_mmap_base()` 放在 `mm_init_user_heap()` 与 `alloc_pages()` 之间，完整保持既有 staged 生命周期偏序（`elf_load < mm_init_user_heap < alloc_pages < setup_user_stack < task_list_lock / old_mm = current->mm`），既避免熵失败时浪费 2 MiB 栈页分配，也兼容 `test_process_image_lifecycle`（8 KiB 扫描窗与顺序断言）与 `qemutests/test_context_facade.py`。

**PID 1 `/bin/init` 启动**：`kernel/init/userspace.c:81` 通过 `spawn_user_task()` 创建 PID 1，因此在 STRONG 环境下 `/bin/init` 自动获得随机 `mmap_base`；在 WEAK/NONE 环境下 `spawn_user_task()` 返回 `-EAGAIN`，由 `boot_failure_stop("init", "/bin/init", init_pid, -1, -1)` fail-closed 停机，与既有 `qemutests/test_entropy_facade.sh` 契约一致。

---

## 6. `mmap` / `munmap` / `mprotect` 窗口、冲突与溢出契约

修改集中在 `kernel/arch/x86_64/memory/mmap.c`。

### 6.1 统一有效上界 `mmap_effective_limit()`

```c
static inline uint64_t mmap_effective_limit(uint64_t addr_limit)
{
    uint64_t limit = addr_limit & PAGE_4K_MASK;
    if (limit > X86_USER_MMAP_MAX_ADDR)
        limit = X86_USER_MMAP_MAX_ADDR;
    return limit;
}
```

- 任何 4 KiB 页对齐的半开区间 `[addr, addr + length)` 落在用户地址空间内，当且仅当 `addr < limit && length <= limit - addr`（等价于 `end <= limit` 且无溢出）。
- 生产用户态（`addr_limit = 0x00007FFFFFFFFFFF`）得到 `limit = 0x00007FFFFFFFF000`。
- 若测试将 `current->addr_limit` 缩小（如 `MUNMAP_BASE + 0x1000` 或 `MUNMAP_BASE + 0x800`），`limit` 精确反映缩小的页对齐上界；若测试将其设为 `0xffff800000000000` 或 `UINT64_MAX`，仍钳制在 `X86_USER_MMAP_MAX_ADDR`，永不接受顶保护页或非规范地址。
- 错误码保持既有约定：`do_mmap()` 越过 `limit` 返回 `-ENOMEM`；`do_munmap()` 与 `do_mprotect()` 越过 `limit` 返回 `-EINVAL`。

### 6.2 `do_mmap()` 参数与 `MAP_FIXED` 预检顺序（无副作用）

在获取 `mm->lock` 或任何分配/文件引用之前，按以下顺序校验：

1. `length == 0` → `-EINVAL`。
2. `rounded = PAGE_4K_ALIGN(length)`；若 `rounded < length`（向上页对齐发生 64 位回绕）→ `-EINVAL`。
3. 若 `addr != 0`：
   - `addr < X86_USER_MIN_ADDR` → `-EINVAL`。
   - `addr > UINT64_MAX - rounded`（`addr + rounded` 64 位溢出）→ `-EINVAL`。
4. 文件偏移校验：`offset & (PAGE_4K_SIZE - 1)` → `-EINVAL`；`!(flags & MAP_ANONYMOUS) && offset > UINT64_MAX - rounded` → `-EINVAL`；`MAP_ANONYMOUS` 且 `(int64_t)fd != -1` → `-EINVAL`。
5. `prot_to_vma_flags(prot, &vm_flags_base)` 失败 → 返回其 `-EINVAL`；`!current->mm` → `-EINVAL`。
6. 计算 `limit = mmap_effective_limit(current->addr_limit)`。
7. 若 `flags & MAP_FIXED`：
   - `addr < X86_USER_MIN_ADDR` → `-EINVAL`（**显式拒绝 `addr == 0` 的 `MAP_FIXED`，封堵零页映射漏洞**）。
   - `addr & (PAGE_4K_SIZE - 1)` → `-EINVAL`。
   - `addr >= limit || rounded > limit - addr` → `-ENOMEM`。
   - `mm_user_range_protected(current->mm, addr, addr + rounded)` → `-EINVAL`。

> **优先级核对**：`addr = HEAP_LIMIT, length = 0xfffffffff0000000` 在步骤 3 即以 `-EINVAL` 拒绝（早于步骤 7 的 `-ENOMEM`），与 `test_user_protected_ranges` 既有错误码完全一致。

### 6.3 非固定间隙搜索 `mmap_find_gap_locked()`

将既有返回无错误码 `uint64_t` 的 `mmap_find_gap()` 替换为带显式上界与状态码的：

```c
static int mmap_find_gap_locked(mm_t *mm, uint64_t length,
                                uint64_t upper_limit, uint64_t *out_addr)
```

调用方已持有 `mm->lock`，且 `length > 0` 为 4 KiB 倍数：

1. **搜索起点合法化**：
   - `search = mm->mmap_base`。
   - 若 `search < X86_USER_STACK_END`，钳制为 `X86_USER_STACK_END`（防御 `mm->mmap_base == 0` 或落在保护区内的异常状态，移除原先盲目覆写 `addr = X86_USER_STACK_END` 而不检查已有 VMA 重叠的缺陷）。
   - 若 `(search & (PAGE_4K_SIZE - 1)) != 0`：若 `search > UINT64_MAX - (PAGE_4K_SIZE - 1)` 返回 `-ENOMEM`，否则 `search = PAGE_4K_ALIGN(search)`。
   - 若 `search >= upper_limit || length > upper_limit - search`，立即返回 `-ENOMEM`。
2. **有序链表 first-fit 扫描（全程减法防溢出，无歧义推进）**：
   ```c
   uint64_t cand = search;
   for (list_t *pos = mm->vma_list.next; pos != &mm->vma_list; pos = pos->next) {
       vma_t *v = container_of(pos, vma_t, list);
       if (v->vm_end <= cand)
           continue;
       if (v->vm_start > cand) {
           uint64_t gap_end = (v->vm_start < upper_limit) ? v->vm_start : upper_limit;
           if (gap_end > cand && length <= gap_end - cand) {
               *out_addr = cand;
               return 0;
           }
       }
       /* 未命中返回时无条件推进越过当前 VMA（严禁放入上面的 else 分支） */
       if (v->vm_start >= upper_limit || v->vm_end >= upper_limit)
           return -ENOMEM;
       if (v->vm_end > cand)
           cand = v->vm_end;
       if (length > upper_limit - cand)
           return -ENOMEM;
   }
   if (cand < upper_limit && length <= upper_limit - cand) {
       *out_addr = cand;
       return 0;
   }
   return -ENOMEM;
   ```
   - 自动跳过零长度 `VMA_HEAP` 与低于 `search` 的 `MAP_FIXED` VMA；正确越过恰在 `search`、跨骑 `search`、或前方空隙小于 `length` 的 VMA；永不产生重叠 VMA，永不回绕到 `search` 以下。
3. **非固定 advisory hint 语义**：当未设置 `MAP_FIXED` 且 `addr != 0` 时，通过 §6.2 步骤 3 校验后忽略 `addr` 提示，统一由 `mmap_find_gap_locked()` 从 `mm->mmap_base` 选址——既不破坏既有 `test_user_protected_ranges`（hint 指向保护区时仍正常分配在安全区），也避免用户态固定 hint 绕过 ASLR。
4. **普通与设备两条非固定路径统一接入并清理旧检查**：
   - **移除既有重复上界检查**：删除 `mmap.c:579-590`（设备路径）与 `mmap.c:640-647`（普通路径）的旧 `current->addr_limit` 校验——`MAP_FIXED` 已在 §6.2 步骤 7 统一校验 `limit` 与保护区，非固定路径已由 `mmap_find_gap_locked(..., limit, &addr)` 严格保证 `[addr, addr + length) ⊆ [X86_USER_STACK_END, limit)`。
   - **设备路径（`if (_dev_mmap) {`）**：在 `if (!(flags & MAP_FIXED)) {` 内短持 `mm->lock` 调 `mmap_find_gap_locked(current->mm, length, limit, &addr)`；若返回非 0，平衡释放 `file_node` / `file` 并直接返回 `-ENOMEM`（不分配模板 VMA、不调 `_dev_mmap`）；紧随其后（仍在 `kmalloc(sizeof(vma_t))` 与 `_dev_mmap(...)` 之前）保留防御性 `if (mm_user_range_protected(current->mm, addr, addr + length))`（命中时平衡释放引用并返回 `-ENOMEM`）。`mmap_dev_commit()` 保持既有的持锁重叠复检。
   - **普通路径**：在单次 `mm->lock` 临界区内的 `if (!(flags & MAP_FIXED)) {` 中调 `mmap_find_gap_locked(mm, length, limit, &addr)`，再做防御性 `mm_user_range_protected(mm, addr, addr + length)` 检查；任一失败均解锁、平衡释放 `file_node` / `file` 并返回 `-ENOMEM`。
   - **保留既有源码审计锚点**：`hosttests/cases/test_user_protected_ranges.c:869-896` 对 `mmap.c` 执行字面锚点检查（`if (_dev_mmap) {` 到 `kmalloc(sizeof(vma_t))` 之间须出现 `mm_user_range_protected(`；`do_mmap` 中首个 `if (!(flags & MAP_FIXED)) {` 到后续 `} else {` 之间须出现 `mm_user_range_protected(`）。实现须保留这两处字面条件形式及块内防御调用，避免误伤该回归门禁。
5. **`MAP_FIXED` 语义不变**：精确使用调用者指定的 `addr`，仍可映射到 `[X86_USER_STACK_END, limit)` 任意位置（包括 `mmap_base` 下方如 `0x40000000`、与 `mmap_base` 重合、或高于 `mmap_base` 至 `limit - length`），继续走既有的事务性 `prepare_unmap_locked` / `commit_unmap_locked` 或 `mmap_dev_commit` 原子替换逻辑。

### 6.4 `do_munmap()` 与 `do_mprotect()` 上界统一

- `do_munmap()`（`kernel/arch/x86_64/memory/mmap.c:307`）将 `if (end > current->addr_limit) return -EINVAL;` 改为：
  ```c
  uint64_t limit = mmap_effective_limit(current->addr_limit);
  if (end > limit)
      return -EINVAL;
  ```
- `do_mprotect()`（`kernel/arch/x86_64/memory/mmap.c:757`）同样改为：
  ```c
  uint64_t limit = mmap_effective_limit(current->addr_limit);
  if (end > limit)
      return -EINVAL;
  ```
- 由此与 §3.2、§6.1 严密闭合：顶有效页 `[0x00007FFFFFFFE000, 0x00007FFFFFFFF000)` 允许 `munmap`/`mprotect`，顶保护页 `[0x00007FFFFFFFF000, 0x0000800000000000)` 与非规范地址（即使宿主测试将 `addr_limit` 设为 `0xffff800000000000` 或 `UINT64_MAX`）一律以 `-EINVAL` 拒绝，且零副作用。

---

## 7. 跨子系统兼容性核查

| 子系统 | 核查结论 |
|---|---|
| 页表与回收（`kernel/arch/x86_64/memory/vmm.c`） | `vmm_pt_walk`、`vmm_free_user_map`、`x86_vmm_pmd_slot`、`pt_range_precheck`、`arch_virt_to_phys`、`arch_user_range_accessible` 均覆盖 `l0 ∈ [0, 255]`，完整支持 `[0, 0x0000800000000000)` |
| 缺页与 COW（`trap.c`、`fork.c`、`uaccess.c`、`futex.c`） | `cr2`、VMA 区间、4 级 VA 重建 `((uint64_t)l4 << 39) | ...` 与 `addr_limit` 均为 64 位无截断；`fork_mm_copy` 完整遍历 `l4 = 0..255` |
| `/proc/<pid>/maps`（`kernel/fs/procfs.c:134`） | `%012lx-%012lx` 为 12 位十六进制（48 位），精确表示 `400000000000..7ffffffff000`；随机映射全部位于栈（`0x20600000`）之上，双路归并升序不变量不变 |
| `libc`（`libc/unistd/mmap.c`） | 仅将 `ret < 0 && ret > -4096` 视作错误码；所有规范低半区地址最高位为 0，在 `int64_t` 下恒为非负 |
| 既有宿主测试与桩 | `test_user_protected_ranges` / `test_process_image_lifecycle` / `test_user_write_cow` / `test_brk_pages` / `test_fork_user_map` 手动置 `fixture_mm.mmap_base = 0x40000000UL`（`>= X86_USER_STACK_END`），保持兼容（并注意保留 `test_user_protected_ranges.c:869-896` 的源码锚点）。联编 `boot3_exec_production.o` 的 `test_boot_spawn_failure`、`test_boot_services_vfs`、`test_boot_services_spawn` 同步联编生产 `aslr.c`：修正前两者错误的 2 参数 `kernel_random_get_strong` 桩签名为 `bool kernel_random_get_strong(uint8_t out[32])`；将 `test_boot_services_spawn.c` 的桩签名同步为 `uint8_t out[32]`（并更新其第 406 行触发时机注释，以及第 206 行 `mm_alloc()` 桩的 `128` 改为 `sizeof(mm_t)`） |
| AArch64 | `kernel/memory/mm.c` 不改动；`aslr.c` 在 `kernel/arch/x86_64/memory/` 下不进入 aarch64 构建清单，并由 `test-aarch64-audit` 与 `test_vmm_caller_audit` 反向守护 |

---

## 8. 分层验证策略与验收标准

按 `docs/testing/testing-policy.md` 分层证明，不以单层代替跨层：

| 层级 | 载体、隔离约束与命令 | 必须证明的不变量 |
|---|---|---|
| **`host`** | 新增 `hosttests/cases/test_x86_aslr_mmap.c` 与 `hosttests/cases/test_x86_aslr_exec.c`（均带 `@os01-test` 元数据）；同步更新 `config/test-scenarios.json`（登记两项宿主主责场景，并在跨层复用场景处补齐 `supporting` + `distinct_risk`）；运行 `make regen-test-catalog`、`make test-host`、`python3 qemutests/scenario_catalog.py --check`、`make test-harness`。<br>**隔离与桩选型约束**：<br>• `test_x86_aslr_mmap.c` 联编生产 `aslr.c`、`mmap.c`、`vma.c`、`brk.c`、`vm_area.c`、`mm.c`，必须使用**稀疏 VA 槽表 PTE 桩**（复用 `hosttests/mock/lifecycle/` 体系或等价独立稀疏桩），禁止复用 `pranges` 固定低址平铺数组（无法表达 64 TiB 与 `0x00007FFFFFFFE000`）；<br>• `test_x86_aslr_exec.c` 联编生产 `exec.c` 与 `aslr.c`，必须编译**独立 `.o` 对象**并配置独立 mock include 目录拦截 `<arch/x86_64/mmu.h>`（提供非特权 `arch_switch_mm()` 记录桩，避免复用 `boot3_exec_production.o` 在 `sys_exec()` 成功路径执行特权 `mov %cr3` 触发宿主机 `SIGSEGV`），同时桩定 `vfs_lookup_at` 与 `task_comm_apply_exec` | ① `x86_aslr_derive_mmap_base` 边界输入（`0`、`1`、`(1<<28)-1`、`1<<28`、`UINT64_MAX`）、4 KiB 对齐、窗域 `[64 TiB, 65 TiB)` 与低 28 位双射；② `x86_aslr_init_mmap_base` 的 `NULL → -EINVAL`（不耗熵）、STRONG 成功派生与小端字节序、STRONG 失败 `-EAGAIN` 且原 `mmap_base` 不变；③ 高 64 位 `mmap_base` 下自动映射、与 `MAP_FIXED`（位于基址/跨骑基址/基址前方精确空隙与过小空隙/基址下方）无冲突；④ `MAP_FIXED` 拒绝 `addr == 0`（`-EINVAL`）、接受 `0x00007FFFFFFFE000` 顶有效页、拒绝 `0x00007FFFFFFFF000` 与 `0x0000800000000000`（`-ENOMEM`）且无副作用；⑤ `do_munmap` / `do_mprotect` 接受顶有效页 `0x00007FFFFFFFE000`，拒绝顶保护页 `0x00007FFFFFFFF000` 与非规范地址 `0x0000800000000000`（即使 `addr_limit = 0xffff800000000000` 或 `UINT64_MAX` 也返 `-EINVAL` 且零副作用）；⑥ 窗口耗尽返回 `-ENOMEM` 不回绕、长度/地址各种溢出返回 `-EINVAL`/`-ENOMEM` 无副作用；⑦ `mm->mmap_base == 0` 或落保护区时钳制到 `X86_USER_STACK_END` 且跳过已有 VMA 不重叠；⑧ 非固定 advisory hint 不绕过随机 `mmap_base`；⑨ 设备非固定映射耗尽时 `vfs_node_put`/`files_put_file` 平衡且不调 `_dev_mmap`；⑩ `spawn_user_task` / `sys_exec` 在 STRONG 失败时返回 `-EAGAIN`、不发布任务、完整回滚且 `sys_exec` 保留旧 `current->mm`（含旧 `mmap_base`）且不调 `arch_switch_mm`；成功时分别独立调用两次 `kernel_random_get_strong`（`mmap_base` 与 `AT_RANDOM`）并提交派生值与 `arch_switch_mm` |
| **`static`** | `make PROFILE=x86_64-clang test-static`、`make PROFILE=x86_64-clang test-static CASE=mm-header-boundary`、`make PROFILE=aarch64-clang test-static CASE=mm-header-boundary`、`make check-test-catalog`、`make PROFILE=aarch64-clang test-aarch64-audit` | 双架构 `mm-header-boundary` 实跑证明公共内存头不泄漏 `_ARCH_X86_64_ASLR_H`、新增 `X86_USER_*` 宏与 `x86_aslr_*` 符号；目录零漂移；断言政策合规；aarch64 `kernel.elf` 符号排除门（含 `x86_aslr_`）与 `test_vmm_caller_audit`（含 `arch/x86_64/memory/aslr.c`）全通过 |
| **`kernel_selftest`** | 新增 `kernel/selftest/test_aslr_mmap.c`（`#ifdef OS01_SELFTEST`，仅在 `selftest.c` 的 `#ifdef OS01_SELFTEST` + `#if !defined(__aarch64__)` 块注册），独立运行 `make KERNEL_SELFTEST=1 test-kernel-selftest` | 真实内核态生产 `x86_aslr_init_mmap_base` + 生产 `kernel_random_get_strong`：① `mm == NULL → -EINVAL`；② STRONG 池多次调用产生不同且落入 `[64 TiB, 65 TiB)` 的 4 KiB 对齐基址；③ 通过 `kernel_random_test_set_pool_quality` 模拟 WEAK 与 NONE 池并按探测到的 `arch_has_strong` 模式感知断言：无硬件 RDSEED 时返回 `-EAGAIN` 且哨兵 `mmap_base` 不变，有硬件 RDSEED 时回退硬件 STRONG 返回 `0` 且落入窗口内；④ 正常结束及每条失败早退路径均恢复保存的 `(saved_q, saved_ready)` |
| **`systest`** | 在 `user/systest/systest.c` 新增 `aslr-probe` 自重入探针，在 `user/systest/test_mem.c` 新增 3 项用例，独立运行 `make OS01_SYSTEST=1 test-syscall`（不混设 `KERNEL_SELFTEST=1`） | ① `aslr_mmap_exec_randomized`：10 次 `fork+exec` 跑 `aslr-probe`，每次返回地址均 4 KiB 对齐、落在 `[0x0000400000000000, 0x0000410000000000)`、不等于 `0x40000000`、真实读写成功，且唯一值数 `>= 9`（统计误报率 `≈ 1.04e-14 < 10^-13`）；② `aslr_mmap_fork_inherit`：父进程先 `mmap` 写入哨兵后 `fork`，父子在 `fork` 后至下一次匿名 `mmap` 前不做其他映射；子进程验证同 VA 读回原哨兵且写时 COW 隔离，再调匿名 `mmap` 通过 pipe 回传地址 `c1`；父进程调匿名 `mmap` 得到 `p1`，断言 `c1 == p1`（证明 `fork` 继承 `mmap_base` 与布局）；再 `fork+exec("aslr-probe")` 经 pipe 回传地址 `e1`，断言 `e1 ∈ [64 TiB, 65 TiB)` 且 `e1 != p1` 且 `e1 != p0`（证明 `exec` 重新随机化）；③ `aslr_mmap_fixed_and_bounds`：`MAP_FIXED(0x40000000)` 与 `MAP_FIXED(0x00007FFFFFFFE000)` 正常读写、`mprotect` 与 `munmap`；`MAP_FIXED(0x0)` 返 `-EINVAL`；`MAP_FIXED` 对 `0x00007FFFFFFFF000` 与 `0x0000800000000000` 返 `-ENOMEM`；`munmap` / `mprotect` 对 `0x00007FFFFFFFF000` 与 `0x0000800000000000` 返 `-EINVAL`；高随机区内 `MAP_FIXED` 替换子页保留邻页；64 TiB 非固定大映射返 `-ENOMEM`（不回绕）；`/proc/self/maps` 正确呈现 12 位十六进制高随机区间 |
| **`qemu_e2e`** | `make test-qemu SUITE=phase-0`；`qemutests/test_entropy_facade.sh tier1-default` | 产品态引导至 ash 提示符全链正常；无 virtio-rng（NONE）下 init 继续 fail-closed 拒启，有 virtio-rng（STRONG）下正常启动 |

---

## 9. 验收后文档回填

实现并全部门禁通过后，同步更新：
- `docs/memory/cow-mmap.md`：记录 x86_64 `mmap_base` 随机化窗口、fail-closed 语义、`mmap_effective_limit()` 与 `mmap_find_gap_locked()` 契约。
- `docs/roadmap.md`：移出已闭环的 `ASLR-A：mmap 基址` 并登记近期闭环索引，继续保留 `ASLR-B：ET_DYN/PIE` 与栈随机化边界说明。
- `docs/changelog.md`：按日期补记闭环摘要与实测证据。

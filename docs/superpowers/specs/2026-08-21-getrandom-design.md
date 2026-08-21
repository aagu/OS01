# getrandom(2) syscall 设计文档

- 日期：2026-08-21
- 状态：待用户 review（尚未进入实现）
- 目标：提供 Linux 兼容的 getrandom(2) syscall，以 ChaCha20 CSPRNG + RDRAND/RDSEED 硬件熵为内核统一随机源；/dev/random 切换到该源，并新增 /dev/urandom
- 修订记录：
  - v1：初版
  - v2：并入首轮 review — ①用户 buffer 逐页 PTE 校验（内核态 PF 无 demand-paging，裸写未映射页会 hlt 挂死）；②RDRAND 从"每调用 rekey"改为初始化充分播种 + 周期 reseed（每 1 MiB）；③ChaCha20 32-bit counter 回绕处理（高 32 位进 nonce）；④len 上限 33554431 + 分块放锁；⑤SMP 全局锁标注为已知限制；⑥内核经 sysroot 看到 libc 头文件的构建链路已验证；⑦aarch64 fallback 改用 arch_cycle_counter()；⑧文件清单补 docs/syscall.md；⑨测试补并发/大 len/monobit；⑩非目标注明 AT_RANDOM 未来消费者；⑪明确 rdi/rsi/rdx 寄存器约定
  - v3：并入二轮 review — ①TOCTOU 闭合：新增 mm->lock，"校验→写"全程持锁，do_munmap/do_mmap(MAP_FIXED) 同锁（已验证 brk 不 unmap、页表 unmap 源仅这两处+进程拆毁）；②/dev/random+/dev/urandom 的 read 用同一 helper 对称保护；③COW 页 -EFAULT 的影响面注明；④reseed 的 RDRAND 失败语义=跳过本次；⑤nonce 字拆分端序写明 little-endian；⑥test#3 改用 mmap+munmap 确定性构造未映射 VA；⑦同 mm 并发竞态在当前用户态不可构造（无 CLONE_VM），race-injection 测试不可表达，如实写明

---

## 1. 背景与动机

现状：`/dev/random`（`kernel/fs/devfs.c` 的 `random_read`）是 rdtsc 低位逐字节异或，逐字节取一次 TSC，熵极低且相邻字节强相关，仅为占位实现。

需求方：

- mbedtls `entropy_poll.c` 已有 `HAVE_GETRANDOM` 路径（`SYS_getrandom` 存在则优先使用），当前 OS01 上不可用。
- busybox / musl 程序在 x86_64 上通过 Linux syscall **318** 调 getrandom；现有 `linux_to_os01[256]` 翻译表容量只有 256，318 无法命中，需要扩表。
- mbedtls 等也会读 `/dev/urandom`（`mbedtls_platform_entropy_poll` 的 fallback 路径），该节点当前不存在。

## 2. 总体架构

分层，每层职责单一：

```
用户态 getrandom(2) ──┐
                      ├─→ trap.c: SYS_getrandom ─→ get_random_bytes()
/dev/random  read ────┤                            │
/dev/urandom read ────┘                            ↓
                          kernel/kernel/random.c：ChaCha20 池
                          （状态 + spinlock + RDRAND/RDSEED reseed）
                                                   ↓
                          libc/random/chacha20.c：纯算法（libk.a）
```

- 算法与状态分离：ChaCha20 block function 是无状态纯算法，放 `libc/`（编入 libk.a 供内核链接，同时进 libc.a 供未来用户态 arc4random 复用）。
- 熵池状态、锁、reseed 策略是内核私有，放 `kernel/kernel/random.c`。
- 架构相关指令（RDRAND/RDSEED）封装进现有 `kernel/include/kernel/arch/` 分发模式（同 `arch/cpuid.h`）。

## 3. ChaCha20 算法层（libc）

新文件：

- `libc/random/chacha20.c` — RFC 8439 ChaCha20 block function。仅约 80 行，自行实现（不引第三方），不分配内存、无全局状态。
- `libc/include/chacha20.h` — 接口：

```c
// 生成一个 64 字节 block：key 32B，counter 32bit，nonce 12B
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);
```

构建：`libc/Makefile` 的 `C_SOURCES` 通配列表新增一行 `$(wildcard random/*.c)`（该列表是显式逐目录 wildcard，不加不会编译）。同时编入 libc.a 与 libk.a（同一规则自动覆盖）。

**include 路径（已验证）**：内核**不直接** `-I../libc/include`；链路是 `libc/Makefile install-headers` 把 `libc/include/.` 拷到 `sysroot/usr/include/`，内核经顶层 Makefile 导出的 `--sysroot=$(SYSROOT) -isystem=$(INCLUDEDIR)` 看到（现有 `<rbtree.h>` 即走此路）。顶层目标 `kernel.bin: lib` 保证 libc 头文件先安装，正常 `make` 流程无需手工干预；单独 `make -C kernel` 前需先 `make -C libc install-headers`。

## 4. 内核 PRNG（`kernel/kernel/random.c` + `kernel/include/kernel/random.h`）

### 4.1 接口

```c
void random_init(void);                      // 启动期调用一次（BSP）
void get_random_bytes(void *buf, size_t len); // 任意上下文可用（IRQ-safe）
```

### 4.2 硬件熵检测

- `kernel/include/kernel/arch/x86_64/regs.h` 补充特性位：
  - `CPUID_FEAT_ECX_RDRAND  (1 << 30)`（leaf 1 ECX）
  - `CPUID_FEAT_EBX_RDSEED  (1 << 18)`（leaf 7 subleaf 0 EBX）
- 新增 `kernel/include/kernel/arch/random.h`（按 `__x86_64__`/`__aarch64__` 分发，同 `arch/cpuid.h` 模式）：
  - x86_64：`rdrand64(uint64_t *out)` / `rdseed64(uint64_t *out)` 内联汇编，返回 bool（CF=1 成功）；失败重试上限 10 次（Intel SDM 建议）。
  - aarch64：stub，恒返回 false（编译通过即可；未来可接 RNDR 寄存器）。

### 4.3 种子与 reseed

- `random_init()`：种子 = RDSEED×4（不可用则 RDRAND×4）⊕ arch_cycle_counter() 多次采样 ⊕ jiffies，写入 ChaCha20 key（32B）。
- **周期 reseed**：每累计输出 **1 MiB** 用 RDRAND×4（或 RDSEED×4）与 key 异或混入一次。**禁止每次调用都抽 RDRAND**：RDRAND 吞吐有限（~100 MB/s 量级）且可能失败需重试，每调用 rekey 会把 CSPRNG 退化成硬件依赖、在高并发下成为瓶颈，且不带来额外安全性。RDRAND 只用于初始化播种与周期补熵。**reseed 失败语义**：RDRAND/RDSEED 重试上限用尽仍失败时，**跳过本次 reseed，key 保持不变**——严禁把未初始化或全零 buffer 异或进 key（那会确定性地改写/削弱 key）。
- **counter 回绕**：池维护 64-bit block 索引 `blk`；调 `chacha20_block` 时 `counter = (uint32_t)blk`，`blk >> 32` 写入 nonce 的第一个 32-bit 字（**RFC 8439 为 little-endian 字序**：12B nonce = 3 个 LE u32，`blk>>32` 即 nonce 字节 [0..3] 的 LE 解释），其余 nonce 字为 0。`chacha20.h` 注释写明 LE 约定。keystream 空间 2^96 block，回绕在事实上不可达，不存在 (counter,nonce) 复用。`chacha20.h` 的 API 形态不变。
- 无任何硬件熵（老 CPU / aarch64 stub）：退化为 arch_cycle_counter() 高频混合种子（两架构均有此函数；aarch64 无 rdtsc，不得使用 rdtsc 命名/调用）。弱熵但不劣于现状，属已知限制，写进代码注释。

### 4.4 并发与 SMP

- 池状态由 `spinlock_irqsave` 保护（syscall 路径与潜在 IRQ 上下文共用）。
- **分块放锁**：`get_random_bytes()` 内部按 64 KiB 分块，块间释放并重取 spinlock，避免大 len 在长临界区内串行化所有 CPU。
- 全局单 key + 全局锁是**已知 SMP 扩展性限制**（所有 CPU 的随机生成被一把锁串行化），本阶段可接受；未来演进方向是 per-CPU ChaCha 状态（见 §10）。

### 4.5 初始化时机

`main.c`：`subsys_init_all()` 之后、`devfs_init()` 之前调 `random_init()`。此后所有 `get_random_bytes()` 调用（devfs read、syscall）都安全。

## 5. syscall 接线（`kernel/arch/x86_64/trap.c` + 两处 uapi）

- `SYS_getrandom = 66`：同步加入 `kernel/include/uapi/syscall.h` 与 `libc/include/sys/syscall.h`（两处现有编号一致，继续保持）。
- `trap.c`：
  - `syscall_names[66] = "getrandom"`（表长 66→67）。
  - **扩表**：`linux_to_os01[256]` → `[320]`，guard `regs->rax < 256` → `< 320`，新增 `[318] = 66`。int8_t 容量足够（66 < 127）。
  - `case SYS_getrandom:`（参数寄存器约定同 SYS_write：`rdi=buf, rsi=len, rdx=flags`）：
    - `len == 0` → 返回 0（buf 可为 NULL，Linux 语义；先于一切指针校验）。
    - `len > 33554431` → 按 Linux urandom 源行为截断为 33554431（填充并返回该值），不报错。
    - flags 校验：仅接受 `0 | GRND_NONBLOCK(1) | GRND_RANDOM(2)` 的任意组合（池不阻塞，两 flag 均为语义 NOP），其他位 → `-EINVAL`。
    - **用户 buffer 校验与写入（关键，见 §5.1）**：用 §5.1 的 `user_write_range_begin()` / `user_write_range_end()` 夹住整个"校验→写"窗口：
      ```c
      int rc = user_write_range_begin((uint64_t)buf, len);  // mm->lock + 范围 + 逐页 PTE
      if (rc < 0) { regs->rax = rc; break; }                // -EFAULT（锁已释放）
      get_random_bytes(buf, len);                           // 内部分块放池锁，mm->lock 全程持有
      user_write_range_end();
      regs->rax = len;
      ```

### 5.1 用户 buffer 安全：mm->lock + 逐页校验（闭合 TOCTOU）

`do_page_fault()` 对**所有**缺页（含内核态 syscall 中写用户内存触发的 PF）统一走 dump + `hlt()` 死循环，**没有 demand-paging**。只查 `addr_limit` 范围不够；只做事前逐页 PTE 校验也不够——校验与写入不是原子区间，同一 mm 的另一任务可在其间 munmap（TOCTOU），仍会把内核挂死。因此本设计引入 **mm 级锁**闭合窗口：

- `mm_t` 新增 `spinlock_T lock`（初始化约定 `.lock = 1L` = 未锁，同 `task_list_lock`；mm 创建路径 `fork_mm_copy` / 新建 mm 处显式初始化。静态 `init_mm = {0}` 不受影响——getrandom 只碰用户 mm）。
- 新 helper（实现在 `kernel/memory/vma.c`，声明进 `kernel/include/kernel/vmm.h`）：
  ```c
  // 持 current->mm->lock（irqsave），然后：syscall_user_range_ok 范围检查 +
  // 逐页 vmm_pt_walk(Phy_To_Virt(current->mm->pml4), va, 0, 0) 要求 PTE 存在
  // 且 PAGE_R_W 置位。全部通过返回 0（锁保持持有）；任一失败释放锁返回 -EFAULT。
  int  user_write_range_begin(uint64_t addr, size_t len);
  void user_write_range_end(void);   // 释放 current->mm->lock
  ```
  内核调用者边界：`current->mm == NULL`（kthread 经 devfs 读 /dev/urandom）时跳过校验与加锁直接返回 0（内核 buffer 受信任），`end()` 对应成 no-op。
- **unmap 源同锁**（已核实用户页表的 unmap 源只有两处 syscall 路径）：`do_munmap()` 与 `do_mmap()` 的 MAP_FIXED 重叠解除段，均持 `current->mm->lock`。brk 已验证不 unmap（只移动 `end_brk` 指针），无需参与。
- 锁内安全性：持锁期间只写"已确认 P+W 的页"，不会触发 PF，不会在中断/异常路径重入该锁，无死锁面。**mm->lock 用普通 spin_lock（非 irqsave）**：它只在 syscall 上下文被取，IRQ/PF handler 均不触碰；这样大填充期间本 CPU 中断保持可用（timer tick 不丢）。持锁调度是安全的（竞争者自旋有界）。池锁与 mm->lock 的嵌套顺序固定为 mm->lock(外) → 池锁(内)，全项目唯一此一处嵌套，无环。大 len 填充全程持 mm->lock（上界 32 MiB cap；典型消费者 ≤ 256B），写进代码注释。
- **已知限制（如实声明，非 airtight 的剩余面）**：`vma_free_all()`（exit/exec 的整 mm 拆毁）**不**参与本锁——共享 mm 的用户任务仅存在于 fork OOM fallback（`fork_mm_copy` 失败时 `tsk->mm = current->mm`），且该路径有既有更严重问题（exit 时无条件 `kfree(current->mm)` 会 UAF 共享方）。本 spec 不触碰它；全局 `copy_to_user()`/GUP（含拆毁路径）属另一个 roadmap 项。在此限制之外，munmap/MAP_FIXED 竞态对本 syscall 是**闭合**的，不再是 best-effort。
- **COW 影响面**：要求 `PAGE_R_W` 意味着 MAP_PRIVATE 文件映射（COW，P=1/W=0）的 buffer 得到 `-EFAULT`，而 Linux 会先 COW-fault 再写入成功。实际消费者（mbedtls 等）的输出 buffer 都是堆/匿名可写页，不受影响；标注为已知偏差，测试不得用 COW 区做 buffer。

### 5.2 为什么 devfs 路径也要同一 helper

§1 的核心动机之一是 mbedtls 的 `/dev/urandom` fallback 路径——若只保护 getrandom(2) 而让 `random_read` 裸写，恰恰漏掉了驱动本设计的消费者，两条路径安全性不对称。因此 `random_read` 用同一对 `user_write_range_begin/end` 夹住"校验→`get_random_bytes`"（devfs read 的 `current` 即读者进程，mm 可用；helper 成本极低）。这是本设计与"其他设备一致"原则的**有意偏离**，理由即 §5.1：其他设备的裸写是继承性缺陷，新增代码不继续扩散它。

## 6. devfs（`kernel/fs/devfs.c`）

- `random_read` 删除 rdtsc 逐字节异或，改为：`user_write_range_begin()` → `get_random_bytes()` → `user_write_range_end()`（与 getrandom(2) 对称保护，理由见 §5.2）。
- 同一 `random_ops` 追加注册 `"urandom"` 节点（Linux 语义上 urandom 不阻塞，与本池行为一致；不区分 random/urandom 质量，与"同源"决策一致）。
- `random_write` 保持现状（接受并忽略）。

## 7. libc 用户态接口

- `libc/include/sys/random.h`（新）：`GRND_NONBLOCK=1`、`GRND_RANDOM=2` 宏 + `ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);` 声明。
- `libc/unistd/getrandom.c`（新）：syscall 包装（unistd/ 已在 Makefile 通配列表中，无需改构建）。
- `libc/include/sys/syscall.h`：`#define SYS_getrandom 66`。

## 8. 测试（`user/systest.c` 新增 `test_getrandom`）

1. 基本：`getrandom(buf, 32, 0)` 返回 32；buf 非全零；两次调用内容不同。
2. flags：`GRND_NONBLOCK`、`GRND_RANDOM`、两者或均接受；`flags=0x100` → `-EINVAL`。
3. 错误指针：越界 buf（addr_limit 之外）→ `-EFAULT`；**确定性未映射页**：`mmap` 一页 → 立即 `munmap` → 用返回的 VA 做 buf（必在 addr_limit 内且必未映射）→ `-EFAULT`（覆盖 §5.1 逐页校验路径，防回归）。**不得用 COW/MAP_PRIVATE 区做 buffer**（会得到 -EFAULT，是 §5.1 注明的已知偏差，不是本测试目标）。
4. `len=0` → 返回 0。
5. 大 len：`getrandom(buf, 1 MiB, 0)` 返回 1 MiB 且不挂死（覆盖分块放锁路径；缓冲区用匿名 mmap，确保可写已映射）。
6. `/dev/urandom`：open + read 16B 成功，两次 read 内容不同；对其做第 3 条的 munmap VA 测试 → read 返回 <0（devfs 对称保护生效）。
7. 并发：fork 出子进程，父子各循环 1000 次 `getrandom(32B)` 不挂死（SMP 下压全局池锁与分块放锁），且父子首 32B 不同（无跨进程输出重复）。**同 mm 并发 munmap 竞态（§5.1 的 TOCTOU）在当前用户态不可构造**——无 CLONE_VM/pthread（clone 映射到 fork），共享 mm 仅存在于 fork OOM fallback——race-injection 测试不可表达，如实说明；mm->lock 的正确性由现有 mmap/munmap 回归（test_mmap/test_fork_mmap/test_cow 全绿）+ review 保证。
8. 统计健全性（宽松界防 flaky）：取 32 KiB，1 的位数占比落在 [0.45, 0.55]（monobit 弱检验）。
9. Linux ABI 路径：`syscall(318, ...)` 翻译仅在 PF_LINUX_ABI 进程生效——systest 是 native 进程，此项不在 systest 断言，由 busybox 实际使用覆盖（实现期可用一个 PF_LINUX_ABI 小程序手工验证一次）。

验收：`make run` 下 systest 全绿（当前基线 132/132 → 按实际新增计数递增）。

## 9. 文件清单与工作量

| 文件 | 改动 |
|---|---|
| `libc/random/chacha20.c` | 新增 ~80 行 |
| `libc/include/chacha20.h` | 新增 ~15 行 |
| `libc/Makefile` | C_SOURCES 加 1 行 |
| `kernel/kernel/random.c` | 新增 ~120 行 |
| `kernel/include/kernel/random.h` | 新增 ~15 行 |
| `kernel/include/kernel/arch/random.h` (+`x86_64/random.h`) | 新增 ~50 行 |
| `kernel/include/kernel/arch/x86_64/regs.h` | +2 行特性位 |
| `kernel/include/uapi/syscall.h` / `libc/include/sys/syscall.h` | 各 +1 行 |
| `kernel/include/kernel/task.h` | mm_t 加 `spinlock_T lock` 字段 |
| `kernel/memory/vma.c` + `kernel/include/kernel/vmm.h` | `user_write_range_begin/end` helper ~50 行；do_munmap / do_mmap(MAP_FIXED) 持 mm->lock |
| `kernel/sched/task.c` | fork_mm_copy / mm 创建处初始化 mm->lock（`.lock = 1L`） |
| `kernel/arch/x86_64/trap.c` | 扩表 + case ~25 行 |
| `kernel/kernel/main.c` | +1 行 random_init() |
| `kernel/fs/devfs.c` | random_read 换实现（begin/end 夹住）+ 注册 urandom |
| `libc/include/sys/random.h` / `libc/unistd/getrandom.c` | 新增 ~25 行 |
| `user/systest.c` | test_getrandom ~110 行 |
| `docs/syscall.md` | syscall 清单 66→67，补 getrandom 条目 |

合计新增约 380 行，改动 17 个文件。

## 10. 非目标（YAGNI）

- 不做中断熵收集（键盘/磁盘时序熵池）——RDRAND + arch_cycle_counter() 已够本阶段。
- 不做 virtio-rng 设备。
- 不做 CRNG "已初始化"阻塞语义（Linux 早期 boot 阻塞）——池在 random_init 后即可用。
- 不做 `getentropy(3)`、`arc4random*` 用户态包装——留待有实际需求时。
- 不做 per-CPU ChaCha 状态（SMP 扩展性）与全局 `copy_to_user()`（§5.1 继承性缺陷的根治）——两者都是独立的 roadmap 项。
- aarch64 只保证编译通过，不追求硬件熵质量。
- **已知未来消费者**：roadmap 中 AT_RANDOM（ELF loader 辅助向量）→ stack canary / ASLR 的种子源将消费内核 `get_random_bytes()`。本设计不实现这些消费者，但 `get_random_bytes()` 的接口形态（任意上下文可调、IRQ-safe、无阻塞）按该预期冻结，后续消费者直接调用即可。

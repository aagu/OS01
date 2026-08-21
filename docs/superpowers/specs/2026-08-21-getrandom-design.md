# getrandom(2) syscall 设计文档

- 日期：2026-08-21
- 状态：待用户 review（尚未进入实现）
- 目标：提供 Linux 兼容的 getrandom(2) syscall，以 ChaCha20 CSPRNG + RDRAND/RDSEED 硬件熵为内核统一随机源；/dev/random 切换到该源，并新增 /dev/urandom
- 修订记录：
  - v1：初版
  - v2：并入首轮 review — ①用户 buffer 逐页 PTE 校验（内核态 PF 无 demand-paging，裸写未映射页会 hlt 挂死）；②RDRAND 从"每调用 rekey"改为初始化充分播种 + 周期 reseed（每 1 MiB）；③ChaCha20 32-bit counter 回绕处理（高 32 位进 nonce）；④len 上限 33554431 + 分块放锁；⑤SMP 全局锁标注为已知限制；⑥内核经 sysroot 看到 libc 头文件的构建链路已验证；⑦aarch64 fallback 改用 arch_cycle_counter()；⑧文件清单补 docs/syscall.md；⑨测试补并发/大 len/monobit；⑩非目标注明 AT_RANDOM 未来消费者；⑪明确 rdi/rsi/rdx 寄存器约定

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
- **周期 reseed**：每累计输出 **1 MiB** 用 RDRAND×4（或 RDSEED×4）与 key 异或混入一次。**禁止每次调用都抽 RDRAND**：RDRAND 吞吐有限（~100 MB/s 量级）且可能失败需重试，每调用 rekey 会把 CSPRNG 退化成硬件依赖、在高并发下成为瓶颈，且不带来额外安全性。RDRAND 只用于初始化播种与周期补熵。
- **counter 回绕**：池维护 64-bit block 索引 `blk`；调 `chacha20_block` 时 `counter = (uint32_t)blk`，nonce 第一个字 = `(uint32_t)(blk >> 32)`，其余 nonce 字为 0。keystream 空间 2^96 block，回绕在事实上不可达，不存在 (counter,nonce) 复用导致的 keystream 重用。`chacha20.h` 的 API 形态不变（RFC 8439 布局）。
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
    - **用户 buffer 两级校验（关键，见 §5.1）**：
      1. `syscall_user_range_ok(addr, len)` 范围检查 → `-EFAULT`；
      2. **逐页 PTE 校验**：对 `[addr, addr+len)` 覆盖的每个 4 KiB 页，`vmm_pt_walk((uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4), va, 0, 0)`（模式同 trap.c COW 处理处），要求 PTE 存在且 `PAGE_R_W` 置位。未映射页或写保护页（含 COW 页：P=1 但 W=0）→ `-EFAULT`。
    - 校验通过后调 `get_random_bytes(buf, len)`（内部分块放锁），返回实际填充字节数。

### 5.1 为什么必须逐页 PTE 校验

`do_page_fault()` 对**所有**缺页（含内核态 syscall 中写用户内存触发的 PF）统一走 dump + `hlt()` 死循环，**没有 demand-paging**。`syscall_user_range_ok()` 只查 `addr_limit` 范围，不查页面是否已映射；用户传入 mmap 后未触碰的页或空洞地址时，裸写会让整个内核挂死。getrandom 是"写"用户内存，比"读"用户内存的 syscall 更易命中未映射页，因此本设计主动逐页校验。注意这是**继承性缺陷**（read(2) 等现有 syscall 同样裸写、同样有风险），本 spec 只保证 getrandom 自身安全；全局 `copy_to_user()` 属另一个 roadmap 项，不在此引入。对 COW 页返回 -EFAULT 而不做内核态 COW 解析，同属继承限制。

## 6. devfs（`kernel/fs/devfs.c`）

- `random_read` 删除 rdtsc 逐字节异或，改为 `get_random_bytes(buffer, size)`。
- 同一 `random_ops` 追加注册 `"urandom"` 节点（Linux 语义上 urandom 不阻塞，与本池行为一致；不区分 random/urandom 质量，与"同源"决策一致）。
- `random_write` 保持现状（接受并忽略）。
- 注：/dev/urandom 的 read 走通用 read(2) 路径，用户 buffer 校验继承该路径现状（含 §5.1 所述继承性风险），本 spec 不为 devfs 单独加校验，保持与其他设备一致。

## 7. libc 用户态接口

- `libc/include/sys/random.h`（新）：`GRND_NONBLOCK=1`、`GRND_RANDOM=2` 宏 + `ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);` 声明。
- `libc/unistd/getrandom.c`（新）：syscall 包装（unistd/ 已在 Makefile 通配列表中，无需改构建）。
- `libc/include/sys/syscall.h`：`#define SYS_getrandom 66`。

## 8. 测试（`user/systest.c` 新增 `test_getrandom`）

1. 基本：`getrandom(buf, 32, 0)` 返回 32；buf 非全零；两次调用内容不同。
2. flags：`GRND_NONBLOCK`、`GRND_RANDOM`、两者或均接受；`flags=0x100` → `-EINVAL`。
3. 错误指针：越界 buf（addr_limit 之外）→ `-EFAULT`；**addr_limit 之内但保证未映射的地址**（如紧邻 brk 顶上一页，实现期选定一个确定未映射的 VA）→ `-EFAULT`（覆盖 §5.1 逐页校验路径，防回归）。
4. `len=0` → 返回 0。
5. 大 len：`getrandom(buf, 1 MiB, 0)` 返回 1 MiB 且不挂死（覆盖分块放锁路径；缓冲区用 mmap 或静态大数组，确保可写已映射）。
6. `/dev/urandom`：open + read 16B 成功，两次 read 内容不同。
7. 并发：fork 出子进程，父子各循环 1000 次 `getrandom(32B)` 不挂死（SMP 下跑在不同 CPU 上压全局锁与分块放锁），且父子首 32B 不同（无跨进程输出重复）。
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
| `kernel/arch/x86_64/trap.c` | 扩表 + case ~30 行 |
| `kernel/kernel/main.c` | +1 行 random_init() |
| `kernel/fs/devfs.c` | random_read 换实现 + 注册 urandom |
| `libc/include/sys/random.h` / `libc/unistd/getrandom.c` | 新增 ~25 行 |
| `user/systest.c` | test_getrandom ~100 行 |
| `docs/syscall.md` | syscall 清单 66→67，补 getrandom 条目 |

合计新增约 330 行，改动 14 个文件。

## 10. 非目标（YAGNI）

- 不做中断熵收集（键盘/磁盘时序熵池）——RDRAND + arch_cycle_counter() 已够本阶段。
- 不做 virtio-rng 设备。
- 不做 CRNG "已初始化"阻塞语义（Linux 早期 boot 阻塞）——池在 random_init 后即可用。
- 不做 `getentropy(3)`、`arc4random*` 用户态包装——留待有实际需求时。
- 不做 per-CPU ChaCha 状态（SMP 扩展性）与全局 `copy_to_user()`（§5.1 继承性缺陷的根治）——两者都是独立的 roadmap 项。
- aarch64 只保证编译通过，不追求硬件熵质量。
- **已知未来消费者**：roadmap 中 AT_RANDOM（ELF loader 辅助向量）→ stack canary / ASLR 的种子源将消费内核 `get_random_bytes()`。本设计不实现这些消费者，但 `get_random_bytes()` 的接口形态（任意上下文可调、IRQ-safe、无阻塞）按该预期冻结，后续消费者直接调用即可。

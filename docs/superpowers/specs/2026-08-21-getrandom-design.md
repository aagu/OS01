# getrandom(2) syscall 设计文档

- 日期：2026-08-21
- 状态：待用户 review（尚未进入实现）
- 目标：提供 Linux 兼容的 getrandom(2) syscall，以 ChaCha20 CSPRNG + RDRAND/RDSEED 硬件熵为内核统一随机源；/dev/random 切换到该源，并新增 /dev/urandom
- 修订记录：
  - v1：初版

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

- `random_init()`：种子 = RDSEED×4（不可用则 RDRAND×4）⊕ rdtsc 多次采样 ⊕ jiffies，写入 ChaCha20 key（32B）；nonce 全零，counter 从 0 递增。
- 每次 `get_random_bytes()` 调用：若 RDRAND 可用，先取 32B 硬件熵与当前 key 异或（rekey，提供前向保密与 RDRAND 降级抵抗），再从池出字节。
- 无任何硬件熵（老 CPU / aarch64 stub）：退化为 rdtsc 高频混合种子。弱熵但不劣于现状，属已知限制，写进代码注释。

### 4.4 并发

- 池状态由 `spinlock_irqsave` 保护（syscall 路径与潜在 IRQ 上下文共用）。
- 临界区内只做 ChaCha20 计算与内存写，典型 len ≤ 256B 时延迟可忽略。

### 4.5 初始化时机

`main.c`：`subsys_init_all()` 之后、`devfs_init()` 之前调 `random_init()`。此后所有 `get_random_bytes()` 调用（devfs read、syscall）都安全。

## 5. syscall 接线（`kernel/arch/x86_64/trap.c` + 两处 uapi）

- `SYS_getrandom = 66`：同步加入 `kernel/include/uapi/syscall.h` 与 `libc/include/sys/syscall.h`（两处现有编号一致，继续保持）。
- `trap.c`：
  - `syscall_names[66] = "getrandom"`（表长 66→67）。
  - **扩表**：`linux_to_os01[256]` → `[320]`，guard `regs->rax < 256` → `< 320`，新增 `[318] = 66`。int8_t 容量足够（66 < 127）。
  - `case SYS_getrandom:`：
    - flags 校验：仅接受 `0 | GRND_NONBLOCK(1) | GRND_RANDOM(2)` 的任意组合（池不阻塞，两 flag 均为语义 NOP），其他位 → `-EINVAL`。
    - 用户 buffer 用现有 `syscall_user_range_ok(addr, len)` 校验 → `-EFAULT`（跟随现有模式直接写用户指针，内核与用户共享页表）。
    - 调 `get_random_bytes(buf, len)`，返回 `len`；`len == 0` 返回 0（buf 可为 NULL，Linux 语义）。

## 6. devfs（`kernel/fs/devfs.c`）

- `random_read` 删除 rdtsc 逐字节异或，改为 `get_random_bytes(buffer, size)`。
- 同一 `random_ops` 追加注册 `"urandom"` 节点（Linux 语义上 urandom 不阻塞，与本池行为一致；不区分 random/urandom 质量，与"同源"决策一致）。
- `random_write` 保持现状（接受并忽略）。

## 7. libc 用户态接口

- `libc/include/sys/random.h`（新）：`GRND_NONBLOCK=1`、`GRND_RANDOM=2` 宏 + `ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);` 声明。
- `libc/unistd/getrandom.c`（新）：syscall 包装（unistd/ 已在 Makefile 通配列表中，无需改构建）。
- `libc/include/sys/syscall.h`：`#define SYS_getrandom 66`。

## 8. 测试（`user/systest.c` 新增 `test_getrandom`）

1. 基本：`getrandom(buf, 32, 0)` 返回 32；buf 非全零；两次调用内容不同。
2. flags：`GRND_NONBLOCK`、`GRND_RANDOM`、两者或均接受；`flags=0x100` → `-EINVAL`。
3. 错误指针：越界 buf → `-EFAULT`。
4. `len=0` → 返回 0。
5. `/dev/urandom`：open + read 16B 成功，两次 read 内容不同。
6. fork 父子各自 getrandom 值不同（验证无跨进程共享退化）。
7. 通过 Linux ABI 路径验证：直接 `syscall(318, ...)`（PF_LINUX_ABI 进程）也应成功——若 systest 不以 PF_LINUX_ABI 运行则跳过此项，由 busybox 实际使用覆盖。

验收：`make run` 下 systest 全绿（当前基线 132/132 → 133/133 或按实际新增计数）。

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
| `user/systest.c` | test_getrandom ~60 行 |

合计新增约 300 行，改动 13 个文件。

## 10. 非目标（YAGNI）

- 不做中断熵收集（键盘/磁盘时序熵池）——RDRAND + rdtsc 已够本阶段。
- 不做 virtio-rng 设备。
- 不做 CRNG "已初始化"阻塞语义（Linux 早期 boot 阻塞）——池在 random_init 后即可用。
- 不做 `getentropy(3)`、`arc4random*` 用户态包装——留待有实际需求时。
- aarch64 只保证编译通过，不追求硬件熵质量。

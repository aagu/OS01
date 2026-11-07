# ARCH-6 FS 并发与块缓存 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 三大 FS 大锁 mutex 化（tmpfs per-mount 化）+ blockdev 层 (dev, LBA) 写回式块缓存 + SYS_sync 接线 + SMP 并发压测闭环。

**Architecture:** Phase 1 把 ext2/FAT/tmpfs 的「spinlock 持锁跨 AHCI 忙等 I/O」换为调度器支持的 per-mount `mutex_t`（pre-SMP 挂载期用 `vfs_sched_ready()` 门控豁免）；Phase 2 在 `block_device_read/write` 包装层下植入 sector 粒度哈希缓存（仅物理盘 key，写回 + 显式 flush）；Phase 3 以 arch10-capacity 模式的用户态 fork 压测 + 内核 pinned-kthread 自测验收。

**Tech Stack:** C（内核/宿主两侧）、make 构建体系、QEMU E2E（python 标记解析）。

**Spec:** `docs/superpowers/specs/2026-11-07-arch6-fs-concurrency-blockcache-design.md`（本计划从 spec 论证，两者一起阅读）

**Review:** 2026-11-07 独立子 agent 两轮复审：首轮 `request_changes` 的宿主 BUSY 完成机制、压测目录清空顺序均已修订；第二轮 `approve`。以下实现任务均尚未执行。

## Global Constraints

- 分支：`feature/arch6-fs-concurrency-blockcache`（worktree `.worktrees/arch6-fs-concurrency-blockcache`），每个 Task 一组 commit。
- 目录规范：新源文件必须源/头成对（`kernel/block/bcache.c` ↔ `kernel/include/block/bcache.h`）；头统一 `#include <block/bcache.h>`。
- 日志规范：新内核代码只用 `log_err/log_warn/log_info/log_debug`（或 `debug_fs/debug_block`），禁 `serial_printk`/`color_printk`。
- 锁序按 spec §4.3：FS mutex 可在外层；active_lock 只做节点簿记。bcache_lock、data_lock、等待队列锁、AHCI port lock 不互相嵌套；I/O/睡眠/分配均在缓存自旋锁外。
- 零 UAPI 变更：不改 `kernel/include/uapi/`（`SYS_sync` 已存在，只补实现）。
- `make test-syscall` 必须带 `OS01_SYSTEST=1` 前缀且**不得**同时设 `KERNEL_SELFTEST=1`（AGENTS.md gotcha）。
- 每个 `register_host_test` 必须同步 `config/test-scenarios.json` 条目（CI 首个未映射项即停）。
- kernel block 源自动发现，无需登记；hosttest 在 drivers.mk 登记 register_host_test、TEST_BINS、对象规则、elf 链接行，并同步 scenario catalog。
- 本期缓存只支持 512B sector；元数据池与 1 MiB 数据池分开分配，初始化/后端错误必须返回，不能清除失败写回项的 DIRTY。
- flush 采用 spec §5.5 的 DIRTY key 快照 + BUSY 预留/drain；保证后端写回可见性，不承诺掉电持久性。
- 仅修订文档时不执行以下实现步骤或 commit；本计划是后续实施契约。

## Review Focus

以下失败类别必须由对应任务的测试约束：

1. **重复 key / 提前复用**：Task 4 保持填充 pin，Task 6 脏逐出后重新查重，Task 7 双 CPU 并发首次缺失只读一次。
2. **flush 遗漏 pinned/BUSY 脏数据**：Task 5 脚本化 pin→flush→释放，Task 7 真实 SMP flush 等待读取；成功返回后后端有新值。
3. **后端失败丢数据**：Task 4 读失败重试；Task 5 flush 失败保脏；Task 6 逐出失败保旧 key/data，并检查错误返回和无 BUSY/pin 泄漏。
4. **耗尽或完成事件丢唤醒**：Task 4 建立 ARM→重查协议与调度脚本，Task 6 覆盖耗尽窗口，Task 7 pin 满 2048 槽后确认阻塞、释放和恢复。
5. **验收假绿**：Task 7 late 阶段/按设备统计；Task 9 核验实际四核、12 个唯一 PASS 和父进程汇总，跨 worker 校验结束前不删除交换文件。

---

### Task 1: `vfs_sched_ready()` 门控 + ext2 mutex 化

**Files:**
- Modify: `kernel/fs/vfs.c:31-52`（`vfs_bootstrap` 定义处附近）
- Modify: `kernel/include/fs/vfs.h`（声明区）
- Modify: `kernel/include/fs/ext2.h:126`（`spinlock_T lock`）
- Modify: `kernel/fs/ext2.c`（全部 `spin_lock(&fs->lock)` / `spin_unlock(&fs->lock)` 站点；`ext2_init` 的 `spin_init`）

**Interfaces:**
- Produces: `bool vfs_sched_ready(void)`（`kernel/include/fs/vfs.h`，Task 2/3 复用）——`return !vfs_bootstrap && current && current->pid > 0;`
- Produces: ext2 per-mount `mutex_t lock` 与 helper `static void ext2_lock(ext2_fs_t*)` / `static void ext2_unlock(ext2_fs_t*)`（ext2.c 内部）——`vfs_sched_ready()` 为真时 `mutex_lock/unlock`，否则 no-op。
- Produces（OS01_HOST_TEST 钩子）: `void vfs_test_set_sched_ready(bool ready)`（vfs.c/vfs.h）——设 `vfs_bootstrap = !ready`，供宿主测试激活真实取锁路径。

- [ ] **Step 1: 在 `vfs.c` 实现 `vfs_sched_ready()` + 测试钩子，`vfs.h` 加声明**（`#include <stdbool.h>` 若缺）。紧邻 `vfs_ns_lock` 放置，注释说明与 `vfs_ns_lock` 同一豁免语义（启动时单核无竞争；运行时 idle/IRQ 不得访问这些 FS ops）；钩子注释注明仅 OS01_HOST_TEST。

- [ ] **Step 2: ext2 锁类型与 helper 替换**。`ext2.h`：`spinlock_T lock` → `mutex_t lock`（补 `#include <sync/mutex.h>`）。`ext2.c`：`spin_init(&fs->lock)` → `mutex_init(&fs->lock)`；新增 `ext2_lock/ext2_unlock` helper；逐函数把每个 `spin_lock(&fs->lock)`/`spin_unlock(&fs->lock)` 对替换为 helper（含早退路径上的 unlock——用 `grep -n "fs->lock" kernel/fs/ext2.c` 清点，改前改后站点数一致）。`_locked` 后缀函数注释更新为「caller holds mutex」。

- [ ] **Step 3: 修复 `hosttests/cases/test_vfs_iops_fops.c` 的锁内检断言**（`vfs_test_runtime.h:76` 已定义 `current`，`task_t` 有 `pid` 字段）：
  - 每个涉及锁内断言的用例在 `vfs_init()` **之后**调 `vfs_test_set_sched_ready(true)` 并确保 `vfs_test_current->pid > 0`（如 `= 4321`），使 helper 真实取 mutex；
  - `vfs_test_on_active_register` 的 ext2 分支（:331）`assert_eq(0, fs->lock.lock)` → `assert_true(fs->lock.owner != 0)`（mutex `owner`：0=free，>0=holder，`kernel/include/sync/mutex.h:7-8`）；
  - 注意：bootstrap 清除后 `vfs_ns_lock` 在宿主测试中也被激活（mutex 路径），若暴露新失败按 mutex 语义修复测试而非绕过门控。

- [ ] **Step 4: 构建与既有测试回归**

Run: `make && make test-host && make test-static`
Expected: 构建零警告新增；test-host 全绿（重点 `test_vfs_iops_fops`、`test_fat32_basic`、`test_block_device_ops`）；test-static 的全部已登记审计通过。

- [ ] **Step 5: Commit** `git commit -m "feat(fs): scheduler-backed per-mount mutex for ext2"`

### Task 2: FAT mutex 化

**Files:**
- Modify: `kernel/include/fs/fat.h:113`（`spinlock_T fat_lifecycle_lock` → `mutex_t lock`）
- Modify: `kernel/fs/fat.c`（全部 `fat_lifecycle_lock` 站点；`spin_init` 处）
- Modify: `hosttests/cases/test_vfs_iops_fops.c:337/:528`（`fat_lifecycle_lock` 字段引用 → `lock`，断言改 `owner` 语义，同 Task 1 Step 3 模式）

**Interfaces:**
- Consumes: Task 1 的 `vfs_sched_ready()`。
- Produces: `fat32_fs_t.lock`（`mutex_t`）+ helper `static void fat_lock(fat32_fs_t*)` / `fat_unlock(fat32_fs_t*)`（fat.c 内部，模式同 Task 1）。

- [ ] **Step 1: 字段改名 + 类型替换 + helper 替换**，替换前 `grep -c "fat_lifecycle_lock" kernel/fs/fat.c` 清点站点数，改后用 `fat_lock(fs)` / `fat_unlock(fs)`（或等价命名）逐一配对替换。`on_fat_block_read` 钩子断言（`s_test_fat_fs->lock.owner != 0`）在本步后依然成立（mutex 持锁覆盖后端读，宿主已激活门控）。

- [ ] **Step 2: 构建与回归**（同 Task 1 Step 4 命令；重点 `test_fat32_basic`）

- [ ] **Step 3: Commit** `git commit -m "feat(fs): scheduler-backed per-mount mutex for FAT32"`

### Task 3: tmpfs per-mount ctx + mutex 化

**Files:**
- Modify: `kernel/fs/tmpfs.c`（全局 `static spinlock_T tmpfs_lock`（:61）删除；`tmpfs_fs_mount`/`tmpfs_fs_unmount`（:753/:743）；`tmpfs_get_node`（:94-99）；全部取 `tmpfs_lock` 的 ops）

**Interfaces:**
- Consumes: Task 1 的 `vfs_sched_ready()`。
- Produces: tmpfs.c 内部类型（不上头文件）：

```c
typedef struct tmpfs_mount_ctx {
    mutex_t       lock;   /* per-mount 大锁，mutex_init 于挂载时 */
    tmpfs_node_t *root;   /* 原 mp->fs_data 直接存的根节点 */
} tmpfs_mount_ctx_t;
```

`mp->fs_data` 语义变更：`tmpfs_node_t*` → `tmpfs_mount_ctx_t*`。新增 `static tmpfs_mount_ctx_t *tmpfs_ctx_of(vfs_node_t *node)`（`node->mount->fs_data`）与 `static tmpfs_node_t *tmpfs_root_of(vfs_node_t *node)`（`tmpfs_ctx_of(node)->root`，替代原 `tmpfs_get_node` 的 root 分支）。锁 helper `tmpfs_lock_ctx(ctx)`/`tmpfs_unlock_ctx(ctx)`（`vfs_sched_ready()` 门控，模式同 Task 1）。

- [ ] **Step 1: 类型与挂载/卸载改造**。`tmpfs_fs_mount`：分配 ctx（calloc）、`mutex_init(&ctx->lock)`、root 建好后 `ctx->root = root; mp->fs_data = ctx;`；失败路径释放。`tmpfs_fs_unmount`：取 ctx、加锁、`tmpfs_free_node(ctx->root)`、解锁、`kfree(ctx)`、`mp->fs_data = NULL`。改前 `grep -c "tmpfs_lock" kernel/fs/tmpfs.c` 清点，改后所有站点经 ctx 取锁。

- [ ] **Step 2: 全部 ops 换 ctx 锁**，root 访问处（原 `mount->fs_data` 直转 `tmpfs_node_t*`）改经 `tmpfs_root_of`。

- [ ] **Step 3: 构建与回归**：`make && make test-host && make test-static` 全绿。

- [ ] **Step 4: Phase 1 收尾 boot 验证**

Run: `make OS01_SYSTEST=1 test-qemu SUITE=systest`
Expected: 全部已登记 systest 通过；真实锁竞争/睡眠证据由 Task 7/9 补充，不以这一轮串行用例推断已覆盖。（spec §4.4 的 headless Ctrl-C 冒烟统一在 Task 8 执行，此处不重复。）

- [ ] **Step 5: Commit** `git commit -m "feat(fs): per-mount scheduler-backed mutex for tmpfs"`

### Task 4: bcache 读核心、状态观测与宿主等待环境

**Files:**
- Create: `kernel/block/bcache.c` ↔ `kernel/include/block/bcache.h`
- Modify: `kernel/block/blockdev.c`（初始化、注册错误、静默退役/reset 接线；本 Task 暂不分流 read/write）
- Create: `hosttests/cases/test_bcache.c`
- Create: `hosttests/mock/arch6_bcache_runtime.h`、`hosttests/mock/arch6_bcache_runtime.c`
- Modify: `hosttests/mk/drivers.mk`、`config/test-scenarios.json`

**Interfaces:**

```c
#define BCACHE_BUFS 2048
#define BCACHE_BUCKETS 512
int bcache_init(void); /* 0 或 -ENOMEM，成功后幂等 */
int bcache_read_sector(block_device_t *dev, uint64_t lba, void *buf);
int bcache_write_sector(block_device_t *dev, uint64_t lba, const void *buf);
int bcache_flush_device(block_device_t *dev);
void bcache_drop_device(block_device_t *dev); /* 仅静默设备，丢弃不写回 */
#if defined(OS01_HOST_TEST) || defined(OS01_SELFTEST)
typedef struct {
    uint32_t entries, pinned, busy, dirty, waiters;
} bcache_test_stats_t;
void bcache_reset_for_test(void); /* 全局静默，清项但保留池 */
void bcache_debug_stats(block_device_t *dev, bcache_test_stats_t *out);
int bcache_debug_pin(block_device_t *dev, uint64_t lba);
void bcache_debug_unpin(block_device_t *dev, uint64_t lba);
#endif
#ifdef OS01_HOST_TEST
int bcache_test_reserve_busy(block_device_t *dev, uint64_t lba);
void bcache_test_complete_busy(block_device_t *dev, uint64_t lba,
                              int io_result, const void *sector);
#endif
```

`pinned` 统计 refcnt>0 的项数，`waiters` 按请求设备统计已 ARM、尚未清理的等待操作；统计受元数据锁保护。观察/注入等待窗口可用 host 链接器 wrap `wait_queue_arm` / `schedule`，内核以原子旗标、worker task state 和上述统计协调，不添加生产 I/O 绕路。

HOST_TEST 专用 reserve/complete 只构造没有外层读 API 调用栈的填充预留：reserve 仅接受缺失 key，按真实发布 helper 建 BUSY 项并持专属测试 pin（已存在返回 EEXIST，暂无 free 槽返回 ENOMEM）；complete 校验 key 和测试所有权，成功时复制给定512B并经真实成功收尾 helper 清 BUSY/置 VALID，失败时走真实回收 helper，最后释放测试 pin并唤醒。无所有权、重复 complete 或成功却传 NULL 均由宿主断言报错；不得释放其他调用方的 pin。这里只验等待握手，真实在途 ops->read 竞争由 Task 7 覆盖。

- [ ] **Step 1: 写失败用例并登记**。直接调用 bcache 读 API（包装层仍直通，避免只缓存读而直写引入过时命中）：
  - `read_miss_fills`：同 LBA 连读两次，后端 reads=1，数据一致；按设备 entries=1、pinned=busy=0。
  - `read_error_retries`：首次后端 EIO，返回 EIO、entries=pinned=busy=0；后端恢复后重读成功，reads=2。
  - `reset_drops_entries` / `unregister_drops_entries`：静默 reset 或 boot unregister 后测试设备 entries=0；不得通过已退役 ops 写回。
  - `init_oom_retry`：首次初始化各分配点逐一失败，无泄漏/半发布；重试成功；初始化后的 reset 不重复分配池。以独立测试进程/初始化前用例避免重置在用池。
  - `busy_wake_before_arm` / `busy_wake_after_arm`：先 test_reserve_busy，再调用真实 bcache_read_sector，由 ARM 前/后脚本 test_complete_busy 推进预留；断言读到填充模式、不发生无未来生产者的睡眠、队列清空且 IRQ 状态恢复。禁止在停住的 fake read 回调栈内嵌套同 key 读；另验错误 complete 后可正常重试及重复 complete 被拒绝。
  Run: `make test-host`；Expected: 新接口/实现尚不存在导致失败。
- [ ] **Step 2: 建立宿主运行环境**。arch6 runtime 复用 arch9 block_runtime 的分配/日志表面，补最小 task/current、IRQ、schedule/task_wake；链接生产 `kernel/sync/wait.c`。默认 schedule 对非预期等待直接报错；test_bcache 可设置完成脚本（参考 test_mutex_handshake，不能用空 wait stub 掩盖错误）。
  drivers.mk 将该 runtime force-include 到 bcache 和专用 wait 对象；`blockdev_production.o` 的三个消费者 test_block_device_ops/test_ahci_lifecycle/test_device_boot 及新增 test_bcache 都追加 bcache、专用 wait、公共 runtime 对象。各对象编译时 spinlock/wait/task 布局必须一致；新 elf 登记四处和 scenario catalog。
- [ ] **Step 3: 实现初始化与读状态机**。元数据池/1 MiB 数据池分开分配；锁内先发布 BUSY key + refcnt=1，锁外读，成功清 BUSY但保留 pin到拷出后，失败撤销项并归还槽。所有 flags 在元数据锁内更新；data_lock irqsave 内只 memcpy；等待遵循 spec §5.3，唤醒在元数据锁外。此 Task 分配仅用 free list，耗尽暂返 ENOMEM，Task 6 替换。
  `block_device_init(void)` 保持签名；缓存初始化失败不得置 ready。register 在参数校验后检查 `bcache_init()` 返回值再发布设备；read API 也检查初始化结果。unregister 成功后静默 drop（拒绝 foreign dev/EBUSY 时不得 drop）；reset 在释放设备对象前清缓存。实现 HOST_TEST reserve/complete 并复用真实发布/收尾 helper，不复制一套状态机。后续 write/flush API 可暂返 ENOSYS，不接到生产包装层。
- [ ] **Step 4: 回归**。Run: `make test-host && make test-static`。Expected: 全绿。既有 registry OOM 计数测试在注入前初始化缓存并清零调用计数；不要把池分配混入设备对象计数。先验证失败原因，再报告测试绿。
- [ ] **Step 5: Commit** `git commit -m "feat(block): cache read core and deterministic wait harness"`

### Task 5: 同时接入缓存读写、同步 flush 与 SYS_sync

**Files:**
- Modify: `kernel/block/bcache.c`、`kernel/block/blockdev.c`
- Modify: `kernel/syscall/sys_misc.c`
- Modify: `hosttests/cases/test_bcache.c`、`hosttests/cases/test_block_device_ops.c`

**Interfaces:** Consumes Task 4。物理盘 read/write 都逐 sector 经 bcache；分区不缓存，继续 ops 透传到父盘。`bcache_flush_device` 采用 spec §5.5 的 key 快照，返回 0 或原始负错误；`block_device_flush` 先软件缓存、再可选驱动 flush；`SYS_sync` 尝试所有 BLOCK_DISK，返回首个错误或 0。

- [ ] **Step 1: 添加失败用例**：
  - `partition_key_convergence`：子设备读填充一项；再次子读、父盘同绝对 LBA 读均不增加后端 read；子写后父读立即见新值；子 flush 使父后端得新值；按父设备 entries=1、按子设备 entries=0。
  - `write_back_read_your_writes`：包装写后后端仍旧值，包装读新值；`read_write_read_coherent` 覆盖先读热缓存再写同 LBA。
  - `flush_persists_to_backend`：flush 后后端新值，第二次无额外后端 write。
  - `flush_waits_for_pin`：写已完成→debug_pin→flush；schedule 脚本确认后端尚旧、DIRTY 保留、flush 未返回，再 unpin；flush 返回后后端新值、busy/pinned/waiters/dirty=0。
  - `flush_failed_write_keeps_dirty`：后端 write=EIO，flush 返回 EIO、保留新数据/dirty；恢复后重试成功。
  - `flush_without_driver_callback`：ops.flush=NULL 仍软件写回；`driver_flush_error` 检查后端 write 先完成，再返回驱动 flush 错误及 last_error。
  - `flush_drain_wake_windows`：ARM 前/后释放旧 pin均能完成；BUSY 预留后新 pin不得越过，旧 pin可以提交 flags并释放。
  Run: `make test-host`；Expected: 新行为断言失败。
- [ ] **Step 2: 实现写状态机与双向接线**。整 sector 新写发布 BUSY key + pin，无需先读；数据复制后锁内提交 VALID|DIRTY，保留维护方 BUSY，最后解 pin。read/write 同次变更一起分流，保留所有参数/只读校验以及 last_error 语义。
- [ ] **Step 3: 实现 flush 与 syscall**。锁外分配目标数组，锁内快照全部 DIRTY key（含 pinned/BUSY）。逐 key重查、置 BUSY阻止新 pin、等旧 pin归零，锁外写回；只有成功才清 DIRTY；所有出口清自有 BUSY并唤醒。快照后新增 key不纳入；不存在/已干净的目标代表之前成功写回。多 flush靠 BUSY协调，不存可复用槽指针。包装层缓存失败立即返回并更新 last_error，成功才调用可选 ops.flush；SYS_sync 失败后继续其他盘并返回首错。
- [ ] **Step 4: 更新旧计数断言并回归**。test_block_device_ops 的 LBA10,count5 变为 reads=5,last_lba=14,last_count=1；写 LBA20,count2 在 flush 前 writes=0，之后 writes=2,last_lba=21,last_count=1。单 sector 计数保持对应语义，所有用例先静默 reset。Run: `make test-host && make test-static`；Expected: 全绿。
- [ ] **Step 5: Commit** `git commit -m "feat(block): coherent cached IO and synchronous flush; wire SYS_sync"`

### Task 6: LRU、失败可恢复的脏逐出与耗尽等待

**Files:** Modify `kernel/block/bcache.c`、`hosttests/cases/test_bcache.c`。

**Interfaces:** Consumes Tasks 4/5；替换 free-list-only ENOMEM 桩。选 refcnt=0 且非 BUSY 的 LRU 项，先保留旧 key并置 BUSY，脏写回成功后才复用；失败返原错且保留旧数据。无可用槽按 spec §5.3 等待，不因暂时占满返回 ENOMEM。

- [ ] **Step 1: 添加失败用例**：
  - `evict_writes_back`：写 BCACHE_BUFS+16 个 sector，未 flush 时只断言最早被逐出的 16 项已在后端；驻留项由包装读校验；显式 flush 后再校验全部后端。entries=BCACHE_BUFS。
  - `lru_preserves_hot`：写满、读热最早项，再写 8 项；热项再读不增加后端 reads。
  - `evict_error_preserves_victim`：池满后注入受害项 write=EIO，新请求返回 EIO，旧 key/数据/DIRTY 仍在且无自有 BUSY/pin；恢复后逐出成功。
  - `evict_rechecks_requested_key`：在脏受害项后端写回回调中，让脚本为目标新 key先完成发布；原请求重查后复用已有项，不产生双 key；归还未使用槽。
  - `exhaustion_waits_for_unpin`：pin 满池，请求新 key，由 schedule 脚本释放一项；请求完成而非 ENOMEM；全释放后 pinned=busy=waiters=0。
  - `exhaustion_wake_windows`：在 ARM 前/后释放最后可用 pin，均无丢唤醒。
  Run: `make test-host`；Expected: 新逐出/耗尽用例行为失败。
- [ ] **Step 2: 实现**。LRU 命中更新；锁外脏写回后锁内重新查目标 key，已有则归还候选槽并重试。失败不能移除旧 key或清 DIRTY，必须清自有 BUSY并唤醒。解 pin到零、BUSY清除、槽回收均在元数据锁外 wake_all；统计与所有错误出口配对。
- [ ] **Step 3: 回归**。Run: `make test-host && make test-static`；Expected: 全绿。
- [ ] **Step 4: Commit** `git commit -m "feat(block): recoverable LRU write-back and exhaustion wait"`

### Task 7: 调度器就绪后的真实 SMP 缓存自测

**Files:**
- Create: `kernel/selftest/test_bcache.c`
- Modify: `kernel/selftest/selftest.c`、`kernel/arch/x86_64/cpu/sched_boot.c`、`kernel/core/main.c`

**Interfaces:** Consumes Task 4 调试接口和 `kernel_thread_on_cpu`。新增 late id `bcache_smp_coherence`：在 x86 OS01_SELFTEST 下 `selftest_register_late`；sched_boot 在 scheduler_ok=1 后执行 `test_bcache_smp_coherence()`，用 `selftest_record_late` 报结果，在 selftest_end_run 前完成。main 中 `selftest_begin_run(8)` 同步改为 9；不得用 early selftest_register(fn)。

- [x] **Step 1: 实现自测和 late 接线**。fake 盘 backend 至少 `(BCACHE_BUFS+32)*512` 字节，注册 bcachetest；父 idle harness 只发起 worker、观察/超时/yield，所有可能等待的 bcache 操作由 pid>0 的 pinned worker 执行；两个 CPU 均须 online/scheduler_ok，否则明确失败，不宣称覆盖。
  - `same_sector_atomic`：先 seed PATTERN_A，再以原子握手保证读写双方在整个压力窗口均有进展；读到的完整 512B 只能是 A/B，不以仅启动了两线程作为重叠证据。
  - `same_sector_concurrent_miss`：fake read 回调停住首个未缓存 key 的填充，另一 CPU读同 key须进入 BUSY等待；释放后两方数据一致，后端 reads只增1、测试设备该 key唯一。
  - `adjacent_sector_isolation`：两个 CPU各写 LBA10/11，结束后完整校验最终模式。
  - `flush_waits_for_reader`：已完成写后 pin，另一 worker flush；观察该 worker已 ARM、任务处于等待且尚未完成，释放 pin后 flush成功，后端新值。
  - `exhaustion_waits`：worker pin满2048槽；另一worker读新key，父harness观察 waiters及等待任务状态，确认未完成后通知持pin worker释放一项；新请求完成后清理全部pin。fake盘填满时可正常逐出真实启动盘缓存，禁止调用全局 reset。
  - `flush_backend_visibility` / `no_leaks`：workers结束后由worker flush并直读fake数组；按测试设备 pinned=busy=waiters=0，静默 unregister 后 entries=0。不得把全局占用数与 fake盘项数比较。
  所有等待沿用 fd_refcount 的 jiffies预算与 IRQ恢复约定；超时先设 abort、释放测试pin/后端停顿、等待workers退出，再退役设备；未确认退出则不得清理仍在访问的数据或报告PASS。
- [x] **Step 2: 验证**。Run: `make KERNEL_SELFTEST=1 test-kernel-selftest`（不得 OS01_SYSTEST）；Expected: 协议含 SELECT/BEGIN/PASS bcache_smp_coherence，9个late case均记录、单一END，全套通过；保留阻塞→释放→完成证据。
- [x] **Step 3: Commit** `git commit -m "test(kernel): late SMP cache coherence and exhaustion selftest"`

### Task 8: 缓存阶段全量回归与正常交互盘冒烟

- [ ] **Step 1: Run** `make test-host && make test-static && make OS01_SYSTEST=1 test-qemu SUITE=systest`；Expected: 全绿。
- [ ] **Step 2: 正常盘冒烟**。先 `make PROFILE=x86_64-clang image`（不带测试旗标），通过 `print-run-paths` 取得本次正常 firmware/image，执行 AGENTS.md 的 cat /dev/urandom→Ctrl-C→SHELL_ALIVE_123；Expected: cat被SIGINT终止、shell存活。不得误用前一步systest镜像。
- [ ] **Step 3: 如有修复，回归其受影响用例后 Commit** `git commit -m "fix(block): ARCH-6 cache regression fixes"`。

### Task 9: 四核用户态 FS 压测与 fail-closed 解析

**Files:**
- Create: `user/test_arch6_fsstress/test_arch6_fsstress.c`、`user/test_arch6_fsstress/app.mk`（`APP_SOURCES := test_arch6_fsstress.c`）
- Create: `config/inittab.arch6-fsstress`（`tty1:once:/bin/test_arch6_fsstress`）
- Modify: `Makefile`、`mk/project.mk`、`config/rootfs.mk`、`mk/components/run.mk`
- Modify: `qemutests/run_test.py`（显式 suite 分支；现有框架不自动发现新解析器）
- Create: `qemutests/arch6_fsstress.py`、`qemutests/test_arch6_fsstress.py`

**Interfaces:** 4个worker×3个FS；逐项 `[arch6-fsstress] <fs> worker<N> PASS`，失败 `FAIL(<detail>)`；只有父进程确认全部子进程exit0及12个结果后打印最终 `[arch6-fsstress] PASS`。命令 `make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress`；有效QEMU核数必须为4。

- [ ] **Step 1: 先写解析/启动参数失败测试**。缺少/重复worker结果、任意FAIL、缺最终PASS、只有最终PASS、panic、非四核或SMP/QEMU_SMP冲突均不能通过；正常完整日志及四核参数通过。测试不实际启动QEMU。将 `qemutests.test_arch6_fsstress` 加入 TEST_HARNESS_MODULES，Run: `python3 -m unittest qemutests.test_arch6_fsstress`；Expected: 新解析器尚无导致失败。
- [ ] **Step 2: 实现压测**。父进程预建 `/arch6stress`、`/tmp/arch6stress`、`/boot/arch6stress`，每个根内预建并列的 `files/`、`shared/`。每worker独占 `files/f<N>`，写32段签名数据，读回逐段校验，rename到 `files/f<N>.done`；全员rename后，读取相邻worker文件并按邻居签名校验；全员读完后各删自己的文件；全员删完由父进程确认 `files/` 为空。
  shared阶段全员屏障后各建唯一文件，全员建完后各删自己的文件，全员删完由父检查 `shared/` 的readdir为空（忽略`.`/`..`）。父进程最后依次rmdir files/、shared/、测试根目录；不在shared仍存在时断言测试根为空，不由worker相互unlink。
  屏障用一根共享child→parent事件管道、四根parent→child释放管道（创建时10个FD+标准3个，不超过默认16）；fork后关闭无关端。固定小事件记录以单次write发出，包含worker/fs/阶段/成功状态；父对每阶段收齐四个唯一事件再释放。poll带有界预算；任何FAIL/EOF/超时立即终止并waitpid回收其余子进程，禁止依赖文件轮询充当屏障。只在全部阶段与子进程退出成功后发最终PASS。
- [ ] **Step 3: 构建与runner接线**。
  - Makefile 在 include project.mk 前按旗标选择 inittab；project.mk 定义0/1校验，新增与 systest/nettest/selftest/canary/arch10/分辨率/terminal-present/ARCH9_FAULT/X86_INTR_FAULT_CASE/自定义测试inittab 的互斥检查；已有互斥列表同步加入新旗标。IMAGE_VARIANT/USER_VARIANT 加 arch6-fsstress；OS01_SUBMAKE_ALLOWED 加旗标；不新增内核fixture宏。
  - rootfs三分支过滤：arch6排除sh/test_arch10_capacity；arch10排除sh/test_arch6_fsstress；正常排除sh和两套专用程序。用户app自动发现，无需额外登记。
  - run.mk 加 TEST_ARCH6_FSSTRESS_IMAGE、TEST_QEMU_IMG_arch6-fsstress、TEST_QEMU_FLAVOR_arch6-fsstress=ARCH6_FSSTRESS_TEST=1、suite白名单/usage/顶层旗标gate；此suite要求SMP=4。runner启动前若QEMU_SMP已设置且不等于SMP则报错，再为本suite显式传 `QEMU_SMP="$(SMP)"`；其他suite保持原行为。
  - run_test.py 显式分派 test_arch6_fsstress(tester)；新函数启动前校验 tester._effective_smp == "4"，总超时180s，解析完整累计串口日志，核验percpu四核证据、12个唯一结果、无FAIL/panic和父最终PASS。只调用一次start_qemu并由公共finally清理；保留标准结果归档。
- [ ] **Step 4: 验证**。Run: `python3 -m unittest qemutests.test_arch6_fsstress`、`make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress`（连续两次）、`make test-qemu SUITE=phase-0`。Expected: 单测绿、每次12项+最终PASS、实际四核，正常盘可启动；检查正常rootfs manifest未安装专用压测程序，不以phase-0启动成功替代过滤检查。
- [ ] **Step 5: Commit** `git commit -m "test(e2e): four-CPU concurrent FS stress suite"`

### Task 10: 文档闭环与终验

**Files:** `docs/roadmap.md`、`docs/changelog.md`、`docs/fs/filesystem.md`、`docs/build/build.md`。

- [ ] **Step 1: 更新文档**。ARCH-6移入已闭环清单；记录实际日期/commit范围；说明FS mutex、缓存锁/pin/BUSY/等待握手、分区key、错误返回、flush快照边界及后端可见性。不得写成掉电持久性保证；build文档登记旗标与带SMP=4的suite命令。
- [ ] **Step 2: 终验**。Run: `make test-host && make test-static && make OS01_SYSTEST=1 test-qemu SUITE=systest && make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress && make KERNEL_SELFTEST=1 test-kernel-selftest`；Expected: 全绿，有实际四核和late协议证据。
- [ ] **Step 3: Commit** `git commit -m "docs: close ARCH-6 (FS concurrency and block cache)"`。
- [ ] **Step 4: 汇报**。列出实际执行的验证与限制，合并由用户决定；遵循AGENTS.md的fast-forward优先规则。

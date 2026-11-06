# ARCH-6 FS 并发与块缓存 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 三大 FS 大锁 mutex 化（tmpfs per-mount 化）+ blockdev 层 (dev, LBA) 写回式块缓存 + SYS_sync 接线 + SMP 并发压测闭环。

**Architecture:** Phase 1 把 ext2/FAT/tmpfs 的「spinlock 持锁跨 AHCI 忙等 I/O」换为调度器支持的 per-mount `mutex_t`（pre-SMP 挂载期用 `vfs_sched_ready()` 门控豁免）；Phase 2 在 `block_device_read/write` 包装层下植入 sector 粒度哈希缓存（仅物理盘 key，写回 + 显式 flush）；Phase 3 以 arch10-capacity 模式的用户态 fork 压测 + 内核 pinned-kthread 自测验收。

**Tech Stack:** C（内核/宿主两侧）、make 构建体系、QEMU E2E（python 标记解析）。

**Spec:** `docs/superpowers/specs/2026-11-07-arch6-fs-concurrency-blockcache-design.md`（本计划从 spec 论证，两者一起阅读）

## Global Constraints

- 分支：`feature/arch6-fs-concurrency-blockcache`（worktree `.worktrees/arch6-fs-concurrency-blockcache`），每个 Task 一组 commit。
- 目录规范：新源文件必须源/头成对（`kernel/block/bcache.c` ↔ `kernel/include/block/bcache.h`）；头统一 `#include <block/bcache.h>`。
- 日志规范：新内核代码只用 `log_err/log_warn/log_info/log_debug`（或 `debug_fs/debug_block`），禁 `serial_printk`/`color_printk`。
- 锁序不变量（全期）：`FS mutex → mount->active_lock → bcache 元数据锁（叶子）→ AHCI port lock`；bcache 元数据锁内无 I/O、无分配。
- 零 UAPI 变更：不改 `kernel/include/uapi/`（`SYS_sync` 已存在，只补实现）。
- `make test-syscall` 必须带 `OS01_SYSTEST=1` 前缀且**不得**同时设 `KERNEL_SELFTEST=1`（AGENTS.md gotcha）。
- 每个 `register_host_test` 必须同步 `config/test-scenarios.json` 条目（CI 首个未映射项即停）。
- kernel 源列表自动发现：`kernel/Makefile:127` `$(wildcard block/*.c)`，新增 `.c` 无需登记；hosttest 需在 `hosttests/mk/drivers.mk` 登记三处（register_host_test / elf 链接行 / 对象规则）。

## Review Focus

spec 隐含但无任务测试会踩的输入类别（每行已 pinned 到拥有它的 Task）：

1. **分区设备绕过缓存**（`dev->parent != NULL` 若不透传 → 同一物理扇区双 key + 分区脏数据永不落盘）。→ Task 4 Step 2 的 `partition_bypass` 用例：带 parent 的设备读后 `bcache_debug_count()` 不变。
2. **同 sector 裸并发撕裂**（无 FS mutex 的双 CPU 读/写必须读到某次完整写入）。→ Task 7 Step 1 的 `same_sector_atomic` 断言：读到的字节序属 `{PATTERN_A, PATTERN_B}` 全集，无混合。
3. **flush 对在途缓冲提前清 DIRTY**（丢写且缓存自称干净）。→ Task 5 Step 1 的 `flush_skips_inflight` 用例：pin 住后 flush，后端读旧值且缓冲仍 DIRTY；解 pin 再 flush 落盘。
4. **脏块逐出未写回**（静默丢数据）。→ Task 6 Step 1 的 `evict_writes_back` 用例：写满池后绕过包装直读后端校验。
5. **设备退役/reset 后悬挂缓存项**（hosttest 用例间串扰）。→ Task 6 Step 1 的 `reset_drops_entries` 用例：reset 后 count 归零且重读重新填充。

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

- [ ] **Step 1: 在 `vfs.c` 实现 `vfs_sched_ready()`，`vfs.h` 加声明**（`#include <stdbool.h>` 若缺）。紧邻 `vfs_ns_lock` 放置，注释说明与 `vfs_ns_lock` 同一豁免语义（pre-SMP 单核无竞争）。

- [ ] **Step 2: ext2 锁类型与 helper 替换**。`ext2.h`：`spinlock_T lock` → `mutex_t lock`（补 `#include <sync/mutex.h>`）。`ext2.c`：`spin_init(&fs->lock)` → `mutex_init(&fs->lock)`；新增 `ext2_lock/ext2_unlock` helper；逐函数把每个 `spin_lock(&fs->lock)`/`spin_unlock(&fs->lock)` 对替换为 helper（含早退路径上的 unlock——用 `grep -n "fs->lock" kernel/fs/ext2.c` 清点，改前改后站点数一致）。`_locked` 后缀函数注释更新为「caller holds mutex」。

- [ ] **Step 3: 构建与既有测试回归**

Run: `make && make test-host && make test-static`
Expected: 构建零警告新增；test-host 全绿（重点 `test_vfs_iops_fops`、`test_fat32_basic`、`test_block_device_ops`）；test-static 三项审计通过。

- [ ] **Step 4: Commit** `git commit -m "feat(fs): scheduler-backed per-mount mutex for ext2"`

### Task 2: FAT mutex 化

**Files:**
- Modify: `kernel/include/fs/fat.h:113`（`spinlock_T fat_lifecycle_lock` → `mutex_t lock`）
- Modify: `kernel/fs/fat.c`（全部 `fat_lifecycle_lock` 站点；`spin_init` 处）
- Check: `grep -rn "fat_lifecycle_lock" hosttests/ user/`（hosttest 若引用字段则同步）

**Interfaces:**
- Consumes: Task 1 的 `vfs_sched_ready()`。
- Produces: `fat32_fs_t.lock`（`mutex_t`）+ helper `static void fat_lock(fat32_fs_t*)` / `fat_unlock(fat32_fs_t*)`（fat.c 内部，模式同 Task 1）。

- [ ] **Step 1: 字段改名 + 类型替换 + helper 替换**，替换前 `grep -c "fat_lifecycle_lock" kernel/fs/fat.c` 清点站点数，改后用 `fat_lock(&fs->lock)` / `fat_unlock(&fs->lock)`（或等价命名）逐一配对替换。

- [ ] **Step 2: 构建与回归**（同 Task 1 Step 3 命令；重点 `test_fat32_basic`）

- [ ] **Step 3: Commit** `git commit -m "feat(fs): scheduler-backed per-mount mutex for FAT32"`

### Task 3: tmpfs per-mount ctx + mutex 化

**Files:**
- Modify: `kernel/fs/tmpfs.c`（全局 `static spinlock_T tmpfs_lock`（:61）删除；`tmpfs_fs_mount`/`tmpfs_fs_unmount`（:753/:743）；`tmpfs_node_of`（:94-99）；全部取 `tmpfs_lock` 的 ops）

**Interfaces:**
- Consumes: Task 1 的 `vfs_sched_ready()`。
- Produces: tmpfs.c 内部类型（不上头文件）：

```c
typedef struct tmpfs_mount_ctx {
    mutex_t       lock;   /* per-mount 大锁，mutex_init 于挂载时 */
    tmpfs_node_t *root;   /* 原 mp->fs_data 直接存的根节点 */
} tmpfs_mount_ctx_t;
```

`mp->fs_data` 语义变更：`tmpfs_node_t*` → `tmpfs_mount_ctx_t*`。新增 `static tmpfs_mount_ctx_t *tmpfs_ctx_of(vfs_node_t *node)`（`node->mount->fs_data`）与 `static tmpfs_node_t *tmpfs_root_of(vfs_node_t *node)`（`tmpfs_ctx_of(node)->root`，替代原 `tmpfs_node_of` 的 root 分支）。锁 helper `tmpfs_lock_ctx(ctx)`/`tmpfs_unlock_ctx(ctx)`（`vfs_sched_ready()` 门控，模式同 Task 1）。

- [ ] **Step 1: 类型与挂载/卸载改造**。`tmpfs_fs_mount`：分配 ctx（calloc）、`mutex_init(&ctx->lock)`、root 建好后 `ctx->root = root; mp->fs_data = ctx;`；失败路径释放。`tmpfs_fs_unmount`：取 ctx、加锁、`tmpfs_free_node(ctx->root)`、解锁、`kfree(ctx)`、`mp->fs_data = NULL`。改前 `grep -c "tmpfs_lock" kernel/fs/tmpfs.c` 清点，改后所有站点经 ctx 取锁。

- [ ] **Step 2: 全部 ops 换 ctx 锁**，root 访问处（原 `mount->fs_data` 直转 `tmpfs_node_t*`）改经 `tmpfs_root_of`。

- [ ] **Step 3: 构建与回归**：`make && make test-host && make test-static` 全绿。

- [ ] **Step 4: Phase 1 收尾 boot 验证**

Run: `make OS01_SYSTEST=1 test-qemu SUITE=systest`
Expected: 228 项 systest 全通过（mutex 路径在 SMP 下真实睡眠/唤醒无死机）。

- [ ] **Step 5: Commit** `git commit -m "feat(fs): per-mount scheduler-backed mutex for tmpfs"`

### Task 4: bcache 骨架 + 读路径 + hosttest

**Files:**
- Create: `kernel/include/block/bcache.h`
- Create: `kernel/block/bcache.c`
- Modify: `kernel/block/blockdev.c`（`block_device_init` 调 `bcache_init()`；`block_device_read` 包装在参数校验后、`dev->ops->read` 前分流；`block_device_reset_for_test` 调 `bcache_reset_for_test()`）
- Test: `hosttests/cases/test_bcache.c`
- Modify: `hosttests/mk/drivers.mk`（三处登记，模式照抄 `test_block_device_ops`）

**Interfaces:**
- Produces（`kernel/include/block/bcache.h`，消费方仅 blockdev.c 与测试）：

```c
void bcache_init(void);
int  bcache_read_sector(block_device_t *dev, uint64_t lba, void *buf);   /* 512B */
int  bcache_write_sector(block_device_t *dev, uint64_t lba, const void *buf);
int  bcache_flush_device(block_device_t *dev);
void bcache_drop_device(block_device_t *dev, bool writeback);
#ifdef OS01_HOST_TEST
void     bcache_reset_for_test(void);
uint32_t bcache_debug_count(void);              /* 当前占用槽位数 */
int      bcache_debug_pin(block_device_t *dev, uint64_t lba);   /* 模拟在途 */
void     bcache_debug_unpin(block_device_t *dev, uint64_t lba);
#endif
```

- blockdev.c 分流规则：`block_device_read/write` 校验通过后 `if (dev->parent == NULL)` 逐 sector 调 `bcache_read_sector/bcache_write_sector`（memcpy 进出调用方缓冲）；否则直调 `dev->ops->…`（分区透传，spec §5.2）。`block_device_write` 保留原校验与 `last_error` 记账。bcache 内部写回/填充直调 `dev->ops->read/write`（绕过包装防递归）。
- 数据结构按 spec §5.2：`BCACHE_BUFS 2048`、`BCACHE_BUCKETS 512`、`bcache_buf_t`（hash_next/lru 双向链/`dev`/`lba`/`flags: VALID|DIRTY|BUSY`/`refcnt`/`data_lock`/`data`）、全局 `bcache_lock`（irqsave）+ free list + LRU 头 + 全局 `bcache_wq`。读缺失：取槽（脏先写回）→ BUSY → `ops->read` → 置 VALID 挂链清 BUSY → `wait_queue_wake_all(&bcache_wq)`；同 sector 后到者见 BUSY 睡眠重查（spec §5.4）。命中：pin → `data_lock` 内 memcpy → 解 pin。

- [ ] **Step 1: 写 hosttest `test_bcache.c`**（fake 块设备模式照抄 `test_block_device_ops.c`：内存后端数组 + read/write 计数器；用 `block_device_register` 注册。分区透传用例需注册一个 `parent` 指向已注册盘、`kind = BLOCK_PARTITION` 的子设备——直接构造 `block_device_t` 结构体传注册函数即可）。用例：
  - `read_miss_fills`：读 LBA5 两次，后端 read 计数 == 1，两次数据一致；
  - `partition_bypass`：对 parented 设备读后 `bcache_debug_count()` 与读前相同；
  - `reset_drops_entries`：读若干扇区后 `bcache_reset_for_test()`，`bcache_debug_count() == 0`，再读重新填充。
- [ ] **Step 2: 登记 + 跑红**：`hosttests/mk/drivers.mk` 三处登记 + `config/test-scenarios.json` 条目（照抄 `test_block_device_ops` 条目结构）。Run: `make test-host 2>&1 | tail -5` Expected: `test_bcache` 编译失败（bcache.h 不存在）。
- [ ] **Step 3: 实现 bcache.h/bcache.c 与 blockdev 分流**（读路径 + init/reset/pin 钩子；写路径下一 Task）。
- [ ] **Step 4: Run: `make test-host`** Expected: `test_bcache` 3 用例 PASS，全宿主套件绿。
- [ ] **Step 5: Commit** `git commit -m "feat(block): sector-granular block cache with read path"`

### Task 5: bcache 写路径 + flush + SYS_sync 接线

**Files:**
- Modify: `kernel/block/bcache.c`（`bcache_write_sector`、`bcache_flush_device`）
- Modify: `kernel/block/blockdev.c`（写分流已在 Task 4 接好则无改动；`block_device_unregister_boot` 调 `bcache_drop_device(dev, false)`）
- Modify: `kernel/syscall/sys_misc.c:40-45`（`SYS_sync` 分支）
- Test: `hosttests/cases/test_bcache.c`（追加用例）

**Interfaces:**
- Consumes: Task 4 全部接口。
- Produces: `SYS_sync` 语义 = 对 registry 内每个 `kind == BLOCK_DISK` 的设备 `block_device_flush(dev)`（用 `block_device_get(i)` 枚举至 NULL，跳过 `kind != BLOCK_DISK`），返回 0。

- [ ] **Step 1: 追加失败用例**：
  - `write_back_read_your_writes`：写 LBA7 → 后端直读（fake 后端数组）仍旧值；包装读得新值；
  - `flush_persists`：flush 后后端直读得新值；二次 flush 无后端写（write 计数不增）；
  - `flush_skips_inflight`：写 LBA8 → `bcache_debug_pin` → flush → 后端旧值（未落盘）→ `bcache_debug_unpin` → flush → 后端新值。
- [ ] **Step 2: Run: `make test-host`** Expected: 新 3 用例 FAIL（write 未实现/直通后端）。
- [ ] **Step 3: 实现**：写路径——取/分配缓冲（缺失不必先填充：整 sector 写覆盖）→ pin → `data_lock` 内 memcpy → 置 `VALID|DIRTY` → 解 pin。flush——遍历池中 `dev` 匹配项：`refcnt > 0 || BUSY` 跳过（保持 DIRTY），否则 `dev->ops->write` 后清 DIRTY（spec §5.5）。`SYS_sync` 按上述枚举接线。
- [ ] **Step 4: Run: `make test-host`** Expected: 全绿。
- [ ] **Step 5: Commit** `git commit -m "feat(block): write-back cache with explicit flush; wire SYS_sync"`

### Task 6: LRU 逐出 + 脏写回 + 耗尽等待

**Files:**
- Modify: `kernel/block/bcache.c`
- Test: `hosttests/cases/test_bcache.c`（追加用例）

**Interfaces:**
- Consumes: Task 4/5 全部接口。
- Produces: 逐出语义——分配缺失槽时取 free list，否则取 LRU 尾（`refcnt == 0 && !BUSY`）；脏尾先 `ops->write` 写回再复用；无可逐出槽在 `bcache_wq` 睡眠等待（spec §5.3，无失败路径）。

- [ ] **Step 1: 追加失败用例**：
  - `evict_writes_back`：写 `BCACHE_BUFS + 16` 个不同 LBA（触发整池逐出+脏写回）→ 绕过包装直读后端数组校验全部数据（`bcache_debug_count() == BCACHE_BUFS`，最早写的 LBA 已被逐出）；
  - `lru_preserves_hot`：写满池后重读最早写入的 sector 使其变热，再写 8 个新 LBA → 热项仍在缓存（包装读不触发后端读计数）。
- [ ] **Step 2: Run: `make test-host`** Expected: 2 用例 FAIL。
- [ ] **Step 3: 实现 LRU/逐出/写回/耗尽等待**（bcache.h 暴露 `#define BCACHE_BUFS 2048` 供测试引用）。
- [ ] **Step 4: Run: `make test-host`** Expected: 全绿（含前两 Task 全部用例）。
- [ ] **Step 5: Commit** `git commit -m "feat(block): LRU eviction with dirty write-back and exhaustion wait"`

### Task 7: 内核自测 test_bcache（双 CPU sector 原子性）

**Files:**
- Create: `kernel/selftest/test_bcache.c`
- Modify: `kernel/selftest/selftest.c:149+`（`selftest_register_builtin_tests` 登记）

**Interfaces:**
- Consumes: `bcache_*` 接口（内核侧）；`kernel_thread_on_cpu(fn, arg, cpu)`（`kernel/include/sched/task.h:297`）；`selftest_register(name, fn)`。
- Produces: 自测名 `"bcache_smp_coherence"`。测试自备 fake 盘：`static uint8_t backend[64 * 512]` + ops（memcpy 后端数组），经 `block_device_register` 注册为 `"bcachetest"`，测试退出反注册（`block_device_unregister_boot`）。

- [ ] **Step 1: 实现测试**（`test_fd_refcount.c` 的 harness 模式：jiffies 预算轮询、原子旗标握手、IRQ 使能说明照抄）：
  - `same_sector_atomic`：CPU0 循环写 LBA3 = PATTERN_A/PATTERN_B 交替（每字节同相位、两模式字节集不相交）；CPU1 循环读 LBA3，断言每次读到的 512 字节是完整 A 或完整 B（无混合）——阈值内零撕裂即 PASS；
  - `adjacent_sector_isolation`：CPU0 写 LBA10、CPU1 写 LBA11 交替不同模式各 N 次，结束后双 sector 读回各自最新模式完整；
  - `refcnt_no_leak`：测试收尾 `bcache_debug_count()` 断言 ≤ 预注册项数且全部 `refcnt == 0`（经 debug 钩子）。
- [ ] **Step 2: Run: `make KERNEL_SELFTEST=1 test-kernel-selftest`**（不加 OS01_SYSTEST）Expected: `bcache_smp_coherence PASS`，全套件绿。
- [ ] **Step 3: Commit** `git commit -m "test(kernel): SMP block-cache coherence selftest"`

### Task 8: Phase 2 全量回归

- [ ] **Step 1: Run: `make test-host && make test-static && make OS01_SYSTEST=1 test-qemu SUITE=systest`** Expected: 全绿（boot 路径经缓存后 GPT/ext2/FAT 挂载与运行时读写无回归）。
- [ ] **Step 2: 交互盘 headless 冒烟**（AGENTS.md「Headless interactive test」片段，`cat /dev/urandom` + Ctrl-C + `SHELL_ALIVE_123`）Expected: 成功签名出现。
- [ ] **Step 3: Commit**（如有零星修复）`git commit -m "fix(block): ARCH-6 phase-2 regression fixes"`

### Task 9: 用户态 SMP 压测 E2E（arch6-fsstress suite）

**Files:**
- Create: `user/test_arch6_fsstress/test_arch6_fsstress.c` + `user/test_arch6_fsstress/app.mk`
- Create: `config/inittab.arch6-fsstress`
- Modify: `mk/components/run.mk`（flavor / 镜像变量 / SUITE 校验 / recipe 四处，骨架照抄 `arch10-capacity` 块 :1418/:1437/:1460-1478/:1540-1551；本 flavor **无内核编译旗标**——压测纯用户态）
- Create: `qemutests/arch6_fsstress.py`（标记解析器，骨架照抄 `qemutests/arch10_capacity.py` 及其在 `run_test.py` 的发现方式）

**Interfaces:**
- Produces: 串口标记协议——逐项 `[arch6-fsstress] <fs> worker<N> PASS` / `FAIL(<detail>)`，最终 `[arch6-fsstress] PASS`；`<fs>` ∈ {`ext2`, `tmpfs`, `vfat`}。make 目标 `make test-qemu SUITE=arch6-fsstress`。

- [ ] **Step 1: 实现压测程序**。主进程对三个挂载点各建工作目录（`/arch6stress`、`/tmp/arch6stress`、`/boot/arch6stress`），`fork` 4 个 worker（对齐 `SMP=4`）；每个 worker 在每个挂载点：创建 `f<w>` → 写 32 段带 worker 签名的模式数据（累计校验和）→ 读回逐段校验 → `rename` 为 `f<w>.done` → 轮询等待相邻 worker 的 `.done` 文件出现并读回校验（跨 worker 可见性）→ `unlink` 双方文件 → 目录消失确认。任一断言失败打印 `FAIL(<detail>)` 并 `_exit(1)`；全部通过打印逐项 PASS + 最终 `[arch6-fsstress] PASS` 后 `_exit(0)`。
- [ ] **Step 2: inittab + run.mk + 解析器接线**（inittab：`tty1:once:/bin/test_arch6_fsstress`；SUITE 校验列表加入 `arch6-fsstress`；QEMU 用 `-smp 4`）。
- [ ] **Step 3: Run: `make test-qemu SUITE=arch6-fsstress`** Expected: 12 项 worker PASS + 最终 PASS，退出码 0；连跑 2 次无 flake。
- [ ] **Step 4: Commit** `git commit -m "test(e2e): SMP concurrent FS stress suite (arch6-fsstress)"`

### Task 10: 文档闭环 + 终验

**Files:**
- Modify: `docs/roadmap.md`（ARCH-6 移入已闭环清单，注明日期；剩余项改为仅 ARCH-8）
- Modify: `docs/changelog.md`（三期交付记录，含 commit 范围）
- Modify: `docs/fs/filesystem.md`（新增「FS 并发模型与块缓存」一节：锁序不变量、缓存一致性契约、flush/sync 语义、分区透传规则）
- Modify: `docs/build/build.md`（SUITE `arch6-fsstress` 登记）

- [ ] **Step 1: 四处文档更新**。
- [ ] **Step 2: 终验全量**：`make test-host && make test-static && make OS01_SYSTEST=1 test-qemu SUITE=systest && make test-qemu SUITE=arch6-fsstress && make KERNEL_SELFTEST=1 test-kernel-selftest` Expected: 全绿。
- [ ] **Step 3: Commit** `git commit -m "docs: close ARCH-6 (FS concurrency and block cache)"`
- [ ] **Step 4: 汇报**：向用户汇总三期交付与验证证据；合并回 master 由用户决定（遵循 AGENTS.md `git merge --ff` 优先策略）。

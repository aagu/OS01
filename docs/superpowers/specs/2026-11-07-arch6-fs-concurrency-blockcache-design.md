# ARCH-6 FS 并发与块缓存 — 设计规格

- 日期：2026-11-07
- 状态：待评审
- 对应 roadmap：`docs/roadmap.md` 的 ARCH-6「FS 并发与块缓存」
- 前置：ARCH-9（驱动模型/块设备 ops）✅、ARCH-5（VFS 抽象/active_nodes/dcache）✅、ARCH-10（动态 blockdev 容量）✅

---

## 1. 目标与范围

### 1.1 问题

roadmap 对 ARCH-6 的原始描述：

> `fat.c`、`tmpfs.c` 0 处加锁（`ext2.c` 19 处），默认 `-smp 2` 下 FAT 表/簇分配/tmpfs
> 目录存在数据竞争；全内核无 buffer/page cache，每次读直达 AHCI。
> 短期：每挂载点锁兜底 FAT/tmpfs；中期：blockdev 层按 (dev, blkno) 哈希的块缓存，
> ext2/FAT 共用，含写回与一致性；SMP 并发读写压力用例。

### 1.2 现状复核结论（关键，roadmap 部分描述已过时）

逐项复核 master（`58069ab3`）后的实际状态：

1. **「fat.c/tmpfs.c 0 处加锁」已过时**。三大 FS 均已有锁，但形态不对：
   - `ext2.c`：per-mount `spinlock_T fs->lock`（`kernel/include/fs/ext2.h:126`），
     覆盖全部 VFS ops 整个操作体（read/write/truncate/readdir/create/unlink/rename/…）。
   - `fat.c`：per-mount `spinlock_T fat_lifecycle_lock`（`kernel/include/fs/fat.h:113`），
     同样覆盖全部 ops 整个操作体，FAT 表/簇分配/目录变更都在锁内。
   - `tmpfs.c`：**全局静态单锁** `tmpfs_lock`（`tmpfs.c:61`）跨所有 tmpfs 挂载
     （当前仅 `/tmp` 一个挂载，但结构上非 per-mount）。
2. **共性缺陷：spinlock 持锁横跨 AHCI 忙等 DMA**。AHCI `ahci_port_read/write`
   以 `arch_nop()` 忙等轮询完成位（`kernel/driver/ahci.c:230-236`，从不睡眠、
   不用中断完成），而 ext2/FAT 在 spinlock 内直接调 `block_device_read/write`
   （如 `ext2.c:742`、`fat.c:830-848`）。后果：SMP 下一核持锁等 DMA、其余核
   自旋排队，整个挂载点完全串行且白烧 CPU。正确性（数据竞争）目前是兜住的，
   差的是并发性与锁语义。
3. **块层无缓存**：`block_device_read/write`（`kernel/block/blockdev.c:213/248/283`）
   是纯参数校验直通到 `dev->ops->read/write`；每次 FS 访问都 kmalloc 临时缓冲
   → AHCI DMA → kfree，同一扇区反复读盘。
4. **分区转发会回到父盘包装层**：`partition_read/write` 经
   `block_device_read(ctx->parent, …)` 递归进入父盘包装函数（`kernel/fs/gpt.c:32-67`），
   因此对块缓存而言，缓存 key 自然收敛到物理盘的 `block_device_t*`
   （详见 §5.2），无需额外归一化。
5. **`SYS_sync`（40）已是占位 stub**：`kernel/syscall/sys_misc.c:40` 注释明确
   "Future: flush AHCI/FAT buffers here"——本次直接接线。
6. **调度器支持的 mutex 已就绪**：`kernel/sync/mutex.c` 的 `mutex_t` CAS+等待队列+
   `schedule()`，进程上下文可安全持锁跨 I/O；另有 vfs bootstrap 门控先例
   （`vfs.c:31-52`，挂载发生在 pre-SMP 单核阶段，`vfs_ns_lock` 以
   `!vfs_bootstrap && current && current->pid > 0` 豁免）。
7. **全部挂载发生在单核 pre-SMP**（`fs_boot_mounts()`，`kernel/core/main.c:172`，
   先于 `x86_64_boot_aps()`），挂载路径当前不取 FS 大锁（`ext2_init` 不取），
   boot 期豁免锁与现状行为一致。

### 1.3 目标

1. **锁升级**：ext2/FAT/tmpfs 三处 FS 大锁从「spinlock 持锁跨 DMA」升级为
   「调度器支持的 per-mount `mutex_t`，进程上下文持锁」，tmpfs 锁从全局改
   per-mount；pre-scheduler boot 期沿用 vfs bootstrap 门控豁免（单核无竞争）。
2. **块缓存**：blockdev 层新增按（物理盘 `block_device_t*`，LBA）哈希的
   sector 粒度缓存，ext2/FAT/GPT/devfs blkdev 全部路径透明共用；写回
   （write-back）策略 + 显式 `block_device_flush()`；脏块逐出前写回。
3. **一致性闭环**：`SYS_sync` 接线为对所有已注册盘 `block_device_flush()`；
   缓存成为驻留块的唯一事实源（任何路径不再绕过缓存直接读写盘）。
4. **SMP 压力验收**：用户态 fork 压测程序覆盖 tmpfs/ext2/FAT 三挂载点的
   并发读写/创建/改名/删除校验；内核自测用 pinned kthread 在多 CPU 上
   压块缓存一致性与写回；hosttest 覆盖缓存单元行为。

### 1.4 非目标（明确不做）

- **不做 per-inode 细粒度锁**（i_rwsem 类）：仍维持 per-mount 大锁语义，
  只换锁类型与归属；细粒度并发属后续演进。
- **不做日志/崩溃一致性**（journaling、fsck）：写回只保证内核内读己之写
  与显式 flush 后的持久性；掉电窗口与 AHCI flush no-op（`ahci.c:63-67`）
  现状一致，不扩大承诺。
- **不做 page cache**（文件页粒度缓存）：只做 blockdev 层 sector 缓存。
- **不做异步 I/O / 请求队列**：AHCI 忙等模型保持不变；缓存读写仍是同步调用。
- **不改 UAPI / syscall 号**（`SYS_sync` 已存在，仅补实现）。
- **aarch64 不在本期范围**（其 EL1 协作式调度尚无用户态 FS 路径；mutex 为
  arch-neutral，后续 aarch64 M4 用户态落地后自然受益）。

---

## 2. 现状源码依据

| 事实 | 依据 |
|------|------|
| AHCI 读写为忙等轮询、从不睡眠 | `kernel/driver/ahci.c:230-236`（`arch_nop()` 轮询 `p->ci`）、`ahci.c:63-67`（flush no-op） |
| AHCI 端口互斥为 irqsave 自旋 + deadline 门限 | `kernel/driver/ahci_lifecycle.c:54-91`（`ahci_port_claim/release`） |
| ext2 per-mount spinlock 覆盖全部 ops | `kernel/include/fs/ext2.h:126`；`ext2.c:699/768/872/969/1040/1160/…` 取锁点；`ext2.c:1888` `spin_init` |
| FAT per-mount spinlock 覆盖全部 ops | `kernel/include/fs/fat.h:113`；`fat.c:830-848`（read）/`859-886`（write+簇分配）等 |
| tmpfs 全局静态锁 | `kernel/fs/tmpfs.c:61` `static spinlock_T tmpfs_lock`；全部 ops 取用 |
| 锁嵌套序恒为 fs 大锁 → `mount->active_lock` | `ext2.c:837-839/1183-1189`、`tmpfs.c:253-256/475-477` |
| 块层直通无缓存 | `kernel/block/blockdev.c:213-310`（read/write/flush 纯转发） |
| 分区经父盘包装转发 | `kernel/fs/gpt.c:42/58`（`block_device_read(ctx->parent, …)`） |
| mutex 为调度器支持睡眠锁 | `kernel/sync/mutex.c:13-44`（CAS + wait_queue + `schedule()`） |
| vfs bootstrap 门控先例 | `kernel/fs/vfs.c:31-52`（`vfs_bootstrap` 标志 + `pid > 0` 检查）、`sched_boot.c:153`（`vfs_end_bootstrap`） |
| 挂载全在 pre-SMP 单核 | `kernel/core/main.c:172`（`fs_boot_mounts`）先于 `main.c:196`（`x86_64_boot_aps`） |
| `SYS_sync` stub | `kernel/syscall/sys_misc.c:40-45` |
| slab 有 4096 级、4KB 页池可用 | `kernel/memory/slab.c:26-44`、`kernel/include/memory/pmm.h:93-94` |
| pinned kthread 压测先例 | `kernel/selftest/test_fd_refcount.c`（`kernel_thread_on_cpu`，`task.h:297-321`） |
| 用户态 fork 压测 E2E 先例 | `user/test_arch10_capacity/` + `config/inittab.arch10-capacity` + `run.mk:1418/1437/1460-1478` + `qemutests/arch10_capacity.py` |

---

## 3. 方案取舍

考虑过的三条路线：

- **A. 全范围分三期（采用）**：Phase 1 锁升级 → Phase 2 块缓存 → Phase 3
  SMP 压测与文档闭环。缓存与锁的交互（缓存元数据自旋锁 vs FS mutex、
  谁在外层）一次设计到位，roadmap 短期+中期目标一次闭环。
- **B. 只做锁升级 + 压测**：风险最小，但 roadmap 中期「块缓存」不闭环，
  ext2/FAT 每读直达 AHCI 的性能问题仍在，ARCH-6 无法从 roadmap 摘除。
- **C. 只做块缓存 + 压测**：缓存去重读路径，但 tmpfs 全局锁与
  spinlock-across-DMA 仍在，压测会暴露的公平性问题修不掉。

选 A。理由：roadmap 把两项同列 ARCH-6 完成条件；锁升级（mutex 化）是块缓存
压测能通过 SMP 验收的前提（spinlock 大锁下多核压测只是排队，测不出缓存
并发正确性）；三期共享同一条分支，缓存行锁与 FS 锁的嵌套关系一次定稿。

写回策略三选一：全程 write-through（一致性最简但每次写仍同步忙等 DMA，
缓存只加速读）、纯写回+定期写回（一致性边界最复杂、需要定时器基础设施）、
**写回+显式 flush（采用）**。脏块按 LRU 驻留；`block_device_flush(dev)`
同步全量写回；`SYS_sync` 对所有盘调 flush；逐出脏块前先写回。这是性能与
一致性的平衡点，也是 Linux buffer cache 的基础形态。

---

## 4. Phase 1 — 锁升级

### 4.1 锁类型与归属

| FS | 现状 | 目标 |
|----|------|------|
| ext2 | `spinlock_T lock` in `ext2_fs_t` | `mutex_t lock` in `ext2_fs_t`（per-mount） |
| FAT | `spinlock_T fat_lifecycle_lock` in `fat32_fs_t` | `mutex_t fat_lifecycle_lock`（per-mount，字段改名 `lock` 同步清理） |
| tmpfs | 全局 `static spinlock_T tmpfs_lock` | per-mount `mutex_t`，挂在新 `tmpfs_mount_ctx_t` |

- tmpfs per-mount 化：新增 `tmpfs_mount_ctx_t { mutex_t lock; tmpfs_node_t *root; … }`，
  `tmpfs_fs_mount()` 分配并放入 `mp->fs_data`（原 `fs_data` 直接存 root 节点
  的取用处同步改为 `ctx->root`）；`tmpfs_fs_unmount()` 释放。所有原取
  `tmpfs_lock` 的路径改为取 `mp` 对应 ctx 的锁。
- ext2/FAT：`spin_init` → `mutex_init`；各 ops 内 `spin_lock(&fs->lock)` →
  经由统一 helper 取锁（见 4.2）。`_locked` 后缀约定（`ext2_mark_orphan_locked`
  等）与「持锁调 I/O」的注释同步更新为 mutex 语义。

### 4.2 boot 期豁免（vfs bootstrap 门控）

挂载（pre-SMP）与 `current->pid == 0` 的极早期路径不能 `mutex_lock`（调度器
未启动，`schedule()` 即死）。复用 vfs 先例：

```c
/* kernel/fs/vfs.c 导出 */
bool vfs_sched_ready(void);   /* !vfs_bootstrap && current && current->pid > 0 */
```

各 FS 提供 `fs_lock_acquire/release` helper：`vfs_sched_ready()` 为真时
`mutex_lock/unlock`，否则 no-op（pre-SMP 单核无竞争，与 `vfs_ns_lock` 同一
豁免逻辑）。**规则：helper 只能包裹进程上下文的完整 VFS ops 操作体；锁内
代码不得关抢占或进入 IRQ 上下文**（现有 ops 全部满足——FS 调用只发生在
syscall/ELF loader/kthread 进程上下文）。

### 4.3 锁序不变量（全期有效）

```
fs 大锁(mutex) → mount->active_lock(spinlock) → bcache 元数据自旋锁（叶子）
```

- 缓存元数据锁是叶子锁：持有时绝不取 FS 锁或 active_lock；持 FS 锁调
  `block_device_read/write` 进入缓存路径取缓存锁，方向单一，无死锁环。
- 缓存缓冲的 BUSY 等待用 wait queue 睡眠（进程上下文），睡眠时只持缓存
  元数据锁的瞬时区，不持 FS 锁以外的自旋锁——持 FS mutex 睡眠等待缓存
  BUSY 是合法的（mutex 可睡眠）。
- AHCI `port->lock`（irqsave）保持最内层，不变。

### 4.4 验收

- 既有全部套件绿：`make test-host`、`make test-static`、
  `make OS01_SYSTEST=1 test-qemu SUITE=systest`、交互盘 headless boot
  （AGENTS.md 的 Ctrl-C 用例）。
- ext2/FAT 行为零变化（锁语义等价替换）；tmpfs 挂载点行为零变化。
- Phase 3 压测套件（§6）在本期即可预跑观察（不作为本期验收门槛）。

---

## 5. Phase 2 — 块缓存（bcache）

### 5.1 位置与文件

按目录组织规范成对落目录：

- 源：`kernel/block/bcache.c`
- 头：`kernel/include/block/bcache.h`
- 接入点：`kernel/block/blockdev.c` 的 `block_device_read/write` 包装函数内
  （对上层完全透明，ext2/FAT/GPT/devfs blkdev 零改动即接入）。

### 5.2 数据结构与 key

```c
typedef struct bcache_buf {
    struct bcache_buf *hash_next;      /* 桶内链 */
    struct bcache_buf *lru_next, *lru_prev;
    block_device_t    *dev;            /* key: 物理盘 dev（§5.2） */
    uint64_t           lba;
    uint32_t           flags;           /* VALID | DIRTY | BUSY */
    int                refcnt;
    spinlock_T         data_lock;       /* 短临界区：数据拷贝/改写（锁内无 I/O） */
    uint8_t           *data;            /* sector_size 字节，kmalloc */
} bcache_buf_t;
```

- **key 取包装层进入时的 `dev` 指针，且只对物理盘生效**：包装层入口检查
  `dev->parent != NULL`（BLOCK_PARTITION）时**不查缓存、不占缓存项**，直接
  转发 `dev->ops->read/write`——分区 ops 会递归进入父盘包装层，物理块在
  那一层以 `(物理盘 dev, 绝对 LBA)` 收敛为唯一 key。此规则保证：
  (a) 同一物理扇区不存在两个缓存项；(b) `SYS_sync` 枚举 `BLOCK_DISK` 即
  覆盖全部脏数据（分区 key 下永远没有脏项）。缓存接入的透传性仅对物理盘
  调用方成立；分区调用方多一层函数调用，语义不变。
- 粒度 = `dev->sector_size`（当前全链路 512 字节）；一次多 sector 请求在
  包装层按 sector 拆分逐个命中/缺失（4KB ext2 块 = 8 个缓存项）。
- 容量：编译期常量 `BCACHE_BUFS`（默认 2048 项 ≈ 1 MiB 数据 + 元数据，
  512 MiB 内存预算下安全）+ 哈希桶 `BCACHE_BUCKETS`（默认 512，链地址法），
  启动时 `kmalloc` 一次性分配（blockdev 注册前、slab 就绪后，pre-SMP 单核
  初始化无竞争）。

### 5.3 并发控制

- **全局元数据自旋锁 `bcache_lock`（irqsave）**：只保护哈希查找、LRU 摘挂、
  refcnt/flags 变更、脏链表——纯内存操作，临界区极短，锁内绝不做 I/O。
- **per-buf `refcnt`（pin）**：查找命中即 pin +1，释放元数据锁后再拷贝数据
  或发起 I/O；用毕解 pin。eviction 只考虑 `refcnt == 0 && !BUSY` 的 LRU 尾。
- **per-buf `data_lock`（自旋）——sector 原子性**：持有 pin 的命中读
  （拷出）与写（改写 + 置 DIRTY）都在 `data_lock` 内做 `memcpy`，保证
  同一 sector 的并发裸读/裸写（如 Phase 2 内核自测双 CPU 直压场景，上方
  无 FS mutex）观察到的数据是该 sector 一次完整写的结果（旧值或新值，
  不撕裂）。`data_lock` 锁内无 I/O、无内存分配，临界区为 512B memcpy 量级。
- **per-buf `BUSY` 标志 + 全局 `bcache_wq` 等待队列**：读缺失填充/写回/
  逐出的 AHCI I/O 期间置 BUSY（此时该 buf 不接受新 pin，等待者睡眠）；
  完成方 `wait_queue_wake_all`。睡眠只发生在持 FS mutex 的进程上下文，
  合法（§4.3）。
- **缓存槽耗尽**：无可逐出槽（全部 `refcnt > 0` 或 BUSY）时在 `bcache_wq`
  上睡眠等待槽位释放，无失败路径。进度保证：pin 只在单次块层调用内持有、
  解 pin 不依赖任何 FS mutex，故持 FS mutex 睡眠等待槽位不会死锁。
  （主机自测对 reset 钩子有专门的「丢弃全部缓存项」契约，见 §5.5。）
- 中断上下文永不进缓存路径（现状成立：无任何 IRQ 路径调 block I/O）。

### 5.4 读路径

`block_device_read(dev, lba, count, buf)`（物理盘 key，§5.2）逐 sector：
1. 查哈希：命中 VALID → pin → `data_lock` 内拷出 → 解 pin（内存速度）。
2. 缺失：从 free list / LRU 尾取槽（无可逐出则睡眠等待，§5.3；脏则先写回，
   见 5.5）→ 置 BUSY → `dev->ops->read(dev, lba, 1, buf->data)`（原始 ops，
   绕过包装防递归）→ 锁内置 VALID、挂哈希/LRU、清 BUSY → 唤醒等待者 →
   pin → 拷出 → 解 pin。
3. 同 sector 并发缺失：后到者看到 BUSY 即睡眠，由完成方唤醒重查哈希，
   不重复发起 I/O。

### 5.5 写路径与一致性

- `block_device_write(dev, lba, count, buf)`（物理盘 key）逐 sector：取
  缓冲（命中或分配，同上）→ pin → `data_lock` 内改写数据 → 置
  `VALID|DIRTY` → 解 pin → 挂脏管理 → **不立即下盘**。读己之写由缓存保证
  （缓存是驻留块唯一事实源）。
- **唯一性不变量**：缓存接入后，任何模块不得绕过包装层直接调
  `dev->ops->read/write` 访问已挂盘设备（缓存写回本身除外）。现状审计：
  ext2/FAT/GPT/devfs blkdev 均经包装层；AHCI 自身不发起 FS 语义 I/O。
- `block_device_flush(dev)`：遍历该盘 key 下的脏缓冲，**跳过
  `refcnt > 0` 或 BUSY 的在途缓冲（保持其 DIRTY，留给下一次 flush）**，
  其余逐个 `ops->write` 下盘并清 DIRTY。契约：flush 返回时，所有在
  flush 开始前已完成的 write() 的数据均已下盘；与 flush 并发在途的写
  属 best-effort（Linux buffer cache 同款语义）。测试只在静默状态断言
  flush 完整性。
- `SYS_sync` 改为对 registry 内全部 `BLOCK_DISK` 调 `block_device_flush`
  （`sys_misc.c` stub 接线；分区设备无脏项，无需枚举）。
- 逐出脏块：先写回再复用槽位（write-behind on evict）。
- `block_device_flush` 语义向下透传：分区设备 flush → 父盘 flush（现状
  `partition_flush` 已如此，保持）。
- **设备退役/测试 reset 契约**：`block_device_unregister_boot`（生产仅
  pre-mount 回滚路径）与 `block_device_reset_for_test`（hosttest）
  对该设备的缓存项**丢弃而不写回**（设备已退役，写回无意义且 ops 已
  置空），并从哈希/LRU/脏链全部摘除；hosttest 据此在用例间安全重置。

### 5.6 测试（随 Phase 2 落地）

- **hosttest `hosttests/cases/test_bcache.c`**：注册 fake block device
  （沿用 `test_block_device_ops.c` 模式 + `OS01_HOST_TEST` reset 钩子）：
  读缺失填充、写后读命中不落盘（fake 设备读到旧值、包装层读到新值）、
  flush 后 fake 设备读到新值、在途缓冲被 flush 跳过且保持 DIRTY、
  LRU 逐出与脏块写回、容量耗尽阻塞后恢复、reset 丢弃契约（用例间重置）。
- **内核自测 `kernel/selftest/test_bcache.c`**：pinned kthread 双 CPU 对
  同一测试盘的相同 sector 并发读/写（断言 sector 原子性：读到的必是某次
  完整写入的模式）与相邻 sector 并发（断言无串扰）+ flush + 校验
  （`test_fd_refcount` 模式），结束断言全部 buf refcnt 归零无泄漏。

---

## 6. Phase 3 — SMP 压测 E2E 与文档闭环

### 6.1 用户态压测（arch10-capacity 模式）

- `user/test_arch6_fsstress/`：fork N 组 worker（`SMP` 核数对齐，≥4），
  每组对三个挂载点各跑一轮：**创建 → 写入带校验和的模式数据 → 读回校验 →
  改名 → 跨 worker 交换文件校验可见性 → 删除**；文件名含 worker 编号避免
  同名冲突，另有共享目录上的并发创建/删除竞争窗口。输出
  `[arch6-fsstress] <fs> worker<N> PASS/FAIL(<detail>)` 逐项标记 +
  最终 `[arch6-fsstress] PASS`。
- `config/inittab.arch6-fsstress`、`run.mk` 的
  `TEST_QEMU_FLAVOR_arch6-fsstress` / 镜像变量 / SUITE 校验 / recipe、
  `qemutests/arch6_fsstress.py` 标记解析器，全部照抄 arch10-capacity 骨架；
  QEMU 以 `-smp 4` 跑（`KERNEL_SELFTEST_SMP` 同源的 `SMP` 变量）。
- **scenario-catalog 契约**：`register_host_test`（如新增）必须同步
  `test-scenarios.json` 条目（CI 首个未映射项即停）。

### 6.2 文档闭环

- `docs/roadmap.md`：ARCH-6 移入已闭环清单（含日期与 merge 说明）。
- `docs/changelog.md`：三期交付记录。
- `docs/fs/filesystem.md`：补「FS 并发模型与块缓存」一节（锁序不变量、
  缓存一致性契约、flush 语义）。
- `docs/build/build.md`：新增 SUITE `arch6-fsstress` 登记。

---

## 7. 风险与对策

| 风险 | 对策 |
|------|------|
| mutex 化后某条 ops 路径实际运行于非进程上下文 → `schedule()` 死机 | 现状审计 FS 调用只出现在 syscall/ELF loader/kthread；`vfs_sched_ready()` 门控兜底 boot 期；压测 + selftest 全套回归 |
| 递归取锁死锁（mutex owner 按 pid 记账，非重入） | 现有 spinlock 同样非重入，所有 ops 已保证单次取锁；helper 落地时逐函数核对取锁点无嵌套 |
| 写回缓存掩盖数据错误（读到的永远是缓存） | hosttest 用 fake 设备读旧值/新值双视角验证；内核自测 flush 后直读 fake 设备校验 |
| 缓存击穿（同 sector 并发缺失重复 I/O） | BUSY 标志 + 等待队列串行化同 sector 填充 |
| 512B 拆分引入的跨 sector 请求性能回退 | 多 sector 请求在包装层顺序逐 sector 处理，命中路径纯内存拷贝；压测对比不劣化（验收观察项，非硬门槛） |
| CI 计时型 flake | 压测断言只看校验和与标记，不设 wall-time 门槛；jiffies 预算沿用 fd_refcount 先例 |

---

## 8. 交付边界留给实施计划的决策

- 各 ops 取锁 helper 的具体落点与命名（Phase 1 内自行裁定）。
- `bcache` 与 `blockdev.c` 之间的内部接口形态（静态链接直调即可，无需 ops）。
- 压测 worker 数、循环次数与 jiffies 预算的具体取值（以 CI 稳定为先）。

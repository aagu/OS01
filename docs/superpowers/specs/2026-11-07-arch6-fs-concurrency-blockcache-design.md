# ARCH-6 FS 并发与块缓存 — 设计规格

- 日期：2026-11-07
- 状态：独立子 agent 复审通过（2026-11-07，第 2 轮 `approve`；仅设计文档，尚未实施）
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
- **不做日志/崩溃一致性**（journaling、fsck）：保证内核内读己之写，以及
  显式 flush 成功后目标数据已由后端 `ops->write` 成功接收。AHCI flush
  目前为 no-op（`ahci.c:63-67`），因此不承诺设备易失缓存已经提交到稳定介质。
  软件写回会扩大未 sync 时的掉电丢失窗口；设备级 flush 命令不在本期范围。
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
同步完成调用开始时的脏 key 集合；`SYS_sync` 对所有盘调 flush；逐出脏块前
先成功写回。并发写入的边界和错误返回由 §5.5 定义。

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
FS mutex → mount->active_lock（仅 FS 节点簿记）
FS mutex → bcache_lock（缓存元数据）
FS mutex → data_lock（单 sector 拷贝）
```

- `active_lock` 临界区不得调用块 I/O、等待缓存或取 `data_lock`。缓存元数据锁
  是叶子锁；不与 `data_lock`、等待队列锁或 AHCI port lock 嵌套。
- `data_lock` 只包围 kernel buffer 的拷贝，使用 irqsave/irqrestore；释放后
  才取元数据锁提交 flags。pin 贯穿两段临界区，防止中间被复用。
- 等待和 I/O 时不持任何自旋锁；可持外层 FS mutex。裸 blockdev 调用和
  自测无需 FS mutex，但必须处于调度器就绪的非 idle 进程上下文；启动期
  单核路径没有并发 BUSY/pin 等待。AHCI 自行管理 port lock，缓存锁不跨 DMA。

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
    uint8_t           *data;            /* 本期支持 512 字节 sector */
} bcache_buf_t;
```

- **key 取包装层进入时的 `dev` 指针，且只对物理盘生效**：包装层入口检查
  `dev->parent != NULL`（BLOCK_PARTITION）时**不查缓存、不占缓存项**，直接
  转发 `dev->ops->read/write`——分区 ops 会递归进入父盘包装层，物理块在
  那一层以 `(物理盘 dev, 绝对 LBA)` 收敛为唯一 key。此规则保证：
  (a) 同一物理扇区不存在两个缓存项；(b) `SYS_sync` 枚举 `BLOCK_DISK` 即
  覆盖全部脏数据（分区 key 下永远没有脏项）。缓存接入的透传性仅对物理盘
  调用方成立；分区调用方多一层函数调用，语义不变。
- 粒度 = 512 字节（当前全链路如此）；本期缓存对 `sector_size != 512` 返回
  `-EINVAL`，不得用固定大小数据区接收更大的 sector。一次多 sector 请求在
  包装层按 sector 拆分（4KB ext2 块 = 8 项）；请求可能部分完成后遇错，返回
  首个错误，已经完成的 sector 不回滚，保留既有块接口的错误返回形式。
- 容量：编译期常量 `BCACHE_BUFS`（默认 2048 项 ≈ 1 MiB 数据 + 元数据，
  512 MiB 内存预算下安全）+ 哈希桶 `BCACHE_BUCKETS`（默认 512，链地址法），
  在 slab 就绪、首次 blockdev 注册前初始化。元数据池与恰好 1 MiB 的数据池
  分开 `kmalloc`（单次分配上限 1 MiB）；不在元数据锁内分配。初始化失败
  清理已分配部分并向注册/块操作返回 `-ENOMEM`，不得发布半初始化状态；
  `bcache_init()` 成功后幂等，host reset 保留池但清空项，允许失败后重试。

### 5.3 并发控制

- **元数据锁**保护哈希、LRU、refcnt 和全部 flags；无独立脏链表，flush
  在锁内扫描固定槽池生成 key 快照，避免数据修改与脏链发布分离。
- **pin 生命周期**：命中时在元数据锁内 pin；新项在发布 BUSY key 时
  `refcnt = 1`。调用方完成数据拷贝和 flags 提交后才解 pin。读填充清 BUSY
  时保留该 pin；逐出只选 `refcnt == 0 && !BUSY` 的 LRU 项。
- **sector 原子性**：命中读/写在 `data_lock` 的 irqsave 临界区内做完整
  512B memcpy；写在释放 data_lock 后、仍持 pin 时，在元数据锁内提交
  `VALID|DIRTY`（按位更新，保留其他任务设置的 BUSY），最后解 pin。
- **BUSY 是独占维护预留**：填充、逐出及 flush 使用；所有查找先检查 BUSY，
  再检查 VALID。BUSY key 必须已经可被哈希查到；同 key 后到者不得另立项。
  flush 可在现有 pin 尚未归零时先置 BUSY，阻止新 pin，再等待旧持有者
  完成；旧持有者提交 flags/解 pin 不等待 BUSY、不取 FS 锁。
- **等待握手**：发现 BUSY、无可逐出槽，或维护方等待旧 pin 归零时，先
  释放元数据锁；本地 IRQ 保持关闭，`wait_queue_arm` 后重新取元数据锁
  重查条件（能前进时须在本次锁内预留资源），释放锁后才决定 `schedule()`。
  返回后在等待队列锁内 `wait_queue_disarm`，再恢复原 IRQ 状态。元数据锁
  与队列锁不嵌套；唤醒可以先于 schedule，不能覆盖已恢复的 TASK_RUNNING。
- **唤醒事件**：refcnt 降到零、BUSY 清除、失败释放槽、测试释放 pin 后，
  均在释放元数据锁后 `wait_queue_wake_all`。等待是不可中断的内部重试；
  被信号唤醒也必须清理队列节点，再重查，不能遗留 pin/BUSY。
- **耗尽与进度**：没有可用槽时等待上述事件，不把暂时占满当作 ENOMEM。
  每次 sector 操作至多持有一个普通 pin，释放它不依赖等待者持有的 FS 锁；
  后端写回失败则返回该错误，不能因坏盘永远等待。中断上下文不得调用缓存。

### 5.4 读路径

`block_device_read(dev, lba, count, buf)`（物理盘 key，§5.2）逐 sector：
1. 锁内查哈希：BUSY 按 §5.3 等待；否则 VALID 命中即 pin、更新 LRU，
   解锁后在 data_lock 内拷出，完成后解 pin。
2. 缺失：锁内取 free/可逐出槽（脏逐出见 §5.5），在同一临界区查重并发布
   `(dev, lba, BUSY, refcnt=1)`；解锁后原始 `ops->read(dev,lba,1,data)`。
3. 成功：锁内置 VALID、清 BUSY，保留调用方 pin；解锁唤醒后拷出，再解 pin。
   失败：不置 VALID，撤销哈希项，归还槽并释放 pin/BUSY，解锁唤醒并返回错误。
   同 sector 等待者重查后可重试，不把失败的内容作为命中。

### 5.5 写路径与一致性

- 整 sector 写命中时 pin，新项按 §5.4 发布 BUSY key + pin，但无需先读盘。
  在 data_lock 内复制，随后锁内提交 VALID|DIRTY，新项同时清 BUSY，最后
  解 pin 并唤醒。缓存是驻留块的事实源，写成功仅表示软件缓存已接收。
- **唯一性不变量**：缓存接入后，任何模块不得绕过包装层直接调
  `dev->ops->read/write` 访问已挂盘设备（缓存写回本身除外）。现状审计：
  ext2/FAT/GPT/devfs blkdev 均经包装层；AHCI 自身不发起 FS 语义 I/O。
- **flush 边界**：锁外分配最多 BCACHE_BUFS 个 `(dev,lba)` 的目标数组，
  再在元数据锁内快照该盘全部 DIRTY key（含 BUSY/pinned 项）；快照时刻是
  本次 flush 的线性化边界。此前已完成的写，其值或同 sector 后续覆盖版本
  必须在成功返回前被后端成功接收；快照后新增的脏 key 不要求纳入本次。
- **逐目标处理**：每次按 key 重新查哈希，不能保存可复用槽的裸指针。
  key 已不存在或已干净表示先前已有成功写回；BUSY 则等待后重查；否则锁内
  置 BUSY，阻止新 pin，按 §5.3 等旧 pin 归零，再锁外 `ops->write`。
  成功才清 DIRTY，失败保留 key/VALID/DIRTY；两种情况均清 BUSY、唤醒。
  多个 flush 依靠同一 BUSY 协议串行访问各 key，不需要额外全局睡眠锁。
- **包装层与错误**：物理盘 `block_device_flush` 必须先调用缓存 flush，
  成功后才调用可选 `ops->flush`；没有驱动 flush 回调也不能跳过软件写回。
  缓存分配失败返回 ENOMEM；写回失败或驱动 flush 失败返回原错误并更新
  last_error。`SYS_sync` 遍历所有 BLOCK_DISK，失败后继续其他盘，最后返回
  首个负错误或 0。分区 flush 继续经 partition_flush 转发父盘包装层。
- **逐出**：锁内选 refcnt=0 且非 BUSY 的 LRU 项并置 BUSY；脏项锁外
  写回，失败保留旧 key/data/DIRTY，清 BUSY 并返回错误。成功后才移除旧 key。
  脏逐出释放锁期间，新请求 key 可能已被其他 CPU 发布，因此复用前必须
  锁内重新查重：已有项则归还所选槽并重试，否则原子发布新 BUSY key + pin。
- **错误回收规则**：所有填充、写回、逐出失败路径必须释放自己拥有的
  pin/BUSY，保留尚未成功写回的数据，并唤醒等待者；失败不能伪装成干净命中。
- **设备退役/测试 reset 契约**：`block_device_unregister_boot`（生产仅
  pre-mount 回滚路径）与 `block_device_reset_for_test`（hosttest）
  对该设备的缓存项**丢弃而不写回**（设备已退役，写回无意义且 ops 已
  置空），并从哈希/LRU 全部摘除；hosttest 据此在用例间安全重置。
  drop/reset 只允许无在途访问的静默设备/测试环境；必须在释放设备对象前
  丢弃项，不能用全局 reset 清理仍在使用的启动盘。

### 5.6 测试（随 Phase 2 落地）

- **hosttest `test_bcache`**：fake 内存盘双视角验证读填充、分区/父盘同 key、
  写后读、flush 写回、LRU、静默 drop/reset；逐出测试先只检查被逐出项，
  全量后端检查必须在显式 flush 后。故障注入覆盖初始化 OOM、读失败重试、
  flush/逐出失败保脏以及驱动 flush 错误返回。
- 宿主增加可脚本化的调度器/IRQ 环境，链接生产 wait.c（参考
  test_mutex_handshake）：在 ARM 前/后及 schedule 前注入完成事件，验证
  BUSY/耗尽/flush-drain 不丢唤醒。仅 OS01_HOST_TEST 提供按 key 的 BUSY
  reserve/complete 钩子，复用真实发布、成功/失败收尾和唤醒 helper；reserve
  仅允许缺失 key，持有专属测试 pin，complete 只接受自己拥有的预留并释放
  该 pin。脚本用 complete 推进状态，禁止在外层 fake read 栈内嵌套同 key
  读取并期待外层自行完成；单线程脚本只验证等待握手，不冒充真实 SMP 证据。
- **内核 late 自测 `bcache_smp_coherence`**：调度器就绪后运行，两个 pinned
  kthread 以握手证明重叠：先 seed 合法模式，再压同 sector 原子性、相邻
  sector 隔离、并发首次缺失只读一次，以及 flush 等 pinned 读取完成。
- **确定性耗尽自测**：fake 盘至少 BCACHE_BUFS+1 个 sector；先通过测试钩子
  pin 满全局槽池，再让另一非 idle worker 请求新 sector，确认进入等待后
  释放一个 pin，断言唤醒完成。无调度器的 idle harness 只协调和 yield，
  不亲自执行可能睡眠的缓存操作；异常/超时必须让 worker 退出并释放测试 pin。
- 调试接口按 dev 返回 entries/pinned/busy/dirty/waiters；收尾检查测试盘
  pinned=busy=waiters=0，drop 后 entries=0，不限制真实启动盘的全局占用数。

---

## 6. Phase 3 — SMP 压测 E2E 与文档闭环

### 6.1 用户态压测（arch10-capacity 模式）

- `user/test_arch6_fsstress/`：固定 fork 4 个 worker，QEMU 使用 4 个 CPU，
  每组对三个挂载点各跑一轮：**创建 → 写入带校验和的模式数据 → 读回校验 →
  改名 → 跨 worker 交换文件校验可见性 → 删除**；文件名含 worker 编号避免
  同名冲突，另有共享目录上的并发创建/删除竞争窗口。输出
  `[arch6-fsstress] <fs> worker<N> PASS/FAIL(<detail>)` 逐项标记 +
  最终 `[arch6-fsstress] PASS`。
- 主进程通过管道协调阶段：全员完成 rename 后才交换读取；全员完成跨
  worker 校验后才各自删除自己的文件；全员删完后由主进程检查目录为空。
  此处交换文件放在每个测试根目录的独立 `files/` 中，空目录断言只检查
  `files/`；预建的 `shared/` 与它并列。shared 阶段每个 worker 只创建/删除
  自己的唯一文件，分别设创建前、创建完成、删除完成屏障，再检查 shared/
  为空。父进程最后依次删除 files/、shared/、测试根目录。屏障协调不依赖
  被测 FS；任一 worker 失败，主进程终止并回收其余 worker，禁止无限轮询
  或由单个 child 发出最终 PASS。
- `config/inittab.arch6-fsstress`、`run.mk` 的
  `TEST_QEMU_FLAVOR_arch6-fsstress` / 镜像变量 / SUITE 校验 / recipe、
  `qemutests/arch6_fsstress.py` 标记解析器，全部照抄 arch10-capacity 骨架；
  验收命令 `make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress`。
  suite 将有效 SMP 显式传到 TestRunner，拒绝非 4 或与 QEMU_SMP 冲突的设置，
  解析器核验四核启动证据、12 个唯一 worker PASS、无 FAIL 和父进程最终 PASS。
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
| 写回缓存掩盖数据错误（读到的永远是缓存） | fake 后端双视角、显式 flush 后校验；不把后端可见性称作掉电持久性 |
| 缓存击穿或提前复用 | 锁内先发布 BUSY key + 调用方 pin；完成数据/flags 操作后才解 pin |
| 丢唤醒/耗尽挂死 | ARM→重查握手、解 pin/清 BUSY 唤醒；脚本化窗口测试 + pin 满池的真实 SMP 用例 |
| 后端错误丢脏数据 | 错误状态机保留旧 key/data/DIRTY，返回错误并唤醒；故障注入验收 |
| 512B 拆分引入的跨 sector 请求性能回退 | 多 sector 请求在包装层顺序逐 sector 处理，命中路径纯内存拷贝；压测对比不劣化（验收观察项，非硬门槛） |
| CI 计时型 flake | 压测断言只看校验和与标记，不设 wall-time 门槛；jiffies 预算沿用 fd_refcount 先例 |

---

## 8. 交付边界留给实施计划的决策

- 各 ops 取锁 helper 的具体落点与命名（Phase 1 内自行裁定）。
- `bcache` 与 `blockdev.c` 之间的内部接口形态（静态链接直调即可，无需 ops）。
- 压测循环次数与 jiffies 预算（以 CI 稳定为先）；worker/CPU 数固定为 4。

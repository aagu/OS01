# ARCH-6 FS 并发与块缓存 — 实施 Handoff（2026-11-07 暂停点）

- 分支：`feature/arch6-fs-concurrency-blockcache`（worktree `.worktrees/arch6-fs-concurrency-blockcache`）
- 计划：`docs/superpowers/plans/2026-11-07-arch6-fs-concurrency-blockcache.md`（10 Task）
- 规格：`docs/superpowers/specs/2026-11-07-arch6-fs-concurrency-blockcache-design.md`（已批准，含第 2 轮复审修订）
- SDD 台账：`.superpowers/sdd/2026-11-07-arch6-fs-concurrency-blockcache/progress.md`
- 执行方式：inline（superpowers:executing-plans），Task 1–6 已完成并提交

## 1. 已完成（全部有 commit + 全套件绿证据）

| Task | 内容 | commit |
|------|------|--------|
| 1 | `vfs_sched_ready()` + `vfs_test_set_sched_ready()` 门控；ext2 per-mount spinlock → 调度器 mutex；hosttest owner 断言 | `5e233fe1` |
| 2 | FAT 同款 mutex 化（`fat_lifecycle_lock` → `lock`） | `1e8c6fef` |
| 3 | tmpfs per-mount `tmpfs_mount_ctx_t`（mutex + root）；systest 全绿 | `54426769` |
| 4 | bcache 读核心（哈希/LRU/pin/BUSY/ARM→重查握手）+ 宿主确定性等待环境（`arch6_bcache_runtime` mock + wrap `wait_queue_arm/sleep`） | `aa779c33` |
| 5 | 读写双向分流（仅物理盘 key）+ 写回 + `bcache_flush_device`（DIRTY key 快照 + BUSY drain）+ `SYS_sync` 接线 | `50e904da` |
| 6 | LRU 逐出（脏写回可恢复：失败保 key/data/DIRTY）+ 耗尽等待 | `29b96793` |

宿主侧 `test_bcache` 23/23 全绿；`make test-host`/`test-static`/`systest` 全绿。

## 2. 当前卡点：Task 7 内核 late SMP 自测（`kernel/selftest/test_bcache.c`）

### 2.1 失败症状（最新一次运行）

| 用例 | 症状 |
|------|------|
| same_sector_atomic | **PASS**（2000 次写、~1900+ 次读零撕裂） |
| same_sector_concurrent_miss | PASS（backend_reads=1、waiter=1） |
| adjacent_sector_isolation | 读回=后端旧值（0xa7）——**已知测试 bug**：两个 worker 共享 `bc_adj_buf`，需改成 per-worker 缓冲 |
| flush_waits_for_reader | pin 不持久：worker 内 `debug_pin` 后采样 `pinned=1`，harness 数微秒后采样 `pinned=0`；`debug_pin` 的读**未命中刚写入的 key 20**（走了 miss 填充） |
| exhaustion_waits | 2048 次 `debug_pin` 后 `entries≈6`——pin 不累积 |
| flush_backend_visibility | backend[20] ≠ 0x74 —— **脏数据丢失**（写回缓存后条目消失、从未写回） |

### 2.2 核心谜团（未解）

**bcachetest 的缓存条目会在同一线程两次相邻操作之间消失**：
`block_device_write(20)`（entry VALID|DIRTY 入池，post-write 采样 `entries=5 dirty=2`）
→ 紧接着 `debug_pin(20)` 的读 **miss**（重新从后端填充，脏数据丢失、backend[20] 永远拿不到 0x74）。
消失发生时不触发任何移除路径探针（hash_remove / publish_transform / return_slot / fill_failed / reset 均无打印），且 `LOOKUP-MISS-but-present` 探针为 0（查找 miss 时池里确实没有该条目）→ **条目是真的没了，且没有走任何已知的移除代码**。

### 2.3 已排除（探针证据）

- 哈希损坏：审计（chain/pool/dup/cycles/stale）始终干净；DOUBLE-INSERT 绊线 0 次；自环 0。
- `bcache_reset_for_test` 被调用：探针 0 次（内核侧 grep 亦无调用者）。
- `bcache_init` 重入：`ready=1` 幂等返回，池未重建。
- spinlock 约定：内核 `spinlock_T` 为 1=unlock；`g_bcache_lock` 在 `bcache_init` 中 `spin_init`，mutex/mutex_runtime mock 的 0=free 约定只影响宿主侧（已适配）。
- lookup 命中逻辑：`LOOKUP-MISS-but-present` 0 次 → miss 时条目确实不在池中。

### 2.4 下一步排查建议（按优先级）

1. **kthread 栈溢出假设（最可疑）**：`bcache_debug_pin` 有 `-Wframe-larger-than=512` 警告（帧 552B）；worker 调用链深（thread entry → worker → wrapper → bcache → alloc/fill）。确认 `STACK_SIZE`（`get_current_task()` 用 `RSP & ~(STACK_SIZE-1)`）是多少；若 4KB，pinned kthread 的深链 + serial_printk 变参帧可能溢出栈、踩到相邻任务栈/全局区，表现恰好是"元数据静默变质、无任何代码路径命中"。可在 worker 入口打印 `&local` 与栈边界，或在 bcache 全局前后放 canary。
2. 把消失窗口进一步二分：post-write 采样之后、`debug_pin` 之前**只有** `serial_printk`——尝试去掉该 print 再看是否仍消失（排查 printk 路径的意外副作用，如串口驱动内的 log 缓冲触碰）。
3. 修 adjacent 共享缓冲 bug 后单独重跑，排除其干扰。
4. 若栈溢出证实：把 `bcache_debug_pin` 的 `tmp[512]` 改 static/堆，或给 selftest worker 换更大栈；同时评估生产路径 `bcache_read_sector` 调用链的栈预算（FS 大锁下这一链路将来会被 ext2/FAT 走到）。

### 2.5 探针清理清单（全部为临时调试代码，收尾前必须移除）

`kernel/block/bcache.c`：`bc_audit_dev`/`g_lookup_probe_quiet` 全部 `serial_printk` 探针（INIT call、hash_remove、publish_transform、return_slot、fill_failed、unpin、copy_out_unpin、write_hit_unpin、debug_unpin、RESET-FOR-TEST、LOOKUP-MISS、DOUBLE-INSERT、hash 审计块、`g_publish_site`）。
`kernel/selftest/test_bcache.c`：post-write/worker/stats#N/adj-stats 采样打印。
注意：`bcache_test_stats_t`/`bcache_debug_stats`/`bcache_debug_free_slots`/`bcache_test_drop_key`/`bcache_debug_pin/unpin`/`reserve/complete_busy` 是**正式测试接口**（spec §5.6/计划定义），保留。

### 2.6 重大教训：陈旧内核二进制陷阱

调试中段所有"探针无效"的假象，根源是 **内核构建早已失败但测试套件反复跑旧镜像**：
`bc_audit_dev` 先使用后声明 → `make image` 编译错误（`Error 1` 深藏在日志尾部）→ `test-kernel-selftest` 继续用 13:50 的旧 kernel.bin 运行。
**规则：每次改内核代码后运行 QEMU 套件前，必须 (a) `grep -E "error" <make 日志>` 确认零编译错误；(b) 校验探针字符串真的在
`build/x86_64-clang/artifacts/kernel/selftest/kernel.bin`（真实产物路径；`build/x86_64-clang/kernel/` 下只有 .d，没有 .o——内核对象是编译后即清理的临时物）。**

## 3. 剩余工作

- [ ] Task 7：解谜（§2.4）→ 移除全部探针（§2.5）→ 修 adjacent 共享缓冲 → `make KERNEL_SELFTEST=1 test-kernel-selftest` 全绿（协议含 9 个 late case、单一 END）→ commit
- [ ] Task 8：`test-host`/`test-static`/`systest` 全量回归 + headless 交互盘冒烟（AGENTS.md 片段）
- [ ] Task 9：`arch6-fsstress` E2E suite（解析器单测先行、SMP=4 强制、rootfs/project.mk/run.mk 接线——计划 Task 9 有逐步说明）
- [ ] Task 10：roadmap/changelog/filesystem.md/build.md 闭环 + 终验
- [ ] 整分支终审（fresh reviewer，executing-plans 收尾流程）→ `finishing-a-development-branch`

## 4. 台账裁决摘录（详见 progress.md）

- T1：TDD 顺序偏离简报（先改 owner 断言看真红）；worktree 需 `git -c protocol.file.allow=always submodule update --init`。
- T3：跨挂载 rename 被 VFS `-EXDEV`（vfs.c:2200）排除 → tmpfs 单 ctx 锁足够。
- T4：mock spinlock 0=free；`BCACHE_HOST_CFLAGS` 不能带 FRAMEWORK_INC（test_platform.h 冲突）；post-arm 重查视 key 消失为"无未来生产者"（disarm+重试，绝不 schedule）。
- T5：test_block_device_ops 自带 partition ops 补 flush 转发；flush 逐出序为槽序非 LBA 序（断言写成 order-agnostic）。
- T6：`pick_candidate` 沿 LRU 从尾跳过 pinned/BUSY（只看尾会饿死）；`lru_remove` 需 `BC_LLINKED` 守卫（自由槽曾清空 head/tail）；新增 `bcache_test_drop_key` 宿主钩子。

# ARCH-6 — Task 9 交接（2026-11-07 第二次暂停点）

- 分支：`feature/arch6-fs-concurrency-blockcache`，worktree `.worktrees/arch6-fs-concurrency-blockcache`
- 前一份 handoff：`docs/superpowers/reports/2026-11-07-arch6-blockcache-handoff.md`（Task 7 暂停点，已被 codex 闭环）
- 计划/规格/台账路径不变；台账：`.superpowers/sdd/2026-11-07-arch6-fs-concurrency-blockcache/progress.md`

## 1. 提交状态

| Task | 状态 | commit |
|------|------|--------|
| 1–6 | ✅ 已提交 | `5e233fe1`..`29b96793` |
| 7 | ✅ codex 闭环（根因：idle harness 与 runnable worker 互等；43/43 ×3 + 双核 + abort 注入） | `83d37709` |
| 8 | ✅ 本轮完成：host/static/systest 全绿；coordinator 契约 8→9 个 late case；正常盘冒烟判定为**基线即有的宿主问题**（base A/B 证据在台账 task8-evidence/） | `bfe97914` |
| 9 | 🔶 **进行中**：套件全部落码、单测 13/13 绿、E2E 跑通协议但被一个**预先存在的 VFS rename 可见性竞态**阻塞（详见 §3） | 未提交 |
| 10 + 终审 | ⬜ 未开始 | — |

## 2. Task 9 已落码的内容（未提交，工作树中）

- `user/test_arch6_fsstress/test_arch6_fsstress.c` + `app.mk`：4 worker × 3 FS（ext2/tmpfs/vfat）并发 create→写 32 段→读校验→rename→**跨 worker 读邻居 .done**→删除；`shared/` 并发创建/删除窗口；管道屏障（1 事件管道 + 4 释放管道，子进程 5 fd）
- `config/inittab.arch6-fsstress`
- `qemutests/arch6_fsstress.py`（fail-closed 解析器 + runner）+ `qemutests/test_arch6_fsstress.py`（单测 13/13）
- 接线：`Makefile`（inittab 切换）、`mk/project.mk`（旗标校验/互斥/变体 slug/SUBMAKE_ALLOWED）、`config/rootfs.mk`（三分支过滤）、`mk/components/run.mk`（flavor/镜像/gate/白名单/usage/TEST_HARNESS_MODULES/QEMU_SMP 透传）、`mk/profiles/x86_64-clang.mk`（**USER_VARIANT 回退补 arch6 slug——user 子 make 不含 project.mk，缺这个变体目录会指错**）、`qemutests/run_test.py`（分派）
- 调试残留（**收尾前必须移除**）：`test_arch6_fsstress.c` 的 `DIRCHK`/`CROSSDBG` 打印与 cross-open 200 次 `sched_yield` 重试；`kernel/fs/tmpfs.c` 的 LOOKUP-MISS 子项转储探针（未验证完，最后一次构建被中断）

## 3. 当前卡点：跨 worker rename 可见性竞态（**基线即有，非 ARCH-6 回归**）

### 3.1 症状（SMP=4，suite 与手动 QEMU 一致）

worker 的 cross 阶段 `open(<邻 worker 的 f<N>.done>)` 以 **errno=2（ENOENT）持续失败**（200 次 sched_yield 重试耗尽），但 DIRCHK 证明该文件**存在于目录中**；同一 worker 之后对目录的 readdir 能看到它。失败分布：ext2/tmpfs 间歇性出现；**vfat 从不失败**。失败 worker 跳过删除 → 父进程 `files-dir=N` 断言失败 → 套件 FAIL。

### 3.2 A/B 判定

把同一压测程序放进 **base（`58069ab3`，零 ARCH-6 改动）** 以 arch10 变体运行（`/tmp/os01-base`，inittab 改 `/bin/fsstress`）：**完全复现同样的 cross rc=-1 失败**（`/tmp/base-fsstress.log`）。⇒ 预先存在的 VFS 竞态，与 bcache/锁升级无关。

### 3.3 已排除

- 块缓存一致性（base 无缓存同样失败）；tmpfs 内存扫描竞态（per-mount mutex 串行化）；sscanf 极简 libc（已改手工分词）；release 管道错位（已修）；屏障协议（事件流已验证正确）；套件文件串口模式（arch10 对照通过）。

### 3.4 已有假设与下一步

- 主假设：**rename 目标的负 dcache 项/目录块可见性竞态**。`vfs_rename`（vfs.c:2221-2225）在 fs op 前做 `dcache_invalidate×2 + dcache_flush_mount`，fs op 后 `dcache_mutation_end_rename`（dcache.c:405）再次 flush 挂载——理论上负项必被清；但 vfat（查找路径不同）免疫、ext2/tmpfs 中招，指向 fs 级 rename 内部或 walk 侧某个**在 end_rename 之后**才写入的负缓存/陈旧节点。
- 下一步（构建被中断的那一步）：跑通 `kernel/fs/tmpfs.c` 的 LOOKUP-MISS 子项转储（miss 时打印目录 children 数组）——若 miss 时文件**在**数组里，问题在 walk/dcache 层；若**不在**，问题在 tmpfs rename 的同目录 children 重排（`tmpfs_vfs_rename` 的 `od == nd` 分支 idx 重查逻辑，tmpfs.c:566）。
- 修复路线三选一（恢复会话与用户确认）：
  1. **修 VFS/tmpfs 根因**（若定位为小改动，属 ARCH-5 dcache 与 rename 交互的真实 bug，值得顺手闭环并在 changelog 记录）；
  2. **降级套件**：cross 阶段改为邻居文件在 rename **前**交叉读取（屏障前互相读对方已写好的 f<N>，语义仍覆盖跨 worker 可见性），规避 rename 可见性竞态，并把该竞态记入 roadmap 为独立修复项；
  3. **保留现状 + 已知问题标记**：套件对 cross-open 重试后仍失败时降级为 WARN 并继续（最弱，不推荐——会掩盖真回归）。
- 推荐：先跑完 §3.4 的 tmpfs 探针定位；若根因在 `tmpfs_vfs_rename` 同目录分支或 dcache 负项，走路线 1。

## 4. 快速迭代方法（本轮沉淀）

- **迭代压测程序**：`make ARCH6_FSSTRESS_TEST=1 image`（约 1 分钟）→ 直接 QEMU `-serial stdio` 看输出（手动跑 60–90s 足够）。
- **正式验收**：`make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress`（gate 要求顶层旗标 + SMP=4；结果归档 `build/x86_64-clang/logs/tests/arch6-fsstress/<ts>/`，注意 **stdout/stderr 归档可能为空**——runner 的串口回显在 make 输出里，`read_until` 受 `tester.timeout` 钳制，已在 runner 内抬到 180s）。
- **单测**：`python3 -m unittest qemutests.test_arch6_fsstress`（13 用例，不启动 QEMU）。
- 内核/用户程序改动后必须重跑 `make ... image`（手动 QEMU 不会自动重建——曾因此用旧镜像调试）。

## 5. 未提交改动清单（恢复会话第一件事：`git status` 核对）

- 新文件：`user/test_arch6_fsstress/{test_arch6_fsstress.c,app.mk}`、`config/inittab.arch6-fsstress`、`qemutests/{arch6_fsstress.py,test_arch6_fsstress.py}`
- 修改：`Makefile`、`mk/project.mk`、`mk/profiles/x86_64-clang.mk`、`config/rootfs.mk`、`mk/components/run.mk`、`qemutests/run_test.py`
- 调试探针（待清理）：`user/test_arch6_fsstress/test_arch6_fsstress.c`（DIRCHK/CROSSDBG/retry）、`kernel/fs/tmpfs.c`（LOOKUP-MISS 转储 + printk include）
- 这些改动**不要按现状提交**；Task 9 完成后应拆为「套件 + 接线」与「探针清理」的干净 diff。

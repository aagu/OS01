# ARCH-6 FS 并发与块缓存 — Task 7 修复交接（2026-11-07）

- 分支：`feature/arch6-fs-concurrency-blockcache`，worktree `.worktrees/arch6-fs-concurrency-blockcache`。
- 计划：`docs/superpowers/plans/2026-11-07-arch6-fs-concurrency-blockcache.md`。
- 规格：`docs/superpowers/specs/2026-11-07-arch6-fs-concurrency-blockcache-design.md`。
- 台账及本轮完整日志：`.superpowers/sdd/2026-11-07-arch6-fs-concurrency-blockcache/`，本轮日志在其 `debug-task7/` 下。
- 本轮范围：接手 Task 7，定位根因、修复自测、验证和提交。**未推进 Task 8**，未合并或推送。

## 已完成任务

| Task | 内容 | commit |
|------|------|--------|
| 1 | vfs_sched_ready 门控、ext2 mutex | `5e233fe1` |
| 2 | FAT per-mount mutex | `1e8c6fef` |
| 3 | tmpfs per-mount ctx + mutex | `54426769` |
| 4 | bcache 读核心与宿主等待环境 | `aa779c33` |
| 5 | 缓存读写、flush、SYS_sync | `50e904da` |
| 6 | LRU、失败保脏、耗尽等待 | `29b96793` |
| 7 | late SMP 自测及本报告所述根因修复 | 本轮 `test(kernel): late SMP cache coherence and exhaustion selftest` 提交 |

## Task 7 的已验证根因

最初报告中的“缓存项静默消失/脏数据丢失”是旧构建、控制超时和调试探针共同造成的误判；本次复现未发现缓存元数据绕过移除路径被清空的证据，生产缓存算法未修改。

1. **构建首先失败**：遗留自测调用 `bcache_debug_free_slots()`，但头文件没有声明。补齐正式测试接口声明后，重新构建实际 selftest 镜像再复现；不能拿旧 kernel.bin 的结果推断当前源码行为。
2. **idle harness 与 runnable worker 互等**：CPU0 的填充/持 pin worker 忙等 harness 命令；harness 自己是 CPU0 idle task。只要该 worker runnable，调度器就不会恢复 idle harness，命令无法发出。另一个 CPU 的 worker 最终在等待 `bc_miss_r2_go` 时超时，设置 `bc_abort`。
3. **后续循环被 abort 跳过**：邻接写入及 2048 次 pin 循环均有 `!bc_abort` 条件，因此超时后没有执行。debug_pin 后自己的 pin 采样为 1，而 harness 看到 0，是 worker 超时后主动 unpin，再退出让 idle 恢复；不是 pin 无故消失。
4. **探针篡改等待计数**：临时 audit 将全设备哈希项数与单设备 entries 比较，必然误报，并给 waiters OR 上 `0x40000000`。这让“看到 waiter”断言提前满足，掩盖真实的控制问题。全部临时 cache 探针已删除。
5. **邻接自测本身有竞态和漏报**：两个 worker 共用一个 512B 缓冲，失配分支只打印、不设置失败。现改为 per-worker 缓冲，逐字节检查并设置 sticky failure。

栈假设未成立：`STACK_SIZE` 实际为 **32 KiB**，不是历史建议所猜测的 4 KiB；本轮没有通过扩大栈掩盖问题。

## 修复后的测试协议

- worker 等待控制命令改用真实 `blocker_wait()`，条件包括进度、abort 和 deadline。worker 阻塞后 idle harness 才能恢复；不能用 runnable 状态下的 busy loop 或单纯 schedule 代替阻塞。
- 同 sector 原子性由 CPU0/CPU1 真实读写，每批写入要求读方确认，最后必须 acknowledged=2000，避免串行执行或读方提前退出造成假绿。
- BUSY、flush drain、耗尽场景同时检查按设备 waiter 计数和目标 PID 的实际睡眠状态；通过 task_list_lock → rq_lock 快照，不保存可能被自回收的 task 指针。
- 耗尽前明确断言 entries=pinned=2048；释放一项后读请求才能完成。
- 邻接读回和后端可见性都校验完整 512B，不只比较首字节。
- 每阶段收齐两个已创建 worker 的 done 后才复用 fixture。任一失败立即中止后续场景，释放停顿，按成功取得的 pin 数清理；无法确认 worker 完成时保留 fixture，禁止退役仍在访问的设备。
- 保留正式统计、pin/unpin、free-slots 等测试接口；生产 bcache 算法与 Tasks 1–6 行为未改。

## 本轮验证证据

| 检查 | 结果 |
|------|------|
| 原实现新构建复现 | `baseline-built.log`：Task 7 FAIL |
| 控制超时诊断 | `control-timeout-red.log` 对应串口记录 CPU1 等待 bc_miss_r2_go 超时；与 ELF 符号地址匹配 |
| 最小控制修复 | `control-blocking-green.log`：整套 kernel selftest PASS |
| 补强断言后的四核运行 | `strengthened.log`、`repeat-1.log`、`repeat-2.log` 连续三次 **43/43 PASS** |
| 双核运行 | `two-cpu.log`：**43/43 PASS** |
| 宿主缓存回归 | `host-bcache.log`：**23/23 用例 PASS** |
| GDB 中途 abort 注入 | 在 LBA63 的 debug_pin 入口设置 abort，已有 63 项 pin；`abort-cleanup.gdb.log` 和 `abort-cleanup.stdout.log` 证明无 pin/worker 泄漏，Task 7 正确 FAIL，协议到达 END（42 PASS / 1 预期 FAIL） |
| 独立代码复审 | Task 7 可接受，无 Critical / Important；一项 Minor 见下文 |
| 改动格式 | `git diff --check` 通过 |

四核最终归档：`build/x86_64-clang/logs/tests/kernel-selftest/20261107T063410Z-3f23fa65bc1b487f8c667d56a2705399/`。
双核最终归档：`build/x86_64-clang/logs/tests/kernel-selftest/20261107T063658Z-5f34e4b8b5d648d78aeb33dfbfd19ccc/`。
两者均实际核对 9 个 late case 各一次 SELECT/BEGIN/PASS，以及恰好一次 END。

## 非阻断事项与范围边界

- 复审 Minor（暂缓）：concurrent_miss 已断言一次后端读、双方完整数据和真实等待，但尚未另加 fake 设备 entries=2 的独立 key 唯一性断言；常见重复填充仍由后端调用计数捕获。
- 保留已有 bcache_debug_pin 的 552B 栈帧警告及 g_data_pool 未使用警告；Task 7 自测源码编译无新增 warning/error。32 KiB 栈与本次超时证据不支持将该警告判为此次根因。
- 独立复审仅覆盖 Task 7 的实现与既有接口兼容性，不等于 Tasks 1–6 缓存算法整体验证。
- **Task 8 全量回归/正常盘 Ctrl-C 冒烟未执行**；Task 9 压测及 Task 10 文档闭环均未开始。
- 整分支终审、合并仍留给后续明确授权的推进，不因 Task 7 通过自动进行。

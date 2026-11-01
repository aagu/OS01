> 历史交接快照：Task 4 后续修复、最终验证和评审结论见 [P2 验证报告](../reports/2026-10-31-aarch64-ap-sched-p2-validation.md) 与 [最终评审记录](../reports/2026-11-01-aarch64-ap-sched-p2-final-review.md)。下文状态仅对应交接时点。

# Handoff — aarch64 AP scheduler P2 Task 4 (wake variant)

日期：2026-11-01。接手者：codex。原实施者：Luna（executing-plans 内联执行），因 P2-T4 中途交接停止。

## 工作区与状态

- Worktree：`/home/aagu/OS01/.worktrees/aarch64-ap-sched`，分支 `aarch64-ap-sched`。
- WIP 快照 commit：`7ce8c14e`（P2-T4 未完成工作全部在内；`git log 72fd9363..HEAD` 共 10 个 commit，前 9 个为 P1 完成 + P2 T1–T3 完成）。
- 两份 plan：`docs/superpowers/plans/2026-10-31-aarch64-ap-sched-p1-cooperative.md`（**已完成**）与 `docs/superpowers/plans/2026-10-31-aarch64-ap-sched-p2-wakeup.md`（T1–T3 完成，T4 进行中）。
- spec：`docs/superpowers/specs/2026-10-31-aarch64-ap-cooperative-scheduler-design.md`。
- 实施台账（executing-plans）：
  - P1：`.superpowers/sdd/2026-10-31-aarch64-ap-sched-p1-cooperative/progress.md`
  - P2：`.superpowers/sdd/2026-10-31-aarch64-ap-sched-p2-wakeup/progress.md`
  （内容也随 worktree 提交；每任务有 Ruling 行，接手前必读 P2 台账。）
- P1 验证报告：`docs/superpowers/reports/2026-10-31-aarch64-ap-sched-p1-validation.md`。

## 已完成并通过验证的部分

| 任务 | commit | 证据 |
|---|---|---|
| P1 T1–T5 + final review | `bdb932c6`..`fec562aa` | 独立评审 APPROVE（1 Important 已修复 `3a06077c`）；clean 树全门禁 rc=0（报告有逐项矩阵） |
| P2 T1 SGI 传输 | `131cdb75` | `test_aarch64_resched_sgi` 20/20、`test_aarch64_ipi_broadcast` 14/14；ARM sched 16/16、sched-ap 8/8、m3-probe、gic-spi rc=0 |
| P2 T2 blocker 协议 | `b6241b01` | `test_blocker_remote_wake` 34/34；x86 test-host/phase-0/systest/selftest SMP2+8；ARM sched/sched-ap rc=0 |
| P2 T3 ap_work+masked WFI | `bcf648cd` | `test_ap_work_protocol` 33/33、`test_aarch64_idle_wakeup` 33/33、boot-contract 43/43；ARM sched-ap/sched/m3-probe/no-ack rc=0 |

## P2-T4 当前状态（WIP `7ce8c14e`）

### 已就绪（可信赖）

1. **runner phase=wake gate 完成并测试通过**：`qemutests/aarch64_ap_scheduler.py` 的 `check_ap_positive_evidence(text, cpus, phase)` 同时支持 ap/wake 两相（wake schema 冻结在 `WAKE_CASE_FIELDS`，值校验含 UP NA 形状、`wfi_enter/return>=100`、sgi 每 CPU 行、`handler==eoi` 等）；`--phase wake` 已接入 `run_positive_boot`/`run_ap_mode`；unittest（`qemutests/test_aarch64_ap_scheduler.py` 共 22 项 + boot_contract 28 项）全 OK；`--self-test` OK。
2. **构建接线完成**：`AARCH64_AP_SCHED_TEST=wake` → `KERNEL_VARIANT=sched-wake`（独立 kernel/image 目录）；`kernel/Makefile` 定义 `AARCH64_AP_SCHED_WAKE_TEST=1` 并把 `selftest/test_aarch64_ap_wake.c` 加入白名单；profile/targets/run.mk（`MODE=sched-wake` 含外层冲突拒绝）/catalog（`aarch64-sched-wake` 行）都已接。wake 变体 ELF 构建通过；符号隔离已验证（wake ELF 含 2 个时序 hook 符号、sched-ap ELF 含 0 个）。
3. **7 个时序 hook 的调用点全部就位**（`AARCH64_AP_SCHED_WAKE_TEST` 包围）：idle.c 四 stage、core.c 的 skip_scan/after_dequeue/before_finish、ap_sched.c 的 before_ready/poll_ready、gic_driver.c 的 sgi_trace（intid==4 dispatch 前/EOI 后，`gic_trace_cpu()` 在 gic.c）。头 `kernel/include/selftest/aarch64_ap_scheduler.h` 已冻结声明。
4. **wake 驱动主体**：`kernel/selftest/test_aarch64_ap_wake.c`（新文件）——6 个 wake case + phase=wake envelope + late ID 声明（selftest.c 分支）+ `selftest_begin_run(7)`（main.c 分支）。

### 已知坏点（接手后先修）

**B1（阻塞）— scan-fallback 在 UP 卡死**：`wake_case_scan_fallback()` 的等待循环每轮烧满 2s（100 轮 > 120s boot 期限 → runner 超时）。UP 日志显示 `case=scan-fallback cpu=0 rounds=100 completed=0 explicit=0 scan=0 na=0`，即 fallback scan 从未唤醒 sleeper。
驱动里已有 TIMEOUT 诊断分支（`log_info("[ap-sched] scan-fallback TIMEOUT ...")`，输出 target/cond/state/blocker），重跑一次 UP boot（`make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake`，看 `build/aarch64-clang/logs/tests/aarch64-sched-wake/<UTC>/stdout.log`）即可拿到现场。怀疑方向（按可能性排序）：
   1. P2 新 scanner 协议下 `sched_unblock_blocked` 的唤醒没有把 worker 放回rq（`blocker_wake_locked` → `task_wake` → `enqueue_task`，但 UP 上 driver 自己 kthread_yield 的时机可能永远轮不到 worker 被 pick——注意 driver 的等待循环条件含 `jiffies < jdl`（jiffies 需要推进，UP tick 正常）与 cycle deadline 双重限制，两条件同时写导致循环过早退出为"没唤醒"）；
   2. `sf_sleeper` 的 `blocker_wait` 在 P2 协议下二次 check 后 block，而 sf_cond 的 atomic 发布顺序（`sf_cond=1` 是 plain store）对 scanner 的 callback 不可见——改 `__atomic_store_n(&sf_cond, 1, __ATOMIC_RELEASE)`；
   3. `rw_cond` 同样是 plain store（remote-wake 用显式 blocker_wake 掩盖了这一点，scan-fallback 无显式 wake，全靠 callback 读到 cond）。

**B2 — remote-wake/wake-switch 的 UP 行打印**：UP（n=0）时 case 函数空转打印 `na=1` 行（gate 接受）；但 remote-wake/wake-switch 的 gate 对 `missing case ... cpu=0` 在 UP 时**不要求存在**（`if not up` gate）而驱动总是打印——一致，无需改。SMP 行为未验证（UP 卡死后矩阵全 FAIL）。

**B3 — 未验证清单**（修完 B1 后按序跑）：
   1. `MODE=sched-wake` 8 boot（1/2/4/8 核 ×2）——gate 已冻结，SMP 各 case 值=`100*(cpus-1)` 形状；
   2. `python3 -m unittest qemutests.test_aarch64_ap_scheduler qemutests.test_aarch64_ap_boot_contract qemutests.test_aarch64_harness qemutests.test_scenario_catalog qemutests.test_suite_catalog qemutests.test_inventory`（当前全 OK，改 driver 后重跑）；
   3. P2 计划要求的最终门禁表 = P1 T5 的全部门禁（clean 两 profile：x86 test-host/test-static/phase-0/systest/kernel-selftest SMP2+8；ARM compile/audit/旧 6 MODE/sched-ap/fatal-owner）+ sched-wake；`make test-harness`；
   4. wake case 行值核对：wake-switch 的 `on_cpu_wakes`/`finish_enqueues` 依赖 before_finish hook 内的真实 `task_wake(prev)` 被 finish-switch 吸收（hook 已实现，SMP 上未观测过——若计数为 0，检查 hook 的 armed 时序：driver 先等 `rw_target[cpu]` 非 NULL 再设 `wsw_target`，但 worker 阻塞后不再跑 finish_switch；**worker 必须先被 BSP 唤醒一次进入 pick→finish 窗口，hook 才有机会触发**——这正是 `wake_case_wake_switch` 等待 `wsw_on_cpu_wakes` 增加的逻辑，若 SMP 上失败从这里查）；
   5. work-sleep 的 `ready_overlap` 计数来自 `test_aarch64_ap_sched_poll_ready`（BSP ready 等待循环内看到 CHECKPOINT 后 submit）——SMP 上未验证。

### 接手注意事项（从台账 Rulling 汇总）

- `make -C hosttests` 必须带 `OS01_PROFILE_FILE`（env 形式）；`MODE=no-ack` 的注入镜像会被其它变体 prep 覆盖，跑前先 `make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi`。
- `MODE=sched`（SCHED_CASE=all）需要 ordinary normal+selftest 变体（prep 已自动建）。
- aarch64 串口行首带 `\r`：runner 的 END gate 不用 `^` 锚（已修，勿回退）。
- host mock 的 `arch_cpu_halt` 在 armed 时 longjmp（生产 fail-stop 不返回）。
- 结构体改动后必须 `make clean`（无头依赖）；当前两 profile 上次 clean 后已构建多轮，最终门禁前按 P1 T5 表重新 clean。
- **执行方式**：继续用 superpowers:executing-plans 内联执行（brief 路径 `.superpowers/sdd/2026-10-31-aarch64-ap-sched-p2-wakeup/task-4-brief.md`），完成 P2-T4 后按 plan 要求做 P2 最终独立评审（review-package + 全门禁 + validation report `reports/2026-10-31-aarch64-ap-sched-p2-validation.md` + roadmap/build.md/changelog 更新 + 提交 `test(aarch64): verify SGI wakeup and idle race boundaries`）。评审 APPROVE 后停止——不得自动推进每核 timer 或 IRQ 抢占。

### P2-T4 剩余步骤清单（brief 逐条对照）

- [x] runner phase=wake schema/gate/unittest（含负向变体）
- [x] 编译期 hook（7 个调用点 + 头冻结 + 符号隔离）
- [x] profile/kernel 宏/受控 submake/显式路径/mode whitelist/catalog
- [ ] **B1 修复**（scan-fallback 卡死）+ SMP 各 case 真实验证（B3）
- [ ] 全部 unittest 重跑 + 全门禁表（clean 后）
- [ ] docs 更新（roadmap/build.md/changelog：P2 完成=跨核协作唤醒+idle WFI，不表示抢占）+ `reports/2026-10-31-aarch64-ap-sched-p2-validation.md`
- [ ] 提交 `test(aarch64): verify SGI wakeup and idle race boundaries`
- [ ] P2 最终独立评审 APPROVE → 停止

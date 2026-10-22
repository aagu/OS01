# test_tty_vintr 间歇超时：idle 观察方被工作线程饿死

2026-10-22，定位并闭环 roadmap 原 parked 项。修改仅涉及
`kernel/selftest/test_tty_vintr.c`，不修改生产调度器、TTY 或信号实现。

## 根因

`task_init()` 在 BSP idle 任务内执行 late selftests。idle 不进入普通
runqueue，`pick_eevdf()` 只在 runqueue 为空时选择 idle。
原 VINTR 工作线程收到 SIGINT 后置 `vintr_seen=1`，然后保持
`TASK_RUNNING`，反复 `schedule()` 等观察方写 `vintr_thread_release`。

当工作线程在 BSP CPU 上运行时，每次 `schedule()` 都会重新入队并选择
普通可运行任务；它即使主动让出 CPU，也不会使 idle 观察方获得执行机会。
观察方无法写 release，工作线程就永远无法退出。`schedule()` 的切换返回
路径还会保持 IRQ 关闭，使该循环可能停止本 CPU 的定时器进展。

这解释了 SIGINT 已设置、目标已 RUNNING、甚至 `vintr_seen=1`，但仍然没有
`[selftest] task tests done` 的现象。10 月 19 日的 `54e44267` 把观察条件改为
signal/state，缩短了发生窗口，但没有消除 RUNNING release-wait 循环；
原 changelog 中的归因和“已修复”结论由本次调查修正。

## 复现证据

基于 `6126504a`：

1. 原始 4 CPU 完整自测本次通过，确认间歇特征。
2. 仅在 `tty_push_input(tty, 0x03)` 后加入 `schedule()`，单 CPU 复现挂死。
3. 将工作线程通过现有 `kernel_thread_on_cpu()` 固定至观察方 CPU，保留
   注入后的 `schedule()`，4 CPU 下也复现原始签名：

   ```text
   [selftest] test_tty_vintr...
   ERROR: kernel selftest: missing END line
   ERROR: kernel selftest: cases declared but never started: ['tty_vintr']
   ```

4. QEMU GDB 在单 CPU 挂死现场读到：

   ```text
   CPU#0 [running]
   #0 schedule ()
   #1 vintr_thread_fn ()
   #2 arch_kernel_thread_entry ()
   vintr_thread_release: 0
   vintr_seen: 1
   ```

4 CPU 失败原始日志保存在本地忽略的构建产物目录：
`build/x86_64-clang/logs/tests/kernel-selftest/20261022T155027Z-34be05e602a145c8bca0c08dde6aac34/`。
该次除缺失 tty_vintr/END 外没有其他 case 失败。

## 修复与回归约束

- 工作线程收到 SIGINT 时记录真实 `TASK_RUNNING` 唤醒结果，然后以
  `TASK_UNINTERRUPTIBLE` 等待 release。pending SIGINT 不应把该等待重新唤醒。
- 检查 release、发布睡眠状态、cleanup 的 release + `task_wake()` 共用
  `task_list_lock`，覆盖 wake-before-switch 和 release-before-first-sleep。
- idle 观察方使用有 jiffies 截止的等待，主动 `schedule()` 并重新开启 IRQ，
  不再使用依赖宿主速度的固定指令自旋预算。
- cleanup 等待工作线程恢复 `PF_KTHREAD` 并停止访问 fixture 全局数据，
  再复用 fixture；cleanup 超时仅打印 PID，避免线程刚自回收后的悬空访问。
- 恢复原 TTY foreground pgrp 和调用者 IRQ 状态。
- 固定覆盖同 CPU、跨 CPU、同 CPU 首次睡眠前提前清理。正常路径在注入
  VINTR 后主动切换，持续检验原来导致死锁的时序。
- 超时诊断保留目标 `cpu/state/on_cpu/on_rq` 和所属 CPU 的 `schedule_count`。

## 验证

构建与独立运行：

```sh
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest
```

两次完整套件均通过（后者包含评审发现的 cleanup 超时诊断修正）。
此外用同一 selftest 镜像调用 `qemutests/run_kernel_selftest.py`，2/4/8 CPU
各连续 3 次：`tty_vintr` **9/9 PASS**，每次均观察到同 CPU、跨 CPU、提前
清理 PASS 和 END，无 VINTR 超时。该矩阵完整套件仅 **2/9 PASS**，其余
7 次唯一失败均为已有 `fd_refcount rc_-1`，不能将矩阵报告为全绿。
最终镜像另跑单 CPU：同 CPU和提前清理均 PASS，END 为 38/39，唯一
失败为 `fd_refcount`（该测试要求跨 CPU overlap，单 CPU 不满足）。

`python3 -m unittest qemutests.test_kernel_selftest_result`：43 tests OK。
未与 syscall systest 合跑，也未修改其他 flaky selftest。

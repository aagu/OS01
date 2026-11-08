# BOOT-5 调度启动与系统服务解耦 — 实施与验收报告

- **日期**：2026-11-08
- **设计文档**：[`docs/superpowers/specs/2026-11-08-boot-5-scheduler-services-design.md`](../specs/2026-11-08-boot-5-scheduler-services-design.md)
- **实施计划**：[`docs/superpowers/plans/2026-11-08-boot-5-scheduler-services.md`](../plans/2026-11-08-boot-5-scheduler-services.md)
- **基准提交**：`262813e4` (`docs(boot): add BOOT-5 spec, plan, review report, and update roadmap`)
- **最终状态**：**已实施并全矩阵验收通过**

---

## 1. 概述与核心变更

BOOT-5 完成了内核启动尾声阶段（调度启动、VFS 运行时交接、用户态初始化、后台服务与测试机制）的解耦与规范化，解决了启动生命周期的结构耦合与竞态隐患：

1. **VFS 运行时交接（VFS Runtime Handoff）**：
   - 将 `vfs_end_bootstrap()` 明确前置到标准 I/O（`/dev/tty`）就绪之后、首个用户任务（`/bin/init`）发布之前。
   - 在 `vfs_end_bootstrap()` 执行时原子设置 `vfs_bootstrap_active = false` 并向全系统广播状态；在首任务创建前输出明确协议标桩 `[boot-5] vfs-runtime`。
   - `spawn_user_task()` 在入口处校验 `vfs_bootstrap_active()`，若 VFS 仍处于 bootstrap 阶段则 fail-closed 返回 `-EAGAIN`，保证用户态无法在 bootstrap 窗口内被调度执行。

2. **初始用户任务准入校验与原子发布（Placement Eligibility & Atomic Publication）**：
   - 提取 `spawn_boot_target_allowed(cpu)` 纯函数，验证目标 CPU 是否满足：`num_cpus != 0` 且 `cpu < num_cpus` 且 `cpu < NR_CPUS`；`percpu_data[cpu].online == true`；且满足 `scheduler_ok == true` 或 `(cpu_id() == 0 && cpu == 0 && sched_boot_creation_open())`。
   - 用户任务进入调度可见前，在持有 `task_list_lock -> rq_lock` 双重锁保护下重新核验准入资格与 VFS runtime 状态；若目标不再合法或仍在 bootstrap，则回滚清理未发布资源并返回 `-EAGAIN`。
   - 准入校验通过后，在持锁原子区间内完成 `list_add_to_before` 与 `enqueue_task`，确保任务一旦出现在 `task_list` 中即同步就绪于对应 runqueue，消除窗口裂隙。发布成功后输出标桩 `[boot-5] init-created pid=1`。

3. **架构机制与公共策略彻底解耦（Userspace & Service Decoupling）**：
   - 创建公共用户态启动编排模块 `kernel/init/userspace.c`（导向 `kernel_start_userspace()`），统一执行：创建窗口开启 → futex/console 初始化 → 架构准备（`arch_sched_boot_prepare()`）→ `init_mm` 段元数据构造 → idle 发布 → stdio 准备 → `vfs_end_bootstrap()` → `/bin/init` 创建（PID 1）→ 自测创建探针与 boot services 准备 → 启动调度器（`sched_boot_start()`，输出 `[boot-5] runtime-ready window=closed`）→ 启动运行期服务与 late 自测 → 进入架构 idle（`arch_sched_boot_idle()`）。
   - 创建后台系统服务模块 `kernel/init/services.c`（`boot_services_prepare()` 与 `boot_services_start()`），将 lwIP 网络后台线程与 thermal worker 线程从架构 `sched_boot.c` 剥离。
   - 消除 `kernel/arch/x86_64/cpu/sched_boot.c` 中原 260+ 行的公共业务策略代码，仅保留纯硬件机制钩子：`arch_sched_boot_prepare()`（TSS/IST 平台初始化）与 `arch_sched_boot_idle()`（`arch_cpu_halt` + `schedule` 轮询）。
   - 将 late selftests 迁移至独立模块 `kernel/selftest/boot_late.c`，解开架构实现与测试套件的硬耦合。

4. **测试框架与全矩阵覆盖**：
   - 新增 6 组 host 单元测试（`hosttests/cases/test_boot_services_*.c`），覆盖 VFS 门禁、placement 判定、发布锁序、网络/thermal 服务分离、late 自测隔离与架构机制纯度。
   - 新增 QEMU E2E 测试套件 `SUITE=boot-services`（8 组矩阵：normal 1/2/4/8 CPU、no-net 1/2 CPU、overlap 2/4 CPU），全面验证启动标桩全序、PID 1 不变量、镜像 SHA256 隔离与并发安全性。

---

## 2. 变更提交清单

| 提交 Hash | 类型与领域 | 说明 |
|-----------|-----------|------|
| `262813e4` | `docs(boot)` | 添加 BOOT-5 spec, plan, review report 并更新 roadmap |
| `b904fd38` | `fix(boot)` | Task 1: 将 VFS bootstrap 结束点移至首个用户任务发布前，实现 VFS 门禁 |
| `ec7c60d9` | `fix(sched)` | Task 2: 增加初始任务 placement 准入重检，实现原子发布与回滚 |
| `19514f18` | `refactor(boot)`| Task 3: 解耦用户态初始化、后台服务与架构机制实现 |
| `86bf6ef9` | `test(boot)` | Task 4: 实现 boot-services 测试固件、host 测试与 QEMU 8 组矩阵 |
| `1d4f8042` | `test(boot)` | Task 4 补丁: 注册 catalog 场景、CI 工作流条目与 makefile 变量 |
| 本次提交 | `docs(boot)` | Task 5: 记录调度服务解耦边界、更新架构文档与验收报告 |

---

## 3. 验证命令与结果

### 3.1 宿主测试与静态审计
```bash
make test-host
# 输出: 100% tests passed, 0 failures

make test-static
# 输出: all static checks passed (layout, canary, runtime, receipts)

python3 -m unittest qemutests.test_ci_workflow qemutests.test_scenario_catalog qemutests.test_boot_irq_time qemutests.test_boot_services
# 输出: Ran 94 tests in 2.148s, OK
```

### 3.2 BOOT-5 核心 QEMU E2E 矩阵 (`SUITE=boot-services`)
```bash
make test-qemu SUITE=boot-services
# 输出:
#   [test-qemu] SUITE=boot-services case=normal cpus=1: PASS
#   [test-qemu] SUITE=boot-services case=normal cpus=2: PASS
#   [test-qemu] SUITE=boot-services case=normal cpus=4: PASS
#   [test-qemu] SUITE=boot-services case=normal cpus=8: PASS
#   [test-qemu] SUITE=boot-services case=no-net cpus=1: PASS
#   [test-qemu] SUITE=boot-services case=no-net cpus=2: PASS
#   [test-qemu] SUITE=boot-services case=overlap cpus=2: PASS
#   [test-qemu] SUITE=boot-services case=overlap cpus=4: PASS
# SHA256 integrity: normal image untouched, cmp before/after matched.
# PASS: all 8 boot-services scenarios passed
```

### 3.3 既有启动与子系统 QEMU 回归
```bash
make test-qemu SUITE=boot-contract  # 7 组矩阵，PASS
make test-qemu SUITE=boot-irq-time  # 10 组矩阵，PASS
make test-qemu SUITE=boot-failure   # 17 组矩阵，PASS
make test-qemu SUITE=inittab-phase  # inittab 阶段状态机，PASS
make test-qemu SUITE=network        # 网络功能，PASS
```

### 3.4 生产级测试与高压力多核回归
```bash
make OS01_SYSTEST=1 test-qemu SUITE=systest
# 输出: PASS: all 99 syscall tests passed (rc=0)

make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest
# 输出: PASS: kernel selftest completed (boot + scheduled task markers) (rc=0)
```

### 3.5 AArch64 跨架构回归
```bash
make PROFILE=aarch64-clang test-aarch64 MODE=boot-contract  # 7 组矩阵，PASS
make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe       # PASS
make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap       # 1,2,4,8 CPU，PASS
make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake     # 1,2,4,8 CPU，PASS
make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick     # 1,2,4,8 CPU，PASS
make PROFILE=aarch64-clang test-aarch64-boot-logo           # PASS
```

### 3.6 无头交互式终端回归（Ctrl-C / 进程组信号 / 终端存活）
```bash
FW=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^firmware=//p')
IMG=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^image=//p')

( sleep 18; printf 'cat /dev/urandom\n'; sleep 4; printf '\x03'; sleep 4; printf 'echo BOOT5_SHELL_ALIVE\n'; sleep 3; ) \
  | timeout 50 qemu-system-x86_64 -M q35 \
      -drive if=pflash,format=raw,readonly=on,file="$FW" \
      -drive file="$IMG",format=raw,if=none,id=disk -device ahci,id=ahci \
      -device ide-hd,drive=disk,bus=ahci.0 \
      -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
      -m 512 -smp 1 -serial stdio \
      -display none -no-reboot -no-shutdown > /tmp/boot5_interactive.log 2>&1
```
**日志核验结果**：
- `[boot-5] vfs-runtime`：正常输出；
- `[boot-5] init-created pid=1`：正常输出，PID 1 由用户态 `/bin/init` 优先取得；
- `[boot-5] runtime-ready window=closed`：正常输出，创建窗口及时闭合；
- `task 4 killed by signal 2 (default)`：`cat` 任务正确被 SIGINT (信号 2) 终止；
- `BOOT5_SHELL_ALIVE`：`terminal.elf` 与 `ash` 正常存活并回显命令输出。

---

## 4. 结论

BOOT-5 的所有目标与断言已全部达成：
- VFS runtime 交接点明确且受到门禁保护；
- 初始用户任务在双重锁保护下原子发布并具备准入重检机制；
- 用户态启动策略、后台服务与架构底层机制解耦清晰；
- 全套测试（单元、集成、端到端、多核压力、跨架构及交互式终端）全部通过，无任何回归。

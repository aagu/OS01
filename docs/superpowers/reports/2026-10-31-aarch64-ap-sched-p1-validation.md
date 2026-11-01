# aarch64 AP 协作式调度 P1 验证报告

日期：2026-11-01。分支 `aarch64-ap-sched`，基线 `72fd9363`（P1 实施前的 spec/plan 文档提交），P1 实施 commits `bdb932c6` → `2634b244`，交付提交（runner/catalog/文档接线）见本文末尾。

实施者：Luna（executing-plans 内联执行，任务台账见 worktree `.superpowers/sdd/2026-10-31-aarch64-ap-sched-p1-cooperative/progress.md`）。spec：[2026-10-31-aarch64-ap-cooperative-scheduler-design.md](../specs/2026-10-31-aarch64-ap-cooperative-scheduler-design.md)；plan：[P1](../plans/2026-10-31-aarch64-ap-sched-p1-cooperative.md)。

## 任务完成记录（TDD，每任务 RED→GREEN→门禁→提交）

| 任务 | commits | 关键产出 | 焦点测试（RED 先行） |
|---|---|---|---|
| T1 共同接口 | `bdb932c6` | `<arch/sched.h>` 门面 + 两 arch `cpu/sched_notify.c`；`ARCH_HAS_REMOTE_RESCHED` 删除；need_resched/scheduler_ok/t->cpu atomic 审计（读 acquire/写 release/pick 内 relaxed 清零）；scan 门与 `-EOPNOTSUPP` | `test_sched_notify`（61）、`test_sched_notify_{x86,aarch64}_backend`（7/5）、ctor mask 套件 |
| T2 并发观测 | `1b72f796` | observer 模块（token/批次/FREEING/512 槽/失败旗标）、kthread 释放点 token 协议、每 CPU 首次入口槽（TPIDR_EL1 定位）、每 CPU switch/timer 观测、reclaim 迁 4×250 批次 | `test_aarch64_sched_observer`（1044 断言，pthread 并发） |
| T3 AP handoff | `3af9d4a1` | `ap_sched.c` prepare/release/released/enter/idle_handoff + 冻结 admission、验证 trampoline、fatal 无栈单 owner 仲裁 + fatal-owner 变体、轮询 idle、`aarch64_smp_accepted_mask` | `test_aarch64_ap_boot_contract`（19→26 项）+ fatal-owner 2 核 boot |
| T4 生产 7 case | `2634b244` | `test_aarch64_ap_scheduler.c`（before_release admission + run 7 case）、runner positive 相位（8-boot 矩阵）、严格 envelope/字段值 gate | `test_aarch64_ap_scheduler`（15 项 schema/负向） |
| T5 交付 | 本报告提交 | catalog 2 行、CI nightly 覆盖、inventory 推导化、MODE=sched all prep 补建 ordinary 变体、文档 4 份 | catalog/scenario/inventory unittest、host_build_contract、ci_workflow |

## 最终门禁矩阵（两 profile `make clean` 后逐项执行）

以下前 20 项来自 clean 后的串行执行（日志 `/tmp/t5-gates/<name>.log`，逐项命令与退出码同 `summary.txt`）；最后两项为修复后重跑（修复内容见下文「首次运行偏差」）。

| Gate | 命令 | 退出码 |
|---|---|---|
| clean x86 | `make PROFILE=x86_64-clang clean` | 0 |
| clean arm | `make PROFILE=aarch64-clang clean` | 0 |
| x86 host | `make PROFILE=x86_64-clang test-host` | 0 |
| x86 static | `make PROFILE=x86_64-clang test-static` | 0 |
| x86 boot | `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | 0 |
| x86 syscall | `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest` | 0 |
| x86 selftest SMP2 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` | 0 |
| x86 selftest SMP8 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` | 0 |
| ARM compile | `make PROFILE=aarch64-clang test-static CASE=sched-common-compile` | 0 |
| ARM audit | `make PROFILE=aarch64-clang test-aarch64-audit` | 0 |
| ARM sched | `make PROFILE=aarch64-clang test-aarch64 MODE=sched` | 首跑 2 → 修复后 **0** |
| ARM smp | `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | 0 |
| ARM m3-probe | `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | 0 |
| ARM gic-spi | `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | 0 |
| ARM sync-fault | `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | 0 |
| ARM no-ack（注入镜像先建） | `make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi` 然后 `... AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64 MODE=no-ack` | 0 / 0 |
| ARM sched-ap 正向 | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap`（8 boot：1/2/4/8 核 ×2） | 0 |
| ARM sched-ap fatal | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap AP_SCHED_CASE=fatal-owner`（独立 2 核 boot + ELF 审计） | 0 |
| harness | `make test-harness` | 首跑 2 → 修复后 **0** |

## 新 suite 运行证据

- `MODE=sched-ap` 正向 8 boot 全 PASS（runner `matrix-summary` 16 状态全 PASS）；每 boot 的 `[ap-sched] START phase=ap cpus=N accepted_mask=(1<<N)-1` / 7 case 行 / `END phase=ap failed=0` 均被严格解析。
- fatal-owner：恰一行 `owner=<0|1> arrivals_mask=3 entrants=1 losers=1`；`llvm-nm` 审计 fatal ELF 含 `aarch64_ap_fatal_owner_enter`、普通 `sched-ap` ELF 零注入符号。
- 旧 `MODE=sched` 16/16 PASS（8 positive boot + 4 fault 变体 × 2 CPU 数），普通 idle smoke `--cpus 1 2 4 8` 全 PASS。
- runner/framework unittest：`test_aarch64_ap_scheduler`（15）+ `test_aarch64_ap_boot_contract`（26）+ `test_aarch64_harness`（109）+ catalog 三件套（15）+ host_build_contract/ci_workflow（25）全 OK；`aarch64_ap_scheduler.py --self-test` OK。

## 首次运行偏差与修复（均已在最终矩阵中重验）

1. `MODE=sched`（SCHED_CASE=all）在 clean 树上因 ordinary normal/selftest ELF 未构建而 audit FAIL（"ELF not found"）——运行顺序问题，非产品缺陷。修复：`_test-aarch64-prep-sched-all` 追加 `_test-aarch64-prep-sched-ordinary`（构建普通 + KERNEL_SELFTEST=1 两个 ordinary 变体），重跑 rc=0。
2. `test_inventory_unique` 硬编码 153 个 host 测试二进制——与本轮新增 6 个测试冲突。按 plan「inventory 数量从真实注册表推导，不手填」修复：断言改为与 `make print-host-test-inventory` 实际输出集合全等。
3. `test_all_catalog_suites_covered_in_ci` 要求 catalog 新行有 CI 覆盖——在 nightly full-matrix 作业中新增 "AArch64 AP cooperative scheduler P1" step（sched-ap + fatal-owner）。

## 结论

P1（轮询协作式 AP 调度）按 spec §3/§4/§6/§7 验收通过：accepted AP 经单向 task-stack handoff 进入公共 EEVDF 调度器，显式 pinned kthread 在目标 CPU 完成创建/yield/退出/异栈回收全生命周期，BSP 扫描 fallback 与 admission 语义冻结。**边界保持：idle 忙等轮询、无 resched SGI、无抢占、无 AP timer。** P1 基线 commit：`2634b244`（T4 末），交付提交见 `git log 72fd9363..HEAD`；P2 实施前须以实际合入树逐项核对其 plan 中的接口清单。

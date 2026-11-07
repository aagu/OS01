# BOOT-1 CPU 生命周期与 SMP 握手 — 实施验证报告

日期：2026-11-08（实施窗口 2026-11-07～11-08）
分支：`feature/boot1-cpu-lifecycle`；基准 `8524c55b`（plan R2 文档提交）；
最终实现 commit：见 git log（`631de498` / `c1258280` / `07b6aa18` /
`b38b6ca1` + 文档提交）。spec：[BOOT-1 R2 设计](../specs/2026-11-07-boot-1-cpu-lifecycle-design.md)
（`boot1_spec_review` APPROVE）。执行方式：inline（executing-plans），
账本 `.superpowers/sdd/2026-11-07-boot-1-cpu-lifecycle/progress.md`。

## 1. 契约落地位置（spec §4/§5/§6 → 代码）

| 契约 | 生产代码 | 证据 |
|------|----------|------|
| percpu_init 只构造、不发布 online（§4.1） | `kernel/percpu/percpu.c`（memset 边界与末尾 8 字节机制保留） | `test_boot_cpu_lifecycle`（真实 percpu.c，构造后 online=0、identity/rq 有效、尾布局保持） |
| online 发布为 release、跨 CPU acquire（§4.3） | 发布点：x86 BSP `boot.c:107`、x86 AP `smp.c`（tick 尝试后）、aarch64 BSP `boot/main.c arch_boot_percpu`、aarch64 AP `smp.c secondary_idle`（页表验证后、boot ACK 前）；消费点全部 acquire（§7 审计） | `test_double_state_publish` 双架构发布顺序源码契约；Task 2 逐点审查（账本生产者表） |
| ipi_ready=1 ⇒ online=1（§4.4） | `ipi_ready_publish_bsp/ap` 前置 acquire 断言 | `test_x86_ipi_ready_publish`（契约更新为 acquire 形式） |
| x86 两阶段握手与 trampoline 复用门（§6） | `smp_boot_aps`：残留状态 invariant 检查 → INIT-SIPI-SIPI → `x86_smp_wait_u32_acquire(online, 5000000u)` → TSC go/sample（release/acquire，`tsc_boot` 先于 `tsc_sampled`）→ `x86_smp_wait_u32_acquire(scheduler_ok)` → acquire 复核 online∧ipi_ready → `[boot-1] ready` → 下一 AP 填写 | `test_x86_smp_boot_sync` 七类边界（initial-ready / publish-on-first-pause / never-ready / zero-budget / last-check-ready / publish-after-last-pause / NULL）；phase-0 与 boot-lifecycle 矩阵真实运行 |
| 失败 fail-closed、资源永不回收（§6） | `boot_fatal_cpu()`（noreturn）：`[boot-1] FATAL cpu=<id> stage=<prepare\|online\|scheduler\|invariant>` + kpanic；删除原 list_del/free/继续分支 | boot-lifecycle no-online / no-scheduler 组（fatal 后 ≥1 s 观察、无 ready/complete/PID1/后续 CPU） |
| TSC 可选超时不冒充启动完成（§6.4） | 采样超时保持 offset=0 + 告警（非 fatal），但不产生 ready 行 | 源码契约 + normal 组 complete 语义 |
| aarch64 三层资格（§5.3） | AP runtime online 独立于 boot ACK；NO_ACK 只抑制 ACK；admission 只用冻结 accepted mask | `test_double_state_publish`、`test_aarch64_ap_boot_contract`；no-ack 真实运行 `[boot-1] a64-no-ack cpu=1 online=1 ipi_ready=1 scheduler_ok=0 accepted=0` + `SHOOTDOWN-PROBE: OK`（未接纳 AP 参与 shootdown） |
| num_cpus 语义（§4） | 遍历上界，不压缩、不因失败回收 | `docs/subsys/subsys.md` 更新；消费者仍遍历 num_cpus |

## 2. 隔离故障矩阵（spec §8.2）

选择器 `X86_SMP_BOOT_TEST=none|delay-online|no-online|no-scheduler`
（经 `KERNEL_EXTRA_CFLAGS`；双 profile 从有效宏派生 `boot-lifecycle-<case>`
变体；aarch64 profile 拒绝）。注入只存在于故障变体（smp.c 宏内；
normal ELF 无注入 marker/宏选择符，runner 预检 ELF 身份）。

8 组矩阵（`make test-qemu SUITE=boot-lifecycle`，每 case 90 s，runner
自停 QEMU，RunArchive/RunReport 归档）：normal 1/2/4/8、delay-online
2/4、no-online 2、no-scheduler 2 — 全部 PASS（`final-gate.log` 与
`build/x86_64-clang/logs/tests/boot-lifecycle/`）。普通镜像 hash 在首个
故障构建前取、矩阵结束后 cmp 复核不变。

## 3. 最终门禁（最终文件状态）

命令与返回码记录在 `.superpowers/sdd/2026-11-07-boot-1-cpu-lifecycle/final-gate.log`：

| 门禁 | 结果 |
|------|------|
| `make PROFILE=x86_64-clang test-host` | rc=0 |
| `make PROFILE=x86_64-clang test-static`（含 x86 layout / canary / link-order） | rc=0 |
| `make PROFILE=x86_64-clang test-harness` | rc=0（58 个新 boot-lifecycle 契约测试在内） |
| `make PROFILE=x86_64-clang test-qemu SUITE=boot-lifecycle` | rc=0（8/8 组） |
| `QEMU_SMP=1` / `QEMU_SMP=2` phase-0 | rc=0 / rc=0 |
| `make test-qemu SUITE=network` | rc=0 |
| `OS01_SYSTEST=1 test-syscall`（`KERNEL_SELFTEST` 必须留空，见 §5） | rc=0（99/99） |
| `KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` | rc=0 |
| aarch64：MODE=smp、no-ack（注入镜像）、m3-probe、sched-ap、sched-wake、sched-tick | 全部 rc=0 |

串口/结果归档：`build/<profile>/logs/tests/<suite>/`；
ARM no-ack 记录行证据：`build/aarch64-clang/logs/tests/aarch64-uefi-smp/`。

## 4. RED→GREEN 记录（每行为变更先有失败证据）

- 构造 RED：`construct_cpu_stays_offline` 在旧 percpu_init 下
  online==1 失败（红线保存在账本 red-construction.log）。
- 接口 RED：`x86_smp_boot_sync.h` 缺失时的编译失败（red-wait.log）。
- 分发 RED：未知 CASE 非零失败（red-bogus.log）。
- NO_ACK parser RED：fixture 加入 probe/record 要求后 acceptance 失败
  （实施转录）；no-ack 真实运行随后产出记录行转 GREEN。
- boot-lifecycle 套件 RED：58 测试全部行为失败（NotImplementedError
  stub + 接线缺失），后转 GREEN。
- 对当前已正确行为（TLB 三态、picker 三态、notify 状态）按 plan 允许
  直接记录兼容性基线 GREEN，未伪造 RED。

## 5. 未覆盖范围与遗留风险

- **BOOT-2（早期 IRQ / 校准 / 时间设施顺序）未实施**：本期修复的是
  CPU 生命周期握手；"整体启动安全已证明"的结论不成立。首次 IRQ 可达
  路径、PIT/LAPIC handoff、早期校准依赖仍是 roadmap BOOT-2～5 的
  内容。
- x86 AP tick 启动失败的 accounting 行为保持原样（spec §5.2 明示本期
  不宣称解决）。
- aarch64 "未接纳但 IPI ready 的 AP 继续 shootdown" 的动态证据来自
  no-ack 场景的单点观察（`SHOOTDOWN-PROBE: OK` + 记录行），未覆盖
  多 AP 混合故障组合（属 BOOT-3 通用失败传播范围）。
- delay-online 场景不要求真实 QEMU 在固定轮数内被抢占到 BSP 等待循环
  （spec §8.2）；等待确实观测发布前状态由 host 边界测试证明。
- `KERNEL_SELFTEST=0` 不是"关闭"：kernel/Makefile 用 `ifdef`，传 `0` 仍会把 43 项内核自测编进 systest 镜像并在单核下产生 x86_intr 假失败；正确写法是 `KERNEL_SELFTEST=`（留空）。plan Task 5 Step 2 的字面 `KERNEL_SELFTEST=0` 按 AGENTS.md gotcha 语义修正执行。
- 实施过程中的另两处环境事实已固化进套件：QEMU 需 virtio-rng 熵设备
  （否则 CSPRNG fail-closed、init 永不启动）；`KERNEL_SELFTEST=0`
  不等于关闭（kernel/Makefile 用 `ifdef`，必须传空值）。

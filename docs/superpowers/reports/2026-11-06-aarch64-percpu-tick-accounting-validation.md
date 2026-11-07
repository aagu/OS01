# aarch64 每核调度 tick 记账 — 验证报告

日期：2026-11-06（Asia/Shanghai）。工作树：`/home/aagu/OS01`；分支：`feature/aarch64-percpu-tick`；基线：`58069ab3`。

状态：**功能完成，独立评审 APPROVE**。Task 1–4 已按计划逐个提交（`5e9e3c40` / `a0b6be69` / `1f3bbaed` / `8f1805f6`），本报告记录 Task 5 的最终门禁、逐核证据、变异收敛与独立评审结论；独立评审提出的 2 个 Important 已修完并各自有针对性验证（见第 9 节）。

权威文档：[设计 spec](../specs/2026-11-06-aarch64-percpu-tick-accounting-design.md)、[实施 plan](../plans/2026-11-06-aarch64-percpu-tick-accounting.md)。

## 1. 精确语义（不得含糊的部分）

| 项 | 值 | 依据 |
|---|---|---|
| 本核 tick 频率 | **名义 100 Hz**（`AARCH64_SCHED_TICK_HZ = 100`） | `kernel/include/arch/aarch64/sched_tick.h` |
| 采样语义 | **一计真实递送的 CNTP 中断 = 一个样本**；不做墙钟补算，丢掉的 tick 不事后补 | spec §4；`kernel/arch/aarch64/cpu/sched_tick.c` |
| 全局时间基准 | **仍由 BSP 独占**：jiffies、poll timeout、timer list / TIMER_SIRQ、blocker scan；AP 不推进全局时间 | spec §3 |
| 每核记账 | 打断时 `current == rq->idle` → idle 样本；否则 busy 样本 + 给该任务加 1 个 runtime tick | `kernel/sched/accounting.c` |
| vruntime | 每核 tick 驱动，每样本 +1；aarch64 上 `schedule()` **不再**计费（否则同一 tick 双计、vruntime 依赖 yield 频率） | `kernel/sched/core.c` / `kernel/sched/fair.c` |
| BSP reschedule | 每个有效本核 tick（含 idle tick）release-store `need_resched=1`，使空 runqueue 也走到普通协作入口的 blocker 扫描 | `kernel/arch/aarch64/cpu/sched_tick.c` |
| AP reschedule | AP 仅在本核繁忙任务越过 EEVDF deadline 时置位；**idle tick 不无条件请求调度** | 同上 |
| `tick-deadline` 切片 | worker 必须真跑满 `EEVDF_MIN_SLICE = 10` 个**tick 驱动**的 vruntime 增量；未跑满则发布 `need=0`，该行**失败**（而不是被 BSP 无条件的 `need_resched=1` 蒙混过关） | `kernel/selftest/test_aarch64_sched_tick.c` |
| CNTP 配置 | 启动期**冻结**：BSP 一次性发表（字段先写、ready 最后 release-store），AP acquire 读并校验后只 arm 自己的计数器 | `kernel/arch/aarch64/platform/time.c` |
| CNTP 校验 | freq ≥ 100 Hz；period = freq/100 且 `1 <= period <= INT32_MAX`；PPI ∈ 16..31；AP freq 与 BSP 不一致必须失败；arm 失败后清 CTL ENABLE | 同上 |
| GIC handler | BSP 注册**恰好一次**；重复注册（`-2`）必须失败 | `kernel/arch/aarch64/intr/gic.c` |
| 锁序 | usage lock（`sched_account_tick()` 内部取用并释放）与 rq lock **从不嵌套**；tick 不取 `task_list_lock` | `kernel/sched/accounting.c` |
| 中断路径 | tick handler 与 IRQ 返回路径都**不得**调用 `schedule()`；仍是 EL1 协作式 | spec §4 |
| 布局 | 不改 UAPI / `boot_context` / `task_t` / `thread_t` / `percpu_t`；保持 `-mgeneral-regs-only`；x86_64 行为不变 | Task 5 Step 1 的产物审计 |

## 2. 逐核证据

`MODE=sched-tick` 在**每个已录用 CPU 上**发布七个冻结 case 各一行。证据在**被描述的 CPU 上**采集：idle 窗口在目标自己的 idle 上下文测量，busy/yield/deadline/global 窗口在目标核的 pinned worker 上测量，remote 窗口让每个 CPU 都成为一次真实 SGI4 的目标。

8 核 boot（`20261107T023110Z-38fa36e2f3d146e2b4a2254af9a84a43`，独立评审修复后的最终矩阵那轮；`[sched-tick] START cpus=8 accepted_mask=255`）的原始证据行：

```
[sched-tick] case=tick-ready cpu=0 started=1 valid=1 hz=100
[sched-tick] case=tick-idle cpu=0 samples=3 idle=3 busy=0 runtime=0 vruntime=0
[sched-tick] case=tick-busy cpu=0 samples=7 busy=7 idle=0 runtime=7 vruntime=7
[sched-tick] case=tick-yield cpu=0 calls=16 samples=0 runtime=0 vruntime=0 self_switches=0
[sched-tick] case=tick-deadline cpu=0 need=1 irq_switches=0 nested=0 regs_ok=1
[sched-tick] case=tick-global cpu=0 local_samples=1 busy=1 idle=0 global_ticks=1 jiffies_delta=1
[sched-tick] case=tick-remote cpu=0 sgi_ok=1 tlb_ok=1 wfi_enter=1 timer_live=1 na=0
...（cpu=1..7 同构，AP 的 global_ticks 恒为 0，jiffies_delta 只在 cpu=0 出现）
[sched-tick] END failed=0
```

逐 case 语义：

| case | 冻结关系 | 本次实测 |
|---|---|---|
| `tick-ready` | `started=1 valid=1 hz=100` | 每核均满足 |
| `tick-idle` | `samples>=3`、`idle==samples`、`busy=0`、`runtime=0`、`vruntime=0` | 每核 `samples=3 idle=3 busy=0 runtime=0 vruntime=0` |
| `tick-busy` | `samples>=7`、`busy==samples`、`idle=0`、`runtime==samples`、`vruntime==samples` | 每核 `samples=7 busy=7 idle=0 runtime=7 vruntime=7` |
| `tick-yield` | `calls=16`、`samples=0`、`runtime=0`、`vruntime=0`、`self_switches=0` | 每核 `calls=16 samples=0 runtime=0 vruntime=0 self_switches=0` |
| `tick-deadline` | `need=1`、`irq_switches=0`、`nested=0`、`regs_ok=1`；**前置条件**：worker 的 tick 驱动 vruntime 增量 ≥ `EEVDF_MIN_SLICE=10`（不满足则发布 `need=0`，该行失败） | 每核全部满足 |
| `tick-global` | `local_samples>=1`、`local_samples==busy+idle`；AP `global_ticks=0`；CPU0 额外 `local_samples==global_ticks==jiffies_delta` | 每核 `local_samples=1`；仅 CPU0 有 `global_ticks=1 jiffies_delta=1` |
| `tick-remote` | `sgi_ok=1 tlb_ok=1 wfi_enter>=1 timer_live=1 na=0`；**SMP1 整行为全零且 `na=1`** | 每核 `sgi_ok=1 tlb_ok=1 timer_live=1 na=0`；`wfi_enter` 为累计值（CPU0=1 为其自身 WFI 探针，AP 为其 idle 累计） |

采样与 charge 的一致性因此是**逐 CPU 自洽**的：busy 窗口的 `runtime`/`vruntime` 增量与 `samples` 严格相等；idle 窗口两者恒为 0；yield 窗口（IRQ 全程关闭）三者全为 0。

`tick-remote` 的 SGI 证明来自 GIC 自己的 IAR/EOIR trace（`gic_driver.c` 的 SGI4 entry + EOIR 写入观测），**不是**用 periodic timer 的返回冒充 SGI ACK；`timer_live` 单独证明目标核的周期 tick 仍在增长。SMP1（单核）时 remote 显式 NA（`na=1` 且四个值全 0）。

## 3. 最终门禁矩阵

全部命令的退出码均为 0。

| 门禁 | 命令 | 退出码 |
|---|---|---|
| x86 产品 | `make PROFILE=x86_64-clang kernel.bin` | 0 |
| arm 产品 | `make PROFILE=aarch64-clang aarch64-uefi-kernel` | 0 |
| 静态审计 | `make PROFILE=x86_64-clang test-static` | 0 |
| arm 公共调度编译 | `make PROFILE=aarch64-clang test-static CASE=sched-common-compile` | 0 |
| arm 中断审计 | `make PROFILE=aarch64-clang test-aarch64-audit` | 0 |
| host 全量 | `make PROFILE=x86_64-clang test-host` | 0 |
| x86 phase-0 | `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | 0 |
| x86 selftest SMP=2 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` | 0 |
| x86 selftest SMP=8 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` | 0 |
| x86 systest | `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest` | 0（99 个 syscall 用例全通过） |
| arm smp | `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | 0（9 boot 全 PASS） |
| arm gic-spi | `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | 0 |
| arm m3-probe | `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | 0 |
| arm sync-fault | `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | 0 |
| arm sched（全矩阵） | `make PROFILE=aarch64-clang test-aarch64 MODE=sched` | 0（16 boot 全 PASS，含空 rq blocker ≤ 10 jiffies 验收） |
| arm sched-ap | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap` | 0（8 boot 全 PASS） |
| arm sched-wake | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake` | 0（8 boot 全 PASS） |
| **arm sched-tick** | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick` | **0（8 boot 全 PASS）** |
| arm no-ack（注入） | `make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi` → `... test-aarch64 MODE=no-ack` | 0 / 0 |
| arm 恢复验证 | clean → 普通 `MODE=smp` | 0（9 boot 全 PASS） |
| harness | `make PROFILE=x86_64-clang test-harness` | 0（`ALL MODULES PASSED`，497.38 s；首轮因第 7 节的门禁缺陷 FAIL，修复后复跑通过） |

独立评审提出的 2 个 Important 修好后（见第 9 节），受影响门禁**全部重跑**：

| 门禁 | 命令 | 退出码 |
|---|---|---|
| arm sched-tick（make 级） | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick` | 0（8 boot 全 PASS） |
| arm tick 证据矩阵（runner） | `python3 qemutests/aarch64_sched_tick.py --cpus 1 2 4 8` | 0（8/8 PASS） |
| arm smp / gic-spi / m3-probe / sync-fault / sched / sched-ap / sched-wake | `make PROFILE=aarch64-clang test-aarch64 MODE=<mode>` | 各 0 |
| host / runner 单测 | `python3 -m unittest qemutests.test_aarch64_sched_tick qemutests.test_ci_workflow qemutests.test_inventory qemutests.test_suite_catalog qemutests.test_scenario_catalog qemutests.test_kernel_selftest_result qemutests.test_aarch64_ap_boot_contract` | 0（168 用例 OK） |
| harness | `make PROFILE=x86_64-clang test-harness` | 0 |

no-ack 按计划要求走完整的 **clean → 注入构建 → 运行 → clean → 普通恢复 SMP** 序列，注入镜像没有被复用作正常产品。

最终 `MODE=sched-tick` 矩阵（独立评审修复后复跑）的逐 boot 记录：

| CPUs | run | boot id | 结果 |
|---|---|---|---|
| 1 | 1 | `20261107T023049Z-9a5f83e2125a40669882bb996d07c164` | PASS |
| 1 | 2 | `20261107T023051Z-95761dca8be1496f8adcd050767cd440` | PASS |
| 2 | 1 | `20261107T023053Z-091f41e588b34de6896017368b29bbe8` | PASS |
| 2 | 2 | `20261107T023056Z-09e113609b8a472aa3e4b916fec045dd` | PASS |
| 4 | 1 | `20261107T023058Z-41ed50be6b46414cad548df62970c8f4` | PASS |
| 4 | 2 | `20261107T023101Z-98d97b8d88ce4af89c6884cd3a59f5d6` | PASS |
| 8 | 1 | `20261107T023104Z-67c39a02c96e44d182a7df539292ee47` | PASS |
| 8 | 2 | `20261107T023110Z-38fa36e2f3d146e2b4a2254af9a84a43` | PASS |

八次 boot 使用同一套产物：

- `image_sha256`: `b9cda82d33c93bbaf9a3a0552e3ca79448a9a950dc8c6d3d434e97a40ba58037`
- `elf_sha256`: `d91cad246312816fb924ea507e43e1e0eb3f57052eb01058b7b5167fa7275391`
- `firmware_sha256`: `1435274a427feffcdacd09bd933fffcc08f18fdc6141bf461ae575f047a25dd8`
- QEMU 命令：`qemu-system-aarch64 -M virt,gic-version=2,acpi=off -cpu cortex-a53 -smp <N> -m 512 -drive if=pflash,... -drive if=none,file=<image>,format=raw,readonly=on,id=disk -device virtio-blk-device,drive=disk -serial stdio -display none -no-reboot -no-shutdown -dtb <per-boot diagnostic dtb>`

host 重点套件（本轮复跑）：

| host 套件 | 结果 |
|---|---|
| `test-aarch64-sched-tick-host`（链接真实 `sched_tick.c`/`time.c`/`fair.c`/`accounting.c`） | 11 用例 / 139 断言全通过 |
| `test-aarch64-sched-charge-host`（同一 `core.c` 的 aarch64/x86 双编译视图） | 2/2 通过 |
| `test-monitor-host CASE=accounting` | 138/138 通过 |
| `test-aarch64-resched-sgi`（真实 GIC MMIO spy） | 43/43 通过 |

> **环境侧已知抖动（与本轮代码无关）**：`tools/tests/test_kernel_headers.py` 在本会话中**间歇性**失败，失败点在 `kernel/Makefile` 的 `install-headers` 配方里的 `rm -rf $(INSTALL_ROOT)`——本机运行时的「可恢复删除」shim（`rm` → `mavis-trash`）在把 `/tmp` 下的目录搬进 XDG 回收站时被沙箱**间歇性**拒绝（`mavis-trash: failed to trash '…/headers'` / `the sandbox denied moving this file to the trash`）。判定它是环境问题而不是回归的依据有三条：(1) 失败路径与本轮改动无交集——本轮 diff 未触及 `kernel/Makefile` 的 `install-headers`；(2) **不涉及任何项目文件**的最小复现即可触发（`mavis-trash --rm-compatible -rf <tmpdir>/headers`，8 次里 6 次失败，加 `MAVIS_TRASH_FORCE_MV=1` 仍失败）；(3) 同一条 `make test-host` 命令在本会话早前与本轮重跑中都出现过 rc=0。该用例只测内核头暂存，与每核 tick 无关。

## 4. 产物审计（专用符号不泄漏）

`llvm-nm` 审计结果：

- `build/aarch64-clang/kernel/sched-tick/kernel.elf` 含且仅含本变体需要的三个专用符号：`test_aarch64_sched_tick_run`、`aarch64_sched_tick_observe`、`aarch64_sched_tick_irq_probe`；
- 普通产品 ELF（`kernel/kernel.elf`、`kernel/selftest/kernel.elf`、`kernel/sched/kernel.elf`、`kernel/sched-ap/kernel.elf`、`kernel/sched-wake/kernel.elf`）与 x86 产品 ELF 对**全部 12 个** tick 专用符号（observe / sleep-boundary / sgi-trace / irq-switch / irq-nested / self-switch / irq-probe / idle-work / send-sgi / sleep-probe / run）命中数均为 **0**——tick 自测源在非选中变体里是空 TU。

`aarch64_sched_tick_irq_probe` 的反汇编核对（`llvm-objdump`）：本核计数指针由调用方传入（`ldr x9,[sp,#0x60]` + `ldar x10,[x9]`，不是固定全局地址），`x19–x30` 保存/恢复，调用者保存寄存器哨兵 `x0–x5`/`x18` 逐个校验，等待路径是 `mrs CNTVCT_EL0` + `wfi`。

独立评审要求探针只承认「自探针入口起**严格增长**」的样本（否则探针开始前落下的 tick 会被当成打断证据）。实现方式是在哨兵安装**之后**再取一次入口快照 `ldar x10,[x9]` → `str x10,[sp,#0x90]`，等待条件变为 `counter > entry && counter >= target`。为此探针帧从 144 字节扩到 **160** 字节：新槽位放在 `sp+0x90`，**不能**复用 `sp+0x58`——`stp x29,x30,[sp,#0x50]` 把 x30 放在 0x58，覆写它会让 `ret` 跳到计数值（见第 7 节记录的缺陷）。

## 5. 变异收敛记录

| 变异 | 期望 | 实测 | 恢复后 |
|---|---|---|---|
| Task 2：重复 EOI（`gic_driver.c` 二次 `gic_eoi`） | PPI EOI 用例失败 | 失败 4 处 | 源已恢复，对应用例通过 |
| Task 2：重复计费（`sched_tick.c` 两次 `sched_account_tick`） | busy/idle 计数用例失败 | 失败 5 处 | 源已恢复，对应用例通过 |
| Task 2：AP 全局 tick（`time.c` 的 CPU0 判定放宽） | `global_tick_cpu0_only` 失败 | 失败 | 源已恢复，对应用例通过 |
| Task 3：parser 负例（46 个单测，含评审 I-2 新增的 3 条） | 每种破坏形态都被拒 | 全部被拒 | — |
| **Task 4：重复计费（`sched_tick.c` 二次 `sched_account_tick`，产品变体）** | **tick runner FAIL** | **FAIL：`case tick-busy cpu=0: vruntime=4 != samples=8`、`case tick-busy cpu=1: vruntime=4 != samples=8`** | **源已恢复（`git diff` 为空），同镜像 2 核两次复跑 PASS** |
| **Task 5：仅在 BSP 停用 `update_curr()`（评审 I-1 的针对性变异）** | **BSP 的 `tick-deadline` 行必须失败** | **FAIL：`case tick-deadline cpu=0: field need=0 violates in [1,1]`，并同时报出 `case tick-busy cpu=0: vruntime=0 != samples=7`；runner 退出码 1** | **源已恢复（`git diff` 为空），重建后 tick 矩阵 8/8 PASS** |

最后一条变异说明新门禁确实会咬人：双计费使 busy 样本翻倍而 vruntime 只走一次 tick，冻结关系 `vruntime == samples` 立刻被拒；而 I-1 修复后，「BSP 上 vruntime 不再被 tick 驱动」也不再能蒙混过关——deadline 切片跑不满就发布 `need=0`，`need∈[1,1]` 的冻结关系直接拒绝该行。

Task 4 的实现过程中另有一处真实缺口被门禁抓到并修复：初版 remote case 让 CPU0 报 NA 而把 SGI 证据记在 AP 名下，与“证据在被描述的 CPU 上采集”冲突；两核首跑即 FAIL（`sgi_ok=0 tlb_ok=0 wfi_enter=0`），改为**每个 CPU 都是一次真实 SGI4 的目标**（AP 由 BSP 唤醒，BSP 由远端 AP 唤醒并用显式睡眠探针真正入睡）后转绿。

## 6. 独立评审

见第 9 节。

## 7. 门禁抓到的真实缺陷（已修复）

全量 harness 首轮**失败**：`qemutests.test_aarch64_ap_boot_contract` 的
`test_arm_idle_uses_the_masked_wait_helper` 断言“idle 的 C 源码里不得出现
`wfi` 字样”（P2 契约：idle 只能经由被审计的 `aarch64_idle_wait_irq()` 睡眠，
不得内联睡眠指令）。Task 4 新增的 observer 名字 `aarch64_sched_tick_wfi_enter`
/ `..._wfi_return` 与 `arch_aarch64_tick_wfi_probe` 命中了该字面量检查。

修复方式**没有改动该门禁**：observer 钩子从 idle 的 C 文件里移出，经
arch 私有包装 `aarch64_sched_tick_sleep_boundary()`（定义在
`kernel/arch/aarch64/cpu/sched_tick.c`）上报睡眠边界；探针改名
`arch_aarch64_tick_sleep_probe()`，仍调用同一个被审计的等待助手。修复后
`test_aarch64_ap_boot_contract` 28/28 通过，tick 变体的 1/2/4/8 矩阵与普通
产品/普通 selftest 构建均不受影响。

这条记录同时说明：AP 调度器的源码契约门禁对本轮改动**是有效的**，不是走过场。

独立评审的 M-2 修复（探针只承认「自入口起严格增长」的样本）在第一次实现里踩到**汇编栈槽位冲突**：`aarch64_sched_tick_irq_probe` 的 `stp x29,x30,[sp,#0x50]` 把 **x30 放在 sp+0x58**，而新增的入口快照写到了同一个 `sp+0x58`，于是 `ldp x29,x30,[sp,#0x50]` 把 x30 恢复成计数值 `0x14`，`ret` 跳到 `0x14` → **取指异常**：

```
Taking exception 3 [Prefetch Abort] on CPU 0
...with ESR 0x21/0x8600000e
...with FAR 0x14
...with ELR 0x14
[aarch64-sync] FATAL mpidr=0x0000000080000000 ec=0x21 esr=0x000000008600000e elr=0x0000000000000014 spsr=0x0000000060000345 far=0x0000000000000014
```

定位手段：QEMU `-d int` 的异常日志给出「最后一次 IRQ 在探针的 `wfi` 处、随后立刻取指异常」，再用 gdb 挂 QEMU 的 gdb stub（`-S -gdb tcp::1234`）在探针入口与 `ret` 两处下断，直接读出 `x30` 由 `0xffff0000400a0740` 变成 `0x14`。

修法：探针帧从 144 扩到 **160** 字节，入口快照改放 `sp+0x90`（并在该处写明「不得复用 x30 槽位」），重建后 8/8 矩阵全 PASS。

这条缺陷是**评审修复自身引入的**，被 boot 门禁当场抓住（而不是靠人眼读汇编）；这也说明「冻结协议 + 全量矩阵 + 崩溃即失败」的组合在修 bug 时同样在起作用。

## 8. 未跑项与局限

- **未跑项**：无。计划 Task 5 列出的全部门禁都已执行；本报告不把任何未跑项记为 PASS。
- **局限**：**仍然没有 IRQ 返回抢占**。本轮只交付“每核本地 tick + 记账 + tick 驱动 vruntime”，调度仍是 EL1 协作式：tick handler 与 IRQ 返回路径都不调用 `schedule()`，唤醒靠协作入口与 SGI4 请求，不抢占不合作的线程。
- **aarch64 的 timer list / TIMER_SIRQ 不会真的触发**（先存在，不在本轮范围）：BSP 的全局 tick 会 `set_softirq_status(TIMER_SIRQ)`，但 aarch64 侧**没有任何 `do_softirq()` 调用方**，该 softirq 无人消费。本报告因此**不声称** aarch64 的 timer list 会按 jiffies 触发。
- 其它仍在范围外：任务迁移、EL0/用户 MM、统一 `kernel_main`（均未开始，roadmap 未提前标记完成）。
- 100 Hz 是**名义**频率：QEMU 下的真实样本数以实际递送的 CNTP 中断数为准，本报告不声称任何墙钟精度。
- `tick-remote` 的 `wfi_enter` 是累计值（BSP 为其自身睡眠探针次数，AP 为其 idle 累计）；它证明目标核**经过**了等待路径，不表示 QEMU 物理省电。

## 9. 独立评审结论

评审方式：由独立 verifier 子代理**只读**全量改动（Task 1–4 的 4 个提交 + 工作区），对照设计 spec 与实施 plan 逐条核对实现、证据与门禁；本节结论与修复均由该评审驱动。

**首轮结论：APPROVE-with-nits** —— Critical = **0**，Important = **2**，nit = 4。

### 9.1 Important（必须清零）——已清零

| # | 评审发现 | 处置 | 验证证据 |
|---|---|---|---|
| **I-1** | BSP 的 `tick-deadline` 行**不可失败**：`need=1` 被 BSP 无条件的 `need_resched=1` 满足，因此证明不了 vruntime→deadline 机制；「若 BSP 上 `update_curr()` 被静默禁用，该行仍 PASS」。同时，计划 Task 4 Step 4 的「worker 至少跑满 `EEVDF_MIN_SLICE=10` 个有效 tick」在发布字段里**没有任何字段承载** | 产品侧改 `tick_measure_deadline()`：先记录 worker 自身的 `vruntime`，**等 tick 驱动的 vruntime 增量 ≥ `EEVDF_MIN_SLICE`（10）** 再读 `need_resched`；切片没跑满就发布 `need=0`（fail closed）。**冻结字段集、7 个 case ID、行格式一律未动** | 针对性变异：在 `kernel/arch/aarch64/cpu/sched_tick.c` **只对 BSP** 停用 `update_curr()` → runner 拒收（退出码 1）：`case tick-deadline cpu=0: field need=0 violates in [1,1]`，同时 `case tick-busy cpu=0: vruntime=0 != samples=7`；恢复源码后重建，tick 矩阵 8/8 PASS |
| **I-2** | runner 未把 BSP 的 `local_samples` 与 `global_ticks`/`jiffies_delta` 绑定；`local_samples` 也没有下界，空窗口 `0==0+0` 一样能过 | runner **只收紧、不加字段、不改 ID/行格式**：`tick-global` 的 `local_samples` 下界改为 1（每核）；CPU0 增加 `local_samples == global_ticks` 与 `global_ticks == jiffies_delta`（生产者在**同一个 IRQ 屏蔽窗口**里读这三者，实测 `1/1/1` 成立） | 新增 3 条单测（空窗口 `local_samples=0`、BSP 局部样本≠全局 tick、jiffies≠全局 tick）+ runner `--self-test` 负例；`qemutests.test_aarch64_sched_tick` 46/46、相关模块合计 168/168 通过；产品实测 CPU0 `local_samples=1 global_ticks=1 jiffies_delta=1` |

### 9.2 nit（可选，已一并修）

| # | 内容 | 处置 |
|---|---|---|
| M-1 | `kernel/arch/aarch64/platform/time.c` 的文件头注释仍写 `tick_handler()` 负责「need_resched=1, watchdog++」，实际这两项早已移到 `aarch64_sched_tick()` | 注释改成与实现一致（全局 tick 只推进 jiffies / poll timeout / timer list / TIMER_SIRQ） |
| M-2 | 探针用调用方预算的**绝对** `target`；若 tick 落在「调用方读数 → 探针首次读数」之间，`regs_ok=1` 但探针其实没被打断 | 探针改为「自入口起**严格增长**」（入口快照取在哨兵安装之后），并把帧扩到 160 字节；实现中踩到 x30 栈槽冲突并当场修掉（见第 7 节） |
| M-3 | `test_aarch64_sched_tick.c` 里 idle 为 NULL 时发布 `idle_runtime=1, idle_vruntime=1` | 保持 fail-closed 行为，只补注释说明「该分支不可达，发布的是**让冻结行失败**的值，绝不发布能通过的值」 |
| M-4 | aarch64 上**没有任何 `do_softirq()` 调用方**，BSP 全局 tick 抬起的 `TIMER_SIRQ` 在 aarch64 上无人消费 | **先存在、不在本轮范围**：本报告**不声称** aarch64 的 timer list 会触发；见第 8 节 |

### 9.3 评审确认成立的部分（未修改）

- 设计 spec 的十条契约在实现中全部成立；
- tick / IRQ 路径**从不**调用 `schedule()`（EL1 协作式保持不变）；
- usage lock 与 rq lock **从不嵌套**，tick 不取 `task_list_lock`；
- 无 UAPI / `boot_context` / `task_t` / `thread_t` / `percpu_t` 布局漂移，`-mgeneral-regs-only` 保持；
- aarch64 的 `current` 在中断上下文正确（`arch_current_task()` 的栈掩码在 IRQ 帧之下仍指向本任务）；
- tick 专用符号**不泄漏**到普通产品 ELF；
- 构建选择器互斥正确（x86 profile 拒绝 aarch64 的 tick 选择器、`MODE=sched-tick` 拒绝外来覆盖）；
- wake 变体已正确加固（AP 私有 tick 需要 mask/restore）。

### 9.4 最终结论

**APPROVE**。

- Critical = 0（首轮即 0，修复后复核仍为 0）；
- Important = 2 → **已全部修完并各自有针对性验证**（I-1 有专门的 BSP-only 变异被拒收，I-2 有 3 条新单测 + 产品实测）；
- nit = 4 → M-1/M-2/M-3 已修，M-4 为**先存在**的限制并已在第 8 节写明，不属于本轮缺陷；
- 修复后受影响门禁全部重跑通过（见第 3 节），唯一由修复引入的缺陷（探针 x30 栈槽冲突）也在本轮被 boot 门禁当场抓住并修复。

## 10. 提交

- Task 1 `5e9e3c40` `feat(aarch64): prepare shared CNTP configuration and local timer start`
- Task 2 `a0b6be69` `feat(aarch64): account local scheduler ticks on admitted CPUs`
- Task 3 `1f3bbaed` `test(aarch64): validate per-CPU tick evidence and runner failures`
- Task 4 `8f1805f6` `test(aarch64): exercise local tick accounting across SMP CPUs`
- Task 5 `docs: record aarch64 per-CPU tick accounting validation`

独立评审提出的修复按计划「仅在发现真实缺陷时**回到对应 Task 修复**」归位，未新增任务提交：`fixup!` 提交在变基时自动并入对应任务——M-1（`time.c` 注释）并入 Task 2，I-2（runner 收紧 + 3 条新单测）并入 Task 3，I-1 / M-2 / M-3（`tick_measure_deadline` 的切片强制、探针严格增长与栈帧修正、fail-closed 注释）并入 Task 4。变基前后 **tree 哈希一致**（`ef6534f7…`），即被验证的**代码**内容与被提交的代码内容逐字节相同；其后本报告只回填了提交号（代码部分零改动）。变基前的状态另存于分支 `backup/aarch64-percpu-tick-before-autosquash-20261107`。

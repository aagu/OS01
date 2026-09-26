# 已完成工作汇总（Changelog）

> OS01 各阶段已完成工作的按时间汇总。最新在前（截至 2026-09-26）。
> 本表为历史完成记录，规划项见 `docs/roadmap.md`。

---

## 2026-09-26

- refactor: **AAGU-4 残留清理 — close §3.3 ❌ + 🟡** —— branch `docs/roadmap-slim-v30`（commit 链路 `099b060`（spec）→ `4e1d33b`（plan）→ 4 task commits）：
  - **Task 1**（`b994d8f`）：softirq 原子操作实现从 `kernel/arch/<arch>/cpu/atomic.c` 外部函数迁移到 `kernel/include/arch/<arch>/atomic_bitops.h` static inline + `__attribute__((always_inline))`；`kernel/intr/softirq.c` 删除 2 个 `#if defined(__x86_64__)/#elif defined(__aarch64__)/#else #error` 块
  - **Task 2**（`3ababa8`）：`kernel/time/tick.c` poll-timeout scan + PIT/LAPIC handoff 拆分：weak 默认 `poll_timeout_tick()` 在 `kernel/time/tick.c`（aarch64 phase 1 路径），strong 实现在 `kernel/fs/poll.c`（x86_64）；PIT/LAPIC handoff 合并到 `kernel/arch/x86_64/platform/time.c::arch_tick_start()`；`tick.c` 删除 2 个 `#if defined(__x86_64__)` 块
  - **Task 3**（`29996e9`）：`kernel/include/time/clocksource.h` 2 个 `#if defined(__x86_64__)` 块（percpu include + `clocksource_read_ns()` inline）迁至新建 `kernel/include/arch/x86_64/clocksource.h`；`kernel/time/clocksource.c` + `kernel/time/timer.c` SUBSYS_INITCALL ifdef 删除；`timer.c` spin hint 接 `arch_cpu_pause()`
  - **Task 4**（`e91d0b9`）：`kernel/driver/{ahci,keyboard,pit,serial}.c` + `kernel/net/net.c` 共 5 个 TU 删除冗余 `#ifdef __x86_64__` SUBSYS_INITCALL 守护（TU 已在 x86-only 路径，Makefile 已 gate）
  - 文档同步：`docs/arch/cross-boundary-symbols.md` §3.3 ❌/🟡 状态全改 ✅，§6 验收清单增条目
- fix(aarch64): **IPI cpus≥2 FAIL 根因修复（TPIDR_EL1 误读）** —— commit `d695020`（worktree `fix/aarch64-ipi-fail`，当前 HEAD）：`kernel/arch/aarch64/intr/ipi_test.c::ipi_cpu_id()` 从 `TPIDR_EL1` 取指针后误解释为 `aarch64_boot_percpu_t *`（实际 `percpu_t`），跨字段偏移导致 cpu≠self 时 `id` 计算错位（`cpu=1 received=3`、`cpu=2/3 received=0`）。两个「暖机」SGI 掩盖真问题：`cpu=0` 暖机时 IPI handler 调 `gic_send_sgi(self,2,0,FILTER_SELF)` 写 `0x02000002`，触发 SGI 2 self-trigger，handler 重入将 `received[cpu]` 加 1 —— 看似通过；删暖机后 `cpu=1..N-1 received == 0`。修：去掉类型转换，改用运行期 `cpu_id()`（`mrs x0, TPIDR_EL1` → `percpu_current()`），删除两个暖机 SGI；`SGI_TEST_ACK_COUNT == N-1`（`cpu=0` 不应收到自给 SGI）+ `GICC_IAR == 0x401`（sgi_int_id=2，CPU targets=1）严格断言。QEMU `make PROFILE=aarch64-clang test-aarch64-uefi-smp` 1/2/4 ×3 共 9/9 PASS（0 TIMEOUT / 0 FAIL）。完整根因 + 修复记录见 `docs/aarch64-ipi-fail-handoff-2026-09-26.md`
- ci: **bucket test targets 迁移** —— commit `36e6230`：CI workflow 从旧的 forwarding `test-*` aliases 切到 6 个 bucket 目标（`test-qemu` / `test-host` / `test-static` / `test-aarch64` / `test-contract` / `test-kernel-selftest`）
- refactor(harness): **删 forwarding `test-*` aliases** —— commit `ce258a1`（worktree `feat/build-system-harness-consolidation` 收尾）：删 09-25 引入的临时 forwarding `test-*` aliases，仅保留 6 个 bucket 目标作为权威入口。follow-up 见 `9f13465`（track followup）

## 2026-09-25

- refactor: **build system harness consolidation**（worktree `feat/build-system-harness-consolidation`，12 commits `70daaea..ce258a1`）：统一 6 个 bucket test 目标（`test-qemu` / `test-host` / `test-static` / `test-aarch64` / `test-contract` / `test-kernel-selftest`），target taxonomy / capability gate / alias policy 收敛到 `docs/build-system-harness.md` 作为权威 reference：
  - `70daaea` plan (v3, 12 tasks) → `64dd4ac` gate x86 validation + 公开隐藏 test target → `5c43897` drop unused `all` alias → `452bbf1` DRY QEMU command lines（`RUN_QEMU_BASE` / `_FLAGS_<target>`）
  - `f2652a7` consolidate 4 QEMU E2E 目标 → `test-qemu SUITE=<name>`（保留旧名为 aliases）→ `58a6ef9` canonical `test-host` + 拆分 pmm helper → `64e3207` `test-static` umbrella + 4 subset aliases
  - `c28e67a` consolidate 3 aarch64 tests → `test-aarch64 MODE=<smp|no-ack|gic-spi>` → `7350c61` consolidate 2 contract tests → `test-contract PROFILE=<name>`
  - `782e964` `docs/build-system-harness.md` 新建（target taxonomy 权威 reference）→ `30f3508` update Quick start / 用户入口 / test recipes
- refactor: **arch source groups**（commit `c3412da` + `18a4cff`，merge `79ccffb`，spec `2026-09-25-arch-source-groups-design.md`）：kernel 源按 `kernel/arch/<arch>/<topic>/` 职责分组（如 `kernel/arch/x86_64/intr/{8259A,lapic,lapic_timer}.c`、`kernel/arch/x86_64/sched/{task,switch}.c`、`kernel/arch/aarch64/intr/{gic_driver,gic,irq_probe,ipi_test,entry.S}` 等），更新所有架构引用路径 + 文档架构图
- refactor: **compiler_rt 目录裁撤**（commit `1c5816b`，merge `ca72145`）：`kernel/compiler_rt/` 整个目录裁撤，符号落点各归其位——`__stack_chk_*` 迁至 `kernel/core/stack_chk.c`；`__udivti3` 走 `runtime/builtins/`；elf-loader 保留 `runtime/builtins/`。消除「目录名与实际职责不符」的违例
- fix(run): commit `1fbca71` aarch64 QEMU harness 改用 selftest 变体镜像
- feat: **AAGU-5.8 三档环境测试 + 验收清单** —— commit `a05d975`（PR #28，worktree `feat/aagu-5-entropy-facade`）：QEMU 默认 / `+rdrand,+rdseed` / 真硬件 三档测试 + 验收清单。spec `docs/arch/entropy-source-facade.md`
- feat: **AAGU-5.7 STRONG-only pool + selftest NONE-mode + spawn/exec 策略** —— commit `587a894`（PR #27，6 commits `c97a8f5` / `3721e93` / `001528e` / `e9ae356` / `21f0bc7` / `587a894`）：STRONG-only pool + selftest NONE-mode 修正 + AT_RANDOM STRONG-only 控制流 + B3 WEAK-pool e2e 测试 + spawn/exec 决策

## 2026-09-24

- feat(aarch64): **AAGU-5.6 arch entropy facade + strong overrides + kernel/random refactor** —— merge #25（commits `4ab3370` / `f41e4e6` / `1c5bf17`）：spec `docs/arch/entropy-source-facade.md` + `arch_random_get_entropy()` + `arch_random_get_strong()` 双接口 + `kernel/random` refactor。x86_64 strong override：`RDSEED` = STRONG / `RDRAND` = WEAK；aarch64 strong override：`RNDRRS` = STRONG / `RNDR` = WEAK
- feat(aarch64): **AAGU-29 aarch64 libk.a link**（commit `bee8da5`，merge #26 `bec4a4c`，worktree `fix/aagu-29-libk-aarch64`）：aarch64 kernel 现在 link `libk.a`（`kernel/arch/aarch64/make.config`: `ARCH_LIBS = -nostdlib -lk`）→ `memcpy/memset/memmove/calloc/free/malloc/strlen/strcmp` 走 libc 单一来源。删 `kernel/compiler_rt/memset.c` weak fallback + `kernel/arch/aarch64/libc_stub.c` calloc/free shim。完整闭项见 `docs/aarch64-libk-aarch64-closure-2026-09-24.md`
- feat: **AAGU-6 P2 风格/头/测试 cleanup batch** —— commit `b580432`（PR #24，worktree `feat/aagu-6-p2-cleanup`）：统一 kernel `__stack_chk_guard` + 移除 `kernel/arch/aarch64/memset.c` weak stub + `idle_resume`/calloc shim 等清理

## 2026-09-23

- feat(libc): **AAGU-4.6 libc atexit 串入 exit 路径** —— commit `c4a93fe`（PR #23）：libc atexit 串入 exit 路径，关闭 AAGU-4.6
- refactor(atomic): **AAGU-4.5 arch_atomic_or/and_u64 facade + softirq.c drop ifdef** —— commit `7828d7d`（PR #22）：新增 `arch_atomic_or/and_u64` arch-neutral facade + `softirq.c` 内 `#ifdef __x86_64__` 移除，关闭 AAGU-4.5
- feat(kernel): **AAGU-4.3.6 kernel/include/compat/stdlib.h → libc 单一来源** —— commit `bfb314d`（PR #20）：删除 `kernel/include/compat/stdlib.h` 文件，改走 libc 单一来源（`git rm compat/`，目录裁撤的早期收口）。至此 AAGU-4.3.1~6 全套（`list/rbtree/string/stdlib/sys-cdefs/sys-types` 6 mirror 头）落地
- feat(kernel): **AAGU-4.3.5 kernel/include/compat/string.h → libc 单一来源** —— commit `93c5403`（PR #19）
- feat(kernel): **AAGU-4.3.2 kernel/include/compat/rbtree.h → libc 单一来源** —— commit `0ddbfa7`（PR #18）
- feat(kernel): **AAGU-4.3.1 kernel/include/compat/list.h → libc 单一来源** —— commit `f976a1b`（PR #17）
- feat(kernel): **AAGU-4.3.4 kernel/include/compat/sys/types.h → libc 单一来源** —— commit `bdb1630`（PR #16）
- feat(kernel): **AAGU-4.3.3 kernel/include/compat/sys/cdefs.h → libc 单一来源** —— commit `06777a2`（PR #15）

## 2026-09-22

- feat: **AAGU-4.2 UAPI auxv + stat.h 单一源收口** —— commit `8937932`（PR #14）：kernel UAPI 为唯一源，libc 端 sysroot 安装
- refactor(compiler_rt): **AAGU-4.4 kernel `__stack_chk_guard` 单一来源** —— commit `5b4f736`（PR #13）：`kernel/compiler_rt/` 内 `__stack_chk_guard` 单一来源（2026-09-25 目录裁撤后迁至 `kernel/core/stack_chk.c`）
- fix(libc/stdio): **AAGU-4.1 libc stdio `fread/fwrite` stream 验证** —— commit `baaa3e9`（PR #12）：`fread/fwrite` 入口加 `is_open_file` stream 验证，关闭 AAGU-4.1
- spec: **AAGU-4 跨边界符号/ABI 边界规范** —— commit `35ed5e3`（PR #11）+ `180b14e`（Explore agent 二次复核）：spec `docs/arch/cross-boundary-symbols.md`（R1 完成）。规则分 4 类：
  - **builtin 类**（kernel 唯一来源，禁止 libc 镜像）—— 落地于 AAGU-4.3 / 4.4
  - **UAPI 类**（kernel UAPI 为唯一源，libc 端 sysroot 安装）—— 落地于 AAGU-4.2
  - **arch-value 类**（arch-neutral facade + strong override）—— 落地于 AAGU-4.5
  - **libc 镜像类**（libc 单一来源，kernel include 仅引用声明）—— 落地于 AAGU-4.6
- feat(aarch64): **AAGU-3 subsys_stub convergence + BSP-exclusive CNTP** —— commit `0345ca8`（PR #10，worktree `feat/aagu-3-subsys-stub`）：`kernel/arch/aarch64/subsys_stub.c` 收敛为 API-parity 占位 + BSP 独占 CNTP 控制（APs 跳过 timer init）。替换条件已记录：AAGU-29 落地后只剩 ~10 行 drop-in
- fix(ci): **AAGU-8 CI 跑在 `ghcr.io/aagu/os01-ci` 镜像内** —— commit `a062a67`（PR #9）：CI 改跑在 published `ghcr.io/aagu/os01-ci` 镜像内

## 2026-09-20

- ci: **AAGU-7 GitHub Actions CI 启用** —— commit `c5e730b`（PR #6）：GitHub Actions CI 启用，初始 workflow

## 2026-09-19

- fix(ci): commit `424e25f`（PR #8）retain LLVM 和 QEMU runtime libraries —— 修复 CI 镜像 missing `.so` 问题
- ci: **AAGU-7.1 ship clang-22 + qemu-11.1.1 CI image** —— commit `2396622`（PR #7）：CI 镜像发布 clang-22 + qemu-11.1.1
- feat: **AAGU-1 / AAGU-2 [P0] CSPRNG entropy fail-closed + UEFI GetRNG + aarch64 log variadic + AT_PLATFORM facade** —— commit `8931cab`（PR #5）：CSPRNG entropy fail-closed + UEFI `EFI_RNG_PROTOCOL` GetRNG + aarch64 log variadic 适配 + `AT_PLATFORM` facade

## 2026-09-18
- feat(aarch64): **Generic Timer Phase 1 + Phase 2 全套闭环（5/5 follow-ups）** —— 6 merges `b2b81fc` / `e115d79` / `10fd3d2` / `3037df3` / `5edf79a` / `a110ab9`（worktrees `feat/aarch64-timer-phase1` 等）：
  - **Phase 1** CNTP + CNTVCT + clocksource 框架（merge `b2b81fc`，`aarch64_timer.c` 133 行 + `clocksource_register`）
  - **Phase 2 #1** SUBSYS_INITCALL plumbing（merge `e115d79`）
  - **Phase 2 #2** `cntp_tick_handler` → `tick_handler` 集成（merge `10fd3d2`）
  - **Phase 2 #3** per-CPU timer / SMP timer（merge `3037df3`）
  - **Phase 2 #4** `__udivti3` hoist 至 `runtime/builtins/`（merge `5edf79a`，commit 详见 `docs/aarch64-udivti3-hoist-closure-2026-09-18.md`）
  - **Phase 2 #5** `-I libc/include` 清理：6 mirror 头（`list/rbtree/string/stdlib/sys-cdefs/sys-types`）改走 `kernel/include/compat/`，由 libc 单一来源提供（merge `a110ab9`，详见 `docs/aarch64-libc-include-policy-closure-2026-09-18.md`）
  - **Phase 2 P2 follow-ups 5/5 全闭环**

- fix(aarch64): **GIC clobber-probe TIMEOUT 修复** —— commit `2481e1f`（worktree `fix/gic-probe-timeout`）：`kernel/arch/aarch64/irq_probe.c:103-105` asm `mov x10,#0x200` + `movk x10,#0x0002,lsl #16` 两个 immediate 错位，实际算出 `0x0002_0200`（SGI 512, filter=LIST）而非 `0x0200_0002`（SGI 2 + filter SELF）；GICv2 静默丢弃 out-of-range SGI → 2 秒 ldar 轮询命中 deadline → `[gic-probe] save-restore TIMEOUT`。修正为 `mov x10,#2` + `movk x10,#0x200,lsl #16`，`x10 == 0x02000002 == gic_send_sgi(dev,2,0,FILTER_SELF)`（C wrapper 编码 `filter<<24 | targets<<16 | sgi&0xf`）。配套 RED→GREEN hosttest `hosttests/cases/test_gic_probe.c`（180 行 + Makefile wiring）：suite 1 用 production gic_driver.c + mock MMIO 断言 C wrapper 写 `0x02000002`；suite 2 静态扫描 irq_probe.c 源码禁止已知 buggy literal pair。QEMU E2E 矩阵 `make test-aarch64-uefi-smp --cpus 1 2 4 --repeat 3` = 9/9 PASS（0 TIMEOUT / 0 FAIL）；hosttests 23/23（x86_64-clang + aarch64-clang）；aarch64 uefi KERNEL_SELFTEST=1 build PASS。`docs/superpowers/specs/.../phase1` §7.3 clobber-probe 设计 + plan §2.2 评审均提及 SGI 2 self-trigger，但 plan:1078 原写法 `movk x10,#2,lsl #24` 本身是 assembler error（lsl #24 非法），实现层的 bug 制造了 *silent* TIMEOUT，plan 的 bug 只会产生 *obvious* 编译失败——一并记入 follow-up

## 2026-09-17
- feat(aarch64): **GICv2 通用中断框架 Phase 1** —— merge `545a935`（worktree `feat/aarch64-gic`，14 commits = 6 spec/plan docs + 8 实现）：
  - spec/plan docs：`1c555f5` GICv2 Phase 1 spec + plan → `2a0f30a` R1 (9) → `42ece05` R2 (7) → `7d59ac4` R3 (1) → `ccf5b47` R4 (4) → `08c6302` plan NIT-1 hosttest 编译路径（合计 22 条修订全落地）
  - Task 1.1/1.2 GICv2 driver 泛化：`c885d94` gic.h + gic_driver.c 198 行 + gic.c wrapper + `a99ed71` mock-MMIO hosttest RED→GREEN（8 suites 69/69）
  - Task 2.1/2.2 QEMU marker harness + entry.S 全量 save/restore + 通用 dispatch：`ec167ae` RED + `d601570` GREEN（4 marker + 22/22 hosttest grep）
  - Task 2.3a/2.3b SPI harness + PL011 RX handler + DTB interrupts 解析 + test-aarch64-gic-spi target：`5180681` RED + `ef19203` GREEN（8 fixture self-test）
  - Task 3.1/3.2 SMP IPI harness + gic_pub.h dsb ishst + ipi_test.c SGI send + handler + per-CPU trace：`a50b8aa` RED + `93bafb3` GREEN（14 fixture）
  - 边界全守：x86_64 0 改动 / AGENTS.md 0 改动 / `kernel/include/arch/barrier.h` 0 改动（R4-2 决策）/ 不编 kernel core / 不进调度器；Spec G1-G6 + R1-R11 全落地。QEMU E2E `test-aarch64-uefi-smp` 全程 **6/6 触发 `[gic-probe] save-restore TIMEOUT`**——根因 asm immediate 错位，已在 09-18 commit `2481e1f` 修复
- docs: **`user-stack-canary` 闭环** —— master `0819e20`（merge），R1-R14 codex review
  + opus v2 重写 + 9 commits subagent-driven。commit 清单：
  - `00a98e6` feat(user): RED canary_smash probe + systest 45/46/47
  - `3d3fed8` feat(libc): GREEN user-space stack canary (SSP) + 3 build flags
  - `414065c` feat(user): canary_dump probe + systest 43/44 guard entropy
  - `2aa346a` feat(build): test-user-canary 7-step Layer 1 audit target
  - `84b05fe` chore(build): preserve busybox symbols via SKIP_STRIP=y
  - `9174fe0` feat(selftest): RED kernel selftest at_random (Task 2.1)
  - `e35c763` feat(kernel+libc): AT_RANDOM auxv + selftest wrapper + getauxval (Task 2 GREEN)
  - `ae472a6` feat(net): LWIP_RAND → kernel ChaCha20 CSPRNG + hosttest path test
  - `815ba57` docs: roadmap + changelog 同步 (Task 4 — user-stack-canary 闭环)
  - `0819e20` Merge feat/user-stack-canary: user-space stack canary + kernel CSPRNG + LWIP_RAND
- feat(libc): 用户态栈 canary —— libc/user/busybox 全面
  -fstack-protector-strong；guard 每次 exec 经 SYS_getrandom 播种；
  systest 43-47；make test-user-canary 构建期审计
- feat(kernel): auxv AT_RANDOM(16B CSPRNG)+AT_PLATFORM("x86_64")；
  getauxval()；kernel selftest at_random_layout/entropy；systest 48-53
- feat(net): LWIP_RAND 接内核 ChaCha20（替换 jiffies LCG）；
  hosttest --wrap 白盒 + test-network 6/6

## 2026-09-16

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **统一用户态启动方式**（worktree `startup-unification`，merge `01c96a8`，9 commits `d7822cc..09264e4`）：`task.c` 两处用户栈构造（spawn argv-only / exec argc+envc）提取共享 `setup_user_stack()`（一处 auxv 逻辑，消除双站点漂移，`56867f1`）；`__libc_start_main` 真实现（`environ` 初始化 + auxv walk，`2a198ab`）；crt0 从寄存器传参（rdi/rsi/rdx）改为标准 SysV `_start`——从 `[rsp]` 解析 argc/argv/envp/auxv 传 `__libc_start_main`，并正确设置 `environ`；busybox overlay crt0 同步（`09264e4`）。执行中发现并顺手修复两个 latent kernel 缺陷作前置——ext2 稀疏洞读零填（Task 0，`d7822cc`/`2773763`）+ `deep_copy_argv` 接受显式空 `{NULL}` argv/envp 数组（Task 4.5，`e4e3a85`/`26be52e`/`9100ea4`）。验证：systest 基线 268 + 11 新启动探针全 GREEN、test-network 6/6、test-inittab PASS、52-applet 回归 clean | 5 天 | 09-12~09-16 |
| **deep_copy_argv over-cap 回归修复**（`8fa1663`）：`26be52e` 删 `if (count==0) return -E2BIG` 同时干掉了合法空数组 + over-cap 两个判断，回归仅 over-cap 检查。补回 `count > MAX_ARGV` 显式拒绝；RED test `tests/runtime/user/test_argv_overcap.c` 验证。merge `b39d571` | 半天 | 09-16 |

## 2026-09-13

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **文档学习层**：`docs/README.md` 索引（按 0/1/2/3/4 层组织，全局认知→架构骨架→核心机制→I/O 子系统→构建调试）+ 4 份源码导读（调度器 / trap.c / vfs+memory / tty+intr），让新人按指定顺序读源码即可走通关键路径。`docs/README.md` + `docs/scheduler-reading-guide.md` + `docs/trap-reading-guide.md` + `docs/vfs-memory-reading-guide.md` + `docs/tty-intr-reading-guide.md` | 1 天 | 09-13 |

## 2026-09-12

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **目录重构 P1-P6**（merge `31bb128`，commits `eb70174` + `0a392c8` + `e45c650` + `f8fbf62` + `99f5f45` + `7551910` + `6730681` + `6e6d180`）：test 目录 → `hosttests/qemutests/kernel-selftest/runtime-selftest`；`kernel/kernel/ → kernel/core/`（字节相同）；headers 按 subsystem 拆分 + `arch/` lift 出 `include/kernel/`；random/log/font/logo 拆出 core/；pic/timer 归属 owning subsystem + pty 迁移；P6 清理 + 全量文档同步 + PMM 测试移植。spec `2026-09-12-directory-restructure-design.md` | 1 周 | 09-04~09-12 |
| **PMM/sched 稳定性系列**（merge `be7db4f`，commits `1506d6e` + `36eb6a3` + `ea89136` + `beb351c` + `0ba888a` + `0809100` + `4468e75` + `514e062` + `b68e1b1`）：MEMORY_RANGE_GRANULE 64-bit + E820 MEMORY_TYPE 保留 + alloc/free/预留 RAM-relative 索引（3 个独立 PMM 隐患，每个先 RED 后 GREEN）+ 调度 lost-wakeup 窗口修复 + 6 例 PMM host 测试 + `find_mount` 防御（user-pointer mount entry → ENOENT）+ SMP=4 boot 双 bug 修复（linker script orphan sections + active PGD lifetime）。SMP=1/2/4 + systest-repeat 7 连 268/268 | 1 周 | 09-02~09-12 |
| **aarch64 页表原语**（spec `2026-09-11-aarch64-page-table-primitives-design.md`，commits `169e0d5` + `c9c0246` + `cdb6625` + `466401d` + `28ac0ea`）：`kernel/arch/aarch64/page_table.c` arch-local walk/map 原语，为 head.S + MMU 铺路。附带 aarch64 PMM 修复：`1c51dfa` 多 zone `alloc_pages` 索引 + `053a226` direct map 覆盖 PMM 分配范围。QEMU smoke 断言（descriptor bit + minimal intermediate descriptor） | 3 天 | 09-11~09-12 |
| **x86_64 内核栈保护**（commits `ea72359` + `4d884d1` + `b918b08` + `1b79ace` + `51f9617` + `d7209c7` + `77c60dd`）：x86_64 内核使用 `-fno-pic -mcmodel=large -fstack-protector-strong`，全局 per-boot guard；审计直接 `R_X86_64_PC32` 或 `R_X86_64_64` guard 引用并拒绝 GOT；编译门控的 QEMU 破坏性 canary trip test 已打印预期诊断 | 半天 | 09-12 |

## 2026-09-09 ~ 09-10

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **v25 arch-cleanup-gh 系列**（merge `3ab4ef1`，10 commits `67132e2..5dbc63d`）：weak-default + strong-override 模式覆盖 7 个子系统。① `67132e2` roadmap v24 doc；② `af9a6ce` bootinfo(arch) E820 → `bootinfo_x86.h`；③ `19b84a8` arch(neutral) `arch/regs.h` pt_regs_t facade + `rwlock_relax()` 走 `arch_cpu_pause()`；④ `dfead87` intr(arch) `arch_irq` hooks 拆分 controller selection + gsi↔vector + dispatch；⑤ `c37522a` rtc(arch) core + per-arch impl 拆分；⑥ `52f99a1` mm(arch) PGD/PUD/PMD/PTE 层级统一 + bit-constant rename；⑦ `0ecee53` arch(subsys) `SUBSYS_INITCALL()` + `.subsys_init` section 替代硬编码 driver 列表；⑧ `58db2a7` arch(sched) `arch_kernel_thread_entry` 弱默认 + x86_64 强覆盖；⑨ `5dbc63d` build(uefi) digest + staged copy 排除 `*.o/*.a/*.lib` 残留 | 3 天 | 09-09~09-10 |
| **PMM arch-neutral**（spec 13 轮 review 通过 + plan 16 task + final fix，17 commits `2c08e78..a2e7389`）：`pmm_init(const struct boot_context *ctx)` 单一入口 + 弱默认 `pmm_arch_normalize`/`pmm_arch_zone_split` 在 `kernel/memory/pmm_arch.c` + x86_64 强覆盖（E820 + kernel-LMA/handoff/trampoline excludes + 2 MiB granule + sort/merge）+ aarch64 强覆盖（读 `aarch64_ram_map_get()`）。`pmm.c` body 用 RAM-relative indexing（`pages_struct + ((start - lowest_ram) >> 21)`），Step 7 clamp 防 aarch64 unsigned-underflow。x86_64 systest 268/268 + nettest 6/6 零退化，aarch64 uefi-smp 9/9 PASS | 3 天 | 09-09 |
| **log API 统一**：`kernel/log.h` 提供 gate-wrapped `log_err/warn/info` 宏（`do { if (LEVEL <= g_log_level) _log_*_impl(__VA_ARGS__); } while(0)`），保留 `log()` core macro + `g_log_level` 调度；`_log_write` 拆为 variadic forwarder + `_log_writev` va_list core；x86_64 走 `_log_writev`/vsnprintf 串口，aarch64 走 `kputs(fmt)` 忽略 variadic（-nostdlib） | 半天 | 09-09 |

## 2026-09-06

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **回退 busybox 副本变通，切回符号链接 rootfs**（回退 `b32e1e0`；开启 busybox `CONFIG_LN/CONFIG_FIND/CONFIG_FEATURE_FIND_TYPE`；rootfs manifest 重新以 debugfs symlink 写入 29 条 applet 链（含 `/bin/ln`）；`mkdisk` 注释同步；`user/systest.c` case 41 改用 busybox `ln` 创链 + lstat 验证 + exec 跟随；case 42 改用 busybox `find -type l` 验证 `VFS_SYMLINK→DT_LNK` 映射。QEMU：systest 268/268（含 41/42 新断言全部 PASS）、nettest 6/6。spec §6 全部落地） | 半天 | 09-06 |
| **symlink/readlink + 4 新 syscall**（commits `43588c8`..`c64c854`）：VFS 软链接 + ext2 symlink（fast ≤60B inline `i_block[0..59]` / long data block via `i_block[0]`）；4 新 syscall `SYS_symlink/readlink/lstat/fstatat`=71..74；libc `syscall3`/`syscall4`（r10 ABI for fstatat）；`vfs_getdents` `VFS_SYMLINK→DT_LNK`；67 systest + 6 kernel selftest。spec `2026-09-05-symlink-support-design.md` v5 | 1 天 | 09-06 |

## 2026-09-02 ~ 09-05

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **自托管 compiler runtime**（多个 commits）：udivti3 实现 + provider-keyed selfhosted archive + provider 构建不变量硬化 + 内核链接 compiler runtime + kernel link publication 加固 + compiler-rt eligibility 验证 + kernel runtime validation targets + syscall/selftest suite 隔离 + variant link paths + root `make sysroot` 入口。详见 `runtime/` + `docs/build.md` | 3 天 | 09-04~09-05 |
| **aarch64 UEFI bootloader 统一**（commits `af166bc`..`06e6127`，merge `06e6127`）：x86_64 + aarch64 共享 `boot/uefi/main.c` + arch 分发 + `boot_context` handoff ABI + boot_context 头部偏移断言 | 1 天 | 09-03 |
| **aarch64 UEFI 固件修复**：firmware 截断 64MiB 适配 QEMU pflash（`11aa6ed`）+ aarch64 UEFI 默认 URL 下载（`bad8825`）+ aarch64 也显式传 clang+lld 到 posix-uefi（`25872d1`） | 半天 | 09-03 |
| **profile-only UEFI overlay 简化**：x86 UEFI 固件 per-profile（不再用运行时 overlay patch）+ host test 按 profile 隔离 + 所有组件强制声明 profile + profile-only UEFI cleanup contract | 半天 | 09-03 |
| **GNU Make 构建系统重构**（profiles + 受控递归 Make + 单 writer sysroot 原子 generation 发布 + 锁/租约协议 + kernel/libc/user/busybox/mbedtls/posix-uefi 组件适配器 + rootfs manifest + mkdisk 重写 + aarch64 bring-up 图分离 + 变体隔离镜像 + 全部公开 target 恢复。QEMU 验证：x86 disk.img 启动、aarch64 handoff/phase1 双签名、systest 228/228、nettest 6/6。契约测试 5 模式全绿）。**Plan deviation**：rootfs applet 项以 busybox 副本替代 symlink（内核 vfs 无软链接跟随 → exec symlink ENOEXEC，历史构建即副本规避）；kernel exec-symlink gap 列入 roadmap（P1/P5） | 1 天 | 09-02 |

## 2026-08-24 ~ 08-26

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **syscall 边界审计**（commits `a1ad1b9`..`80eab1a`，11 commits）：逐 syscall 检查 user-pointer 边界 + 可睡眠路径 + copy_{to,from}_user 失败处理 + ASLR/canary 未来兼容；详见 `docs/syscall.md` 末尾审计触达清单 | 半天 | 08-24~08-26 |

## 2026-08-23

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **roadmap 瘦身（v1）**：已完成内容迁出到 `docs/` 专题文档，roadmap.md 只剩 phase 表 + 待办 + 主题 docs 索引 | 半天 | 08-23 |

## 2026-08-17 ~ 08-18

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **timer 重构** (clocksource+clockevent 双层抽象 + TSC/RTC-PIE 联合校准 + LAPIC tick 接管掩 PIT + CLOCK_MONOTONIC/REALTIME/nanosleep/poll 迁纳秒 + jiffies self-test，systest 150/150，根治 PIT 200Hz) | 2 天 | 08-17~18 |

## 2026-08-16

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **网络回归 harness** (make test-network + OS01_TCP_ECHO_DELAY_MS delayed-reply + 20/20 no-delay + 10/10 delay250 cohort) | 1 天 | 08-16 |
| **DHCP ACD 关闭** (LWIP_DHCP_DOES_ACD_CHECK=0，消除 ~10.6s ACD 竞态导致的偶发不绑定) | 1 小时 | 08-16 |
| **requested-event-aware poll/select** (按请求方向注册/唤醒，修复复合 flags + PTY 双注册容量 + 时序敏感 select 断言) | 1 天 | 08-16 |
| **E1000 RX ring 所有权串行化** (tcpip 线程独占硬件 ring 消费，IRQ 仅 ack + wake) | 半天 | 08-16 |
| **俄罗斯方块游戏** (framebuffer 像素渲染 + 扫描码输入 + alt-screen 恢复 + UX) | 1 天 | 08-16 |

## 2026-08-15

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **per-poll timeout registry** (poll_timeout_head 链表 + PIT 扫描，修复 lost-wakeup + 并发 clobber) | 半天 | 08-15 |
| **keyboard poll 支持** (/dev/keyboard 扫描码 wait queue + keyboard_poll_dev) | 半天 | 08-15 |
| **tty termios 诚实化** (TCSETS 真存储 + raw 默认 + ICANON/ECHO) | 半天 | 08-15 |
| **terminal alt-screen 双缓冲** (?1049h/l) | 半天 | 08-15 |
| **/proc/<pid>/fd/ 观测性** (files_t 引用协议 pin/unpin + dup/dup2/fcntl 路由重构 + exit 路径 pin) | 1 天 | 08-15 |
| **arch 边界收紧** (x86 平台源选择、early task-state hook、公共 gate ABI、arch signal API、端口 I/O wrapper) | 1 天 | 08-15 |
| **网络正确性加固** (DNS 超时、端口字节序、部分读缓存、shutdown、UDP readiness、响应 hang) | 3 天 | 08-12~08-15 |
| **lwIP 网络栈合并** (E1000/virtio-net + PCI/MSI-X + DHCP/DNS + TCP/UDP socket + poll/select + HTTP wget) | 多迭代 | 08-15 |

## 2026-08-01

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **inittab 配置支持** (ACT_* 位掩码修复 + parse_inittab() open/read 解析器 + 3 套模板 + Makefile/build 集成 + test-inittab 相位派发验证) | 1 天 | 08-01 |

## 2026-07-25 ~ 07-29

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **SMP 负载均衡** (idle-steal + per-schedule pull + sched_pick_cpu + nr_running 指标 + 振荡防护 + 6 项 SMP 前置条件加固 + init 迁移保护 + idle→idle #PF 修复 + smp_stress 验证) | 2 天 | 07-29 |
| **EEVDF 调度器** (rbtree 可运行队列 + vruntime/deadline + pick_eevdf O(log n) + per-CPU TSS SMP 修复) | 2 天 | 07-25 |

## 2026-07-24

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **select/pselect syscall** (poll_table 动态化 + do_poll_core 提取 + 适配层 + pselect6 sigmask 原子性；当时 systest 118/118，当前 126/126) | 2 天 | 07-24 |

## 2026-07-19

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **ext2 读写** (alloc_block/inode、create/mkdir/rmdir/unlink/rename/truncate、selftest) | 2 天 | 07-19 |
| refactor: 固定数组→堆分配 (VFS name/cwd + mount_table + pipe buf + ext2 buf) | 1 天 | 07-19 |

## 2026-07-18

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **poll syscall** (poll_table + 双队列 wake + pipe/tty/devfs + PIT timeout) | 2 天 | 07-18 |
| Makefile QEMU targets 统一 AHCI (run/run-kvm/debug) | 10 分钟 | 07-18 |

## 2026-07-15 ~ 07-17

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **busybox ash 方向键+行编辑** (VT100 CSI + terminal + FIONREAD) | 2 天 | 07-17 |
| arch/x86_64 头文件引用清理 | 1 小时 | 07-17 |
| **多架构清理收尾** (8 dispatch + 7 aarch64 桩) | 1 天 | 07-15 |

## 2026-07-12

| 项目 | 工作量 | 日期 |
|------|--------|------|
| 日志级别系统 | 5 天 | 07-12 |
| 子系统注册框架 | 半天 | 07-12 |
| arch 通用头文件迁移 | 2 天 | 07-12 |

## 2026-07-11

| 项目 | 工作量 | 日期 |
|------|--------|------|
| 内核栈 canary (SSP) | 30 分钟 | 07-11 |
| SMP 栈溢出修复 (ext2 buf[256]→buf[4096]) | 30 分钟 | 07-11 |
| selftest 10/10 + systest 70/70 修复 | 2 小时 | 07-11 |
| disk.img GPT 双分区 + tools/mkdisk | 1 天 | 07-11 |
| ext2 只读 + GPT + tmpfs + /dev 块设备 | 1 天 | 07-11 |
| VFS mount point getdents 修复 | 半天 | 07-11 |
| **COW fork (4KB-only)** | 2 天 | 07-11 |
| 4KB 页面 + VMA + mmap/mprotect | 3 天 | 07-08 |

## 2026-07-02 ~ 07-05

| 项目 | 工作量 | 日期 |
|------|--------|------|
| systest 70/70 | 1 天 | 07-05 |
| 信号 handler 用户态投递 | 2 天 | 07-05 |
| busybox ash shell + 9 applet | 1 天 | 07-04 |
| FPU/SSE 状态保存 (fxrstor/fsave) | 半天 | 07-03 |
| do_signal_delivery (Ctrl-C→SIGINT) | 1 天 | 07-03 |
| syscall + signal 框架 (systest 43 syscall) | 2 天 | 07-02 |

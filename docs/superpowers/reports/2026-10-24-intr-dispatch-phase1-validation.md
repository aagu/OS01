# 中断/异常分派统一第一期 — 验证报告（2026-10-24）

> **执行日期说明**：plan Task 7 要求「实际执行跨日时采用当天日期」。plan 与 spec 的文件名保持 `2026-10-23`，本**验证报告**采用实际执行日 **2026-10-24**（本地时区 UTC+8）。全部归档目录的时间戳是 UTC（`20261024T04…Z`），对应本地 `2026-10-24 12:xx`。

> **本报告的验收对象**：分支 `feature/intr-dispatch-phase1` 的 HEAD `f4a2bc04`。**本次验收矩阵**产出的归档（§2、§3、§4 表格里逐条列出的那些目录）的 `result.json` 都记录 `git_revision=f4a2bc04265435da7ab84d2abd953c3950430016`、`git_dirty=false`，即验收运行期间工作树干净。
>
> **三类归档不在上面这句的范围内，本报告也**不**引用它们**：(a) §6.1 的**对照实验**归档 `aarch64-m3-probe/20261024T042326Z-…`——那一次运行是**故意**临时改过 `qemutests/aarch64_m3_probe.py` 的期望前缀，因此它的 `git_dirty=true` 是有意的；(b) `build/*/logs/tests/**` 下**更早迭代**的归档（例如 `aarch64-uefi-smp/20261023T183*`、`driver-model-boundary/20261023T1554*` 等）——它们来自 Tasks 1–6 的开发期运行，`git_revision` 是当时的中间提交、`git_dirty=true`，与本次验收矩阵无关，只是恰好同处一个日志目录；(c) **review 修复后重跑** `test-static` / `test-aarch64-audit` 产生的 `20261024T043*` 归档（15 个，`git_revision=01a24293`、`git_dirty=true`——重跑时工作树里有未提交的文档改动）——它们是本任务**后续**轮次的复核记录，不属于第 1 节的验收矩阵。

> **本任务不新增任何运行期功能**：它只跑验收矩阵、记录证据、写文档。唯一的例外触发条件（「验收暴露了一个必须修的产品缺陷」）没有被触发——见 §6。

---

## 1. 环境、构建与范围

| 项 | 值 |
|----|----|
| Worktree | `/home/aagu/OS01/.claude/worktrees/intr-dispatch-phase1` |
| 分支 | `feature/intr-dispatch-phase1`（未切换分支、未创建新 worktree） |
| 最终 revision | `f4a2bc04265435da7ab84d2abd953c3950430016` |
| 提交标题 | `test(intr): give each indirect-form fixture class its own form` |
| Commit 范围 | 调查基线 `abb89b03` → HEAD，共 10 个 commit（8 个实现：`a20e10ee`/`fed4028b`/`5e085f34`/`4eff2550`/`6592b684`/`935213ca`/`204dfa68`/`f4a2bc04` + spec `7471981e` + plan `c94a4a91`）；`master` 与本分支的 merge-base 是 `c94a4a91`，即 `master` 上已有 spec/plan，本分支在 `master` 之上是这 8 个实现 commit；本任务再加 1 个文档 commit |
| Dirty 状态 | `git status --porcelain` 空（干净）；**本次验收矩阵的各归档** `git_dirty=false`（归档目录逐条列在 §2/§3/§4 表格里；不在这些表格里的归档——对照实验、更早迭代、review 修复后的重跑——见开头引言的排除说明） |
| 编译器 | `clang` 23.1.3（`x86_64-pc-linux-gnu`）；链接器 `ld.lld` |
| 构建 | GNU Make 4.4.1 |
| 模拟器 | QEMU 11.1.2（`qemu-system-x86_64` 与 `qemu-system-aarch64` 同版本） |
| Python | 3.14.8（`unittest` 驱动的 host/harness 套件） |
| 宿主平台 | Linux 7.2.9-arch1-1 x86_64 |
| Profile | `x86_64-clang` / `aarch64-clang` |
| Variant | **默认 variant**：`INITTAB_FILE=config/inittab`、`LOG_TARGET=serial`、`ARCH9_FAULT=none`、`FB_RESOLUTION_TEST=0`、**`DEBUG_CHANNELS` 未设置**；构建默认 `SMP=2`（`Makefile:41`），但**每条命令实际使用的 `-smp` 值见 §2 / §3 表格**（构建默认并不等于运行拓扑） |
| 本地日志目录 | `/tmp/intr-task7-logs/`（本机临时目录，不入库） |
| 归档目录 | `build/x86_64-clang/logs/tests/**` 与 `build/aarch64-clang/logs/tests/**`（profile build 目录内，不入库） |

### 1.1 无破坏性注入 / 无临时排除的确认

| 检查项 | 结果 |
|--------|------|
| `kernel/Makefile` 中的 Task 2 临时排除 `$(filter-out intr/dispatch.c, …)` | **不存在**（`grep -n "filter-out intr/dispatch" kernel/Makefile` 无匹配；Task 4 已删除） |
| x86 构建包含 `intr/dispatch.c` | ✅ `build/x86_64-clang/kernel/intr/dispatch.o` 存在（`kernel/Makefile` 的 `ARCHDIR` wildcard 收集 `arch/x86_64/intr/dispatch.c`） |
| aarch64 构建包含 `intr/dispatch.c` | ✅ `build/aarch64-clang/kernel/intr/dispatch.o` + `arch/aarch64/intr/dispatch.o` + `arch/aarch64/intr/bad_event.o` 存在 |
| 四个公共符号强定义唯一 | ✅ `llvm-nm` 结果：`arch_intr_dispatch` / `arch_intr_handle_sync` / `arch_intr_handle_irq` / `arch_intr_bad_event` 均为 `T`，无 weak、无重复定义（`intr/dispatch.o` 里 `arch_intr_handle_sync|irq|bad_event` 为 `U`，由各架构后端对象提供 `T`） |
| `DEBUG_CHANNELS` / `FB_RESOLUTION_TEST` / `ARCH9_FAULT` 注入 | 未启用（`ARCH9_FAULT=none`、`FB_RESOLUTION_TEST=0`；submake 环境行里没有 `DEBUG_CHANNELS`） |
| 临时调试插桩 | 无（本次提交只改文档；见 §8「Files changed」） |

### 1.2 双架构正常构建

| 命令 | 退出码 | 日志 |
|------|--------|------|
| `make PROFILE=x86_64-clang kernel.bin` | **0** | `/tmp/intr-task7-logs/build-x86.log` |
| `make PROFILE=aarch64-clang aarch64-uefi` | **0** | `/tmp/intr-task7-logs/build-aarch64.log` |

---

## 2. x86_64 验收矩阵：命令 → 退出码 → 日志路径

`-smp` 取自各归档 `result.json` 的 `argv`（QEMU 实际拓扑，不是构建默认值）。

| # | 命令 | `-smp` | 退出码 | 日志路径 | 摘要 |
|---|------|--------|--------|----------|------|
| 1 | `make PROFILE=x86_64-clang test-host` | 无 QEMU | **0** | `/tmp/intr-task7-logs/test-host.log` | `[run_hosttests] 134 binary(ies) run, overall exit 0` |
| 2 | `make PROFILE=x86_64-clang test-static` | 无 QEMU | **0** | `/tmp/intr-task7-logs/test-static.log` | 13 项静态审计全 PASS（`runtime-audit` / `stack-canary-audit` / `validate-kernel` / `runtime-link-order` / `kernel-runtime-link` / `syscall-boundary` / `kernel-layout` / `kernel-canary-contract` / `header-object-audit` / `stack-frame-audit` / `driver-model-boundary` / `gui-build-audit` / `intr-dispatch-audit`），另加 `PASS: user-canary`（共 14 条 PASS） |
| 3 | `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | **1** | **0** | `/tmp/intr-task7-logs/test-qemu-phase0.log`；归档 `build/x86_64-clang/logs/tests/phase-0/20261024T040849Z-83cb9755b0174ad196e587fe6b509932/` | `PASS: Kernel booted, boot markers verified, shell prompt appeared` |
| 4 | `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` | **1** | **0** | `/tmp/intr-task7-logs/test-syscall.log`；归档 `build/x86_64-clang/logs/tests/systest/20261024T040902Z-e1b21e7503974f58b9cc8ecfe744d8eb/` | `PASS: all 99 syscall tests passed`（`total=100 passed=99 failed=0 skipped=1`） |
| 5 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest` | **4** | **0** | `/tmp/intr-task7-logs/test-kernel-selftest.log`；归档 `build/x86_64-clang/logs/tests/kernel-selftest/20261024T040956Z-fde7307b67f14866a63496cf78d52a80/` | `[TEST] END suite=kernel-selftest total=39 passed=39 failed=0 skipped=0` |
| 6 | `make PROFILE=x86_64-clang test-harness` | 各子 suite 自带（见各归档 argv） | **0** | `/tmp/intr-task7-logs/test-harness.log` | `Ran 678 tests in 557.470s` → `OK (skipped=1)`（约 9–10 分钟，未中断） |

> `-smp` 的来源：`phase-0` 与 `test-syscall` 的 harness 固定 `-smp 1`；`test-kernel-selftest` 由 `mk/components/run.mk:400` 的 `KERNEL_SELFTEST_SMP ?= 4` 决定，故为 `-smp 4`。**结论行以各归档 `argv` 为准**。

**#4 与 #5 分开运行**，全程没有同时设置 `OS01_SYSTEST=1` 与 `KERNEL_SELFTEST=1`（AGENTS.md 明确禁止：内核 selftest 会在 boot 时 spawn kthread，破坏 systest 的 fork+exec+waitpid 用例）。

**关于 #6 日志中的 `FAIL:` / `ERROR:` 行**：这些全部来自 harness **自身的 20 个 unittest 模块**（`qemutests.test_systest_protocol`、`test_kernel_selftest_result`、`test_gui_runner`、`test_resolution_switcher`、`test_systest_repeat_harness` 等）——这些模块**故意**把残缺/畸形/重复/超时的合成输入喂给各个 runner 与解析器，断言 runner **必须**把它们判成失败。因此日志里出现的 `FAIL: systest protocol: missing END line`、`ERROR: kernel selftest: intr-dispatch evidence: case uaccess was not SELECTed`、`[gui] SMP=1 FAIL: [boot/handshake] boot timeout before shell prompt`、`[gui] SMP=1 FAIL: QMP screendump failed …: simulated QMP screendump failure` 等是被**期望**的失败检出，不是本阶段引入的回归（同一批场景在另一次矩阵迭代里就是 PASS，例如 `input_owner` 在 FAIL 之后的一次迭代为 `PASS`）。套件自身汇总为 `Ran 678 tests … OK (skipped=1)`，退出码 0。

### 2.1 按 case 名核对的结果（不固定旧测试总数）

#### 用户信号终止（systest，归档 `.../systest/20261024T040902Z-…/stdout.log`）

| Case | 期望 | 实测 | 判定 |
|------|------|------|------|
| `54b_user_fault_divzero` | #DE → SIGFPE | `do_divide_error from user, killing task (signal=8)`；`[PASS] 54b_fault_divzero (status=0x8)` | ✅ |
| `54c_user_fault_write_ro` | 非法写只读页 → SIGSEGV | 连续 **8** 次子进程（pid 96–103）全部 `Killing task N (user fault at RIP=0x40D873, signal=11)`，`[PASS] 54c_fault_write_ro (pid=N waited=N errno=0 status=0xb)` | ✅ |
| `54d_user_fault_ud2` | #UD → SIGILL | `do_undefined_opcode from user, killing task (signal=4)`；`[PASS] 54d_fault_ud2 (status=0x4)` | ✅ |
| `54e_user_fault_gp` | #GP → SIGSEGV | `do_general_protection(13) from user, killing task 105`；`[PASS] 54e_fault_gp (pid=105 waited=105 status=0xb)`；随后 `[PASS] 54e_parent_continues (pid=106 waited=106 status=0x700)`（父任务存活） | ✅ |

`54c` 的历史 flake（`docs/changelog.md` 2026-10-23 的 zombie-publication 竞态，偶发 `status=0`）**本轮未复现**：8/8 子进程都是 `status=0xb`。

#### COW / mmap / 保护页（systest）

| Case | 实测 | 判定 |
|------|------|------|
| `51_fork_brk_isolation` | `[PASS] fork_brk_child_writes`（子进程 COW 写产生自己的字节）+ `[PASS] fork_brk_parent_unchanged` | ✅ |
| `52_fork_mmap_cow_isolation` | `[PASS] fork_mmap_child_writes` + `[PASS] fork_mmap_parent_unchanged` | ✅ |
| `53_cow_kernel_outputs` | r0–r2 × p0–p3 共 18 项 `cow_output_*` 子断言全 PASS | ✅ |
| `50_protected_ranges` | PASS（保护页范围内写入被拒） | ✅ |
| `49_brk_read_fresh_page` | PASS | ✅ |
| mmap 系列（`auto_mmap_outside_reserve`、`heap_vma_intact`、`getrandom (1MiB mmap)`） | 全 PASS | ✅ |

#### `uaccess` 与 `msr_safe`（kernel-selftest，归档 `.../kernel-selftest/20261024T040956Z-…/stdout.log`）

| Case | 实测 | 判定 |
|------|------|------|
| `uaccess` | `[selftest] uaccess: walker OK` / `address-math OK` / `unmapped output rejected` / `cross-page OK` / `_ft_res OK` / `strnlen_user OK` / **`[selftest] uaccess: dispatch longjmp restore OK`** / `[selftest] uaccess: PASS` | ✅ |
| `msr_safe` | `[selftest] msr_safe: 6 passed, 0 failed` | ✅ |

**Task 5 的硬门槛标记**：`grep -c "dispatch longjmp restore OK"` = **1**（恰一次，无重复、无缺失、无「只有总体 PASS」）。该路径是真实 guest 的 #PF → `do_page_fault` 薄入口 → `arch_intr_dispatch()` → x86 私有 #PF handler → `__builtin_longjmp`；Task 6 的调用链审计另行在目标对象上核对了这条接线（见 §5.2）。

#### systest 的 1 个 SKIP

`[TEST] SKIP sendto_user_boundaries reason=no_udp_socket`。原因是**环境**：systest 的 QEMU 命令行不带任何网卡设备（`result.json` 的 argv 里没有 `-netdev`/`-device e1000`），guest 日志第 28 行 `net: no network devices registered, skipping stack start`，因此没有 UDP socket 可用。这是 harness 现有网络配置的既有限制（与 spec §9.3 提到的 UDP setup 属同一类环境前提），**不是**本次改动的回归，也未据此下调任何断言。

---

## 3. aarch64 验收矩阵

| # | 命令 | `-smp` | 退出码 | 日志路径 | 摘要 |
|---|------|--------|--------|----------|------|
| 1 | `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | **1** | **0** | `/tmp/intr-task7-logs/aarch64-sync-fault.log`；归档 `build/aarch64-clang/logs/tests/aarch64-sync-fault/20261024T041948Z-ec20e1ba3e794950869e1fce673d0380/` | `{"result": "PASS", "fatal_observed": true, "timeout": false}` |
| 2 | `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | **1 / 2 / 4**（3 次重复） | **0** | `/tmp/intr-task7-logs/aarch64-smp.log`；归档 `build/aarch64-clang/logs/tests/aarch64-uefi-smp/` 下 9 个目录（cpus=1：`20261024T041957Z-8703b088…`、`20261024T042002Z-0c6ee0b8…`、`20261024T042007Z-3cbb6337…`；cpus=2：`20261024T042011Z-0fe1fc4d…`、`20261024T042017Z-024dff33…`、`20261024T042021Z-2a49827c…`；cpus=4：`20261024T042027Z-233068205…`、`20261024T042033Z-0bd9c6a8…`、`20261024T042039Z-c4b0da54…`） | **9/9 PASS**（cpus 1/2/4 × 3 次，全部 `"result": "PASS"`） |
| 3 | `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | **1** | **0** | `/tmp/intr-task7-logs/aarch64-gic-spi.log`；归档 `build/aarch64-clang/logs/tests/aarch64-gic-spi/20261024T042049Z-7c3ebd053ef9433984969c66e3156b19/` | `{"result": "PASS", "injected": true, "armed_count": 1, "handled_count": 1}` |
| 4 | `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | **2** | **2** | `/tmp/intr-task7-logs/aarch64-m3-probe.log`；归档 `build/aarch64-clang/logs/tests/aarch64-m3-probe/20261024T042053Z-cd2ee22e773f4534874395547a4b240f/` | **FAIL（timeout）** — 详见 §6.1 |
| 5 | `make PROFILE=aarch64-clang test-aarch64-audit` | 无 QEMU（静态审计） | **0** | `/tmp/intr-task7-logs/aarch64-audit.log`；归档 `build/aarch64-clang/logs/tests/intr-dispatch-audit/20261024T042334Z-dbe4ce52c5da48f89abfc8e6612bfd5f/` | `[audit] OK (vma_*/uaccess_*/fork_* symbol-exclusion gate only)` + `PASS: intr-dispatch-audit` |

### 3.1 `MODE=sync-fault`：原字段 / 首报 / 无 returned

guest 日志尾部（唯一一次 FATAL）：

```
[aarch64-sync-test] armed
[aarch64-sync] FATAL mpidr=0x0000000080000000 ec=0x25 esr=0x0000000096000004 elr=0xffff000040099844 spsr=0x00000000600003c5 far=0xffff800000000000
```

- **原字段**：`mpidr` / `ec` / `esr` / `elr` / `spsr` / `far` 六个字段、顺序、宽度、大小写与 spec §8 要求的原格式一致（`ec=0x25` = data abort lower EL）。
- **首报协议**：`[aarch64-sync-test] armed` 之后**恰一次** FATAL（`grep -c "FATAL"` = 1），无第二份首报。
- **无 returned**：`grep -c "returned"` = **0**。
- runner（`qemutests/aarch64_sync_fault.py`）自身对这些性质做了正反例自检（重复 FATAL、`returned` 标记、FATAL 之后的 tick/selftest/SMP/phase1 证据、`far=n/a` 与 `far` 宽度等一律判失败），因此该 PASS 是**带断言**的 PASS，不是「看到一行就算过」。
- `far=0xffff800000000000` 是本次受控探针的合法 FAR（FnV 有效），不是 `far=n/a`。

### 3.2 `MODE=smp`：1/2/4 CPU × 3 次

9 个 case 全部 `"result": "PASS"`、`"timeout": false`、`"complete": true`、`"returncode": 0`：

| cpus | run 1 | run 2 | run 3 |
|------|-------|-------|-------|
| 1 | PASS | PASS | PASS |
| 2 | PASS | PASS | PASS |
| 4 | PASS | PASS | PASS |

每个 case 内的既有断言均满足（取 4-CPU 归档 `20261024T042039Z-c4b0da5488a24192a01aaf44909f76ed/stdout.log`）：`[smp] requested=4 online=4 status=PASS`、`[spinlock] active=4 iterations=1000000 total=4000000 status=PASS`、`[ipi] summary targets=3 received=3 status=PASS`、`M1 AP PASS cpu=1/2/3`、`[selftest] m3: 5/5 PASS`（含 `4k_round`/`2m_block`/`split_2m`/`kernel_map_pinned`）、`[selftest] m3mc: 5/5 PASS`（含 `target_set`/`partial_mask`/`pt_lock_interleave`/`5b`/`shared_l1`）、`[selftest] slab: 16/16 PASS`。**timer / SGI / Slab / M3 / multicore 未退化。**

### 3.3 `MODE=gic-spi`：真实 SPI 分派未退化

`{"result": "PASS", "injected": true, "armed_count": 1, "handled_count": 1, "died_before_bind": false}` —— 外设 SPI 仍被正确分派并处理一次，无中断风暴、无漏中断。

### 3.4 GIC EOI 归属（不新增断言）

spec 要求「spurious 无 EOI、普通 GIC 完整 IAR 恰一次 EOI」由**既有生产 GIC host tests** 与 **Task 6 调用链审计**共同证明，本任务不新增断言。证据：

- **生产 host test**：`hosttests/cases/test_gic_driver.c`（`test-host` 中 `PASS: test_gic_driver`）——
  - `gic_eoi()` 把完整 IAR 原样写回 EOIR（含 CPUID 位，`assert_eq(0xC07, rd(gicc_mock, M_GICC_EOIR))`）；
  - **spurious**（IAR = 1023）：不写 EOIR、不调 handler、不触发 unexpected（`assert_eq(0, rd(gicc_mock, M_GICC_EOIR))`）；
  - 正常 INTID：handler 之后 **恰一次** EOI（`assert_eq(35, rd(gicc_mock, M_GICC_EOIR))`）。
- **调用链审计**（aarch64 真实 ELF，`build/aarch64-clang/logs/tests/intr-dispatch-audit/20261024T042334Z-…/stdout.log`）：
  `[intr-dispatch-audit] arch_intr_handle_irq -> gic_dev_dispatch exactly once; no direct gic IAR/EOI` —— 共同分派层没有预读 IAR、没有二次 EOI、没有提前开 IRQ。

---

## 4. 调用链审计对两个真实 ELF 的判定

Task 6 的 `qemutests/intr_dispatch_audit.py` 直接审计**生产目标对象**（不是影子实现）。

### 4.1 x86_64（`build/x86_64-clang/logs/tests/intr-dispatch-audit/20261024T040836Z-4b945478314541e681caef40d69f689b/stdout.log`）

> 该归档由 **`test-static`**（§2 表第 2 行）内的 `run_static_audit.py --suite intr-dispatch-audit` 产生（归档时间戳 12:08:36，`test-static.log:114` 是它的命令行）。`test-harness`（§2 表第 6 行）也跑了同一个审计脚本，其**日志内**同样出现 x86 真实 ELF 的判定（`strong def arch_intr_dispatch @ 0xffff80000013cd80 …`、`[intr-dispatch-audit] OK`），但没有另建归档目录——两处判定一致。

| 审计项 | 结果 |
|--------|------|
| `arch_intr_dispatch` 强定义 | `T`，唯一，无 weak |
| `arch_intr_handle_sync` / `arch_intr_handle_irq` / `arch_intr_bad_event` 强定义 | `T`，各自唯一，无 weak |
| 分派链 | `arch_intr_dispatch -> arch_intr_handle_sync (jmp)`、`-> arch_intr_handle_irq (jmp)`、`-> arch_intr_bad_event (call)` |
| 入口确实经过共同函数 | `stub divide_error -> do_divide_error`；`do_divide_error -> x86_intr_decode` + `-> arch_intr_dispatch`；`stub undefined_opcode -> do_undefined_opcode` 同上；`stub general_protection -> do_general_protection` 同上；`stub page_fault -> do_page_fault` 同上 |
| x86 IRQ 后端 fail-closed | `x86 arch_intr_handle_irq fail-closed (arch_intr_bad_event only)` |
| #PF 的 CR2 单次快照 | `#PF chain CR2 snapshot: exactly one read (do_page_fault); the retained #GP fatal diagnostic read is outside this chain` |
| 业务体未私有化 | `arch_intr_handle_sync -> x86_handle_page_fault` / `-> x86_handle_undefined_opcode`（私有 handler 直调，入口不递归） |
| 源码事实核对 | `#GP retained CR2 diagnostic, copy_from_user_ft_res failure branch and the Task 5 probe path all present` |
| 汇总 | `[intr-dispatch-audit] OK` |

### 4.2 aarch64（`build/aarch64-clang/logs/tests/intr-dispatch-audit/20261024T042334Z-dbe4ce52c5da48f89abfc8e6612bfd5f/stdout.log`）

| 审计项 | 结果 |
|--------|------|
| 向量表 | `vector table base 0xffff0000400a3000 (2 KiB aligned)`；`vector slot +0x200 -> el1_sync_entry`；`vector slot +0x280 -> el1_irq_entry`；`el1_sync_entry frame: 272 bytes (34*8)` |
| 入口确实经过共同函数 | `edge el1_sync_entry -> aarch64_el1_sync (bl)` → `aarch64_el1_sync -> aarch64_intr_decode_sync` + `-> arch_intr_dispatch` + `-> arch_intr_bad_event` |
| 四个公共符号强定义唯一 | 全部 `T`，唯一，无 weak |
| 业务体交接 | `arch_intr_handle_sync -> aarch64_intr_decode_sync`、`-> aarch64_el1_sync_fatal`（原 fatal 仍拿原始 ESR/FAR） |
| IRQ 归属 | `arch_intr_handle_irq -> gic_dev_dispatch exactly once; no direct gic IAR/EOI` |
| bad-event 早期路径 | `arch_intr_bad_event -> arch_early_puts` / `-> arch_early_putc`（无普通 logger） |
| 源码事实核对 | `PT_REGS_SIZE (34*8), .balign 0x800 and the el1_sync_entry -> aarch64_el1_sync call pinned` |
| 汇总 | `[intr-dispatch-audit] OK` |

**两个真实 ELF 的判定都是 `OK`。**

---

## 5. spec §1–11 → 实施任务 映射（自查）

| Spec 章节 | 要求要点 | 交付它的任务 | 本次验收的证据 |
|-----------|----------|--------------|----------------|
| §1 意图与成功标准 | 六条真实入口经过共同 `arch_intr_dispatch()`；保留原诊断/恢复/终止 | Tasks 1–6 | §2.1、§3、§4 |
| §2 已核实现状 | 调查基准；`docs/arch.md` 的 3065 行不是工作量依据 | Task 7（文档） | `docs/arch.md` 已改为 1189 行并标注过时 |
| §3 方案比较 | 选定「共同事件入口 + 每架构后端、有限迁移」 | Tasks 1–4 | §4 审计证明走的是共同入口 |
| §4 范围与不做的部分 | 只迁六条；不接 EL0/SVC、不做用户页表/ASID/COW/上下文切换/统一 kernel_main | Tasks 3–4、7 | §7「已完成 vs 剩余」；`docs/roadmap.md` 行已逐项列明剩余 |
| §5.1 共同接口 | 事件结构字段、`void arch_intr_dispatch()`、事件生命周期、无效地址规范 | Task 1 | `kernel/include/intr/dispatch.h`；host `test_intr_dispatch` PASS |
| §5.2 架构后端 | 三个强定义后端、无 weak no-op、bad-event 契约、x86 IRQ fail-closed | Task 2、Task 4 | §4 审计（强定义唯一、`x86 arch_intr_handle_irq fail-closed`） |
| §6 解码与真实调用链 | x86 四向量的 reason/access 规则、CS 定 origin、CR2 单次快照；aarch64 ESR.EC 分类、SPSR 模式校验、FAR 真值源复用 | Task 1、Task 3–4 | `test_intr_decode_x86` / `test_intr_decode_aarch64` PASS；§4 的 CR2 单次快照与 frame 272 字节审计 |
| §7 四种控制流与无尾部资源 | 正常返回 / 故障恢复 / 用户任务终止 / 内核致命停机；共同层不持有需清理资源 | Task 2、4–6 | §2.1 的四个信号 case、`uaccess` 的真实 longjmp 恢复、aarch64 FATAL |
| §8 原诊断与 ABI/log 边界 | aarch64 FATAL 字段/顺序/首报/`far=n/a` 逐字保持；不改 boot_context/UAPI/pt_regs | Task 3–4、6–7 | §3.1 的 FATAL 行逐字段比对 |
| §9.1 host 与编译边界 | 解码表驱动、dispatcher spy、四后端路由、双架构强定义唯一、真实对象审计 | Task 1–6 | §2.1（134 host binary）、§4 |
| §9.2 aarch64 QEMU | sync-fault / smp / gic-spi / m3-probe / audit 五条 | Task 3、6–7 | §3（m3-probe 失败，见 §6.1） |
| §9.3 x86_64 与回归 | phase-0 / systest / kernel-selftest / host / static；真实 guest 非局部恢复硬门槛 | Task 4–7 | §2 全部 |
| §10 预计变更位置 | 文件职责表 | Tasks 3–4、7 | §8「Files changed」 |
| §11 实施者选择与后续项目 | 可改私有命名/fixture 组织；不得改入口覆盖、强链接、信号映射、控制流、EOI 归属、验收强度；后续需单独设计的项目 | Tasks 1–6、7 | §4 审计 + §2.1 逐 case 核对；后续项目清单见 §7.2 |

**结论**：spec §1–11 的每条要求都能映射到具体任务与本报告的具体证据；**没有**任何一条被跳过。

---

## 6. 失败与限制（如实记录，未把失败写成已闭环）

### 6.1 `MODE=m3-probe` 失败 —— **既有的 test 侧命名漂移，不是本阶段的回归**

**现象**（首跑，归档 `build/aarch64-clang/logs/tests/aarch64-m3-probe/20261024T042053Z-cd2ee22e773f4534874395547a4b240f/`）：

```
{"event": "case", "result": "FAIL", "timeout": true, "returncode": 0, "ok_observed": false, ...}
```

guest 串口日志里探针本身是正常的：

```
SHOOTDOWN-PROBE: START
SHOOTDOWN-PROBE: OK
```

**根因（静态证据）**：runner `qemutests/aarch64_m3_probe.py` 匹配的是 `M3-SHOOTDOWN-PROBE:`（`_START_RE`/`_OK_RE`/`_FAIL_RE`/`_SKIP_RE` 四个正则，以及 `_START_PREFIX` 等常量），而内核 `kernel/include/arch/aarch64/boot/shootdown_probe.h` 打印的是 `SHOOTDOWN-PROBE:`（`AARCH64_SHOOTDOWN_PROBE_CPUTS(ops, "SHOOTDOWN-PROBE: START\n")` / `"SHOOTDOWN-PROBE: OK\n"`）。

这个改名来自 commit **`045cba3f` "refactor(aarch64): drop M0/M1/M3 milestone naming from MM/VMM sources"（2026-10-05）**——该 commit 把内核侧 wire 标记从 `M3-SHOOTDOWN-PROBE:` 改成 `SHOOTDOWN-PROBE:`，但没有同步 runner（commit message 只点名保留了 `[selftest] m3:` / `[selftest] m3mc:` 这一对 wire 契约，漏掉了 shootdown probe 这一对）。

**基线判定**：

| 事实 | 证据 |
|------|------|
| `045cba3f` 是 `master` 的祖先 | `git merge-base --is-ancestor 045cba3f master` → 成功 |
| `045cba3f` 是本阶段基线 `abb89b03` 的祖先 | `git merge-base --is-ancestor 045cba3f abb89b03` → 成功 |
| 本阶段 8 个 commit（`7471981e`…`f4a2bc04`）**没有**碰过这两个文件 | `git log --oneline abb89b03..HEAD` 与 `git log -- qemutests/aarch64_m3_probe.py`、`git log -- kernel/include/arch/aarch64/boot/shootdown_probe.h` |

即：在**本阶段开始之前**、在 `master` 上，内核标记与 runner 期望就已经不一致，`MODE=m3-probe` 就已经失败。**这不是中断分派统一引入的回归。**

**运行时对照实验**（用来证明探针本身没坏、且失败点只在 test 侧前缀）：临时把 runner 的四条正则的期望前缀从 `M3-SHOOTDOWN-PROBE:` 改成内核实际的 `SHOOTDOWN-PROBE:`，**同一配置、同一棵树**重跑：

```
{"event": "case", "result": "PASS", "timeout": false, "returncode": 0, "ok_observed": true, ...}
```

（归档 `build/aarch64-clang/logs/tests/aarch64-m3-probe/20261024T042326Z-7c35463e506846db866708f5bc3ceb61/`，日志 `/tmp/intr-task7-logs/aarch64-m3-probe-prefixexperiment.log`。）随后**已用 `git checkout -- qemutests/aarch64_m3_probe.py` 把 runner 原样还原**（`git status --porcelain` 干净，文件回到 98–101 行的原始正则）。

**处理方式**：本任务**没有**修这个 runner（那属于另一个改动，且 plan 明确「不得修改生产内核行为」「不降低断言」），也**没有**把它写成通过。`MODE=m3-probe` 在本报告中**记为失败**。修法建议（留给 controller/后续任务，二选一）：把 runner 的期望前缀改回 `M3-SHOOTDOWN-PROBE:`，或把内核标记改回 `M3-` 前缀——两者都会重新耦合 kernel 与 runner，需要独立决定。**注意**：spec §9.2 明确「不接受 SKIP/FAIL」，所以这一项在本期范围内**尚未闭环**。

### 6.2 `sendto_user_boundaries` SKIP —— 环境缺网卡

见 §2.1 末尾。不是失败，是 harness 网络配置的既有限制（systest 的 QEMU argv 不带网卡）。未下调断言。

### 6.3 本轮没有复现的已知 flake

`54c_user_fault_write_ro` 的历史偶发 `status=0`（2026-10-23 zombie-publication 竞态）本轮 **8/8 未复现**，全部 `status=0xb`。无需触发基线对照。

### 6.4 其它观察（不是失败）

- `test-host` 计数为 **134** 个 host binary（`test-static` 13 项静态审计、`test-harness` 678 项）。本报告**不固定旧测试总数**，只按 case 名核对（§2.1）。
- `test-harness` 用时 557 s（≈9.3 分钟），在预期的 9–10 分钟区间内。

---

## 7. 已完成 vs 剩余（范围收尾）

### 7.1 本期已完成

- arch-neutral 无状态事件分派：`kernel/include/intr/dispatch.h` + `kernel/intr/dispatch.c::arch_intr_dispatch()`。
- 架构后端契约（强定义、无 weak no-op）：`arch_intr_handle_sync` / `arch_intr_handle_irq` / `arch_intr_bad_event`，含 `[intr-dispatch] FATAL bad-event` 早期停机路径。
- 纯解码器：`x86_intr_decode`（四向量 + CS origin + #PF read/write/exec + 地址 0）、`aarch64_intr_decode_sync` / `aarch64_intr_decode_irq`。
- **已统一的六条入口**：aarch64 EL1h sync、aarch64 EL1h IRQ、x86 #DE、x86 #UD、x86 #GP、x86 #PF。
- 四种控制流（正常返回 / 故障恢复 / 用户任务终止 / 内核致命停机）在真实 guest 上各有证据。
- 真实 guest 非局部恢复硬门槛：`uaccess` selftest 的 `[selftest] uaccess: dispatch longjmp restore OK`（恰一次）。
- 调用链审计工具 `qemutests/intr_dispatch_audit.py`，对 x86 与 aarch64 两个真实 ELF 均判定 `OK`。

### 7.2 本期**未**完成（必须继续待实施）

| 项 | 状态 |
|----|------|
| 其余 x86 异常（NMI、#DF、#MC 等） | 未做 |
| x86 IRQ / LAPIC timer / IPI / softirq 的汇编入口迁移 | 未做（x86 IRQ 后端只是 fail-closed 的链接完整性占位） |
| EL0 / SVC 异常与用户态入口 | 未做 |
| aarch64 可恢复 abort / uaccess 恢复路径 | 未做 |
| 用户页表（aarch64） | 未做 |
| ASID / COW（aarch64 侧） | 未做 |
| 上下文切换（`arch_task_switch` / aarch64 `smp.c` 拆分） | 未做 |
| 统一 SMP 启动（Spec B） | 未做 |
| 统一 `kernel_main`（Spec C） | 未做 |
| Spec A / M4 状态 | **保持不变**（M4 仍未开始） |
| `MODE=m3-probe` 的 runner 前缀漂移 | 未修（见 §6.1） |

---

## 8. Files changed（Task 7）

| 文件 | 变更 |
|------|------|
| `docs/arch.md` | 仅更正 Spec A 那一行：过时的「`x86_64/trap.c` 3065 行」改为现值 1189 行，并标注第一期已统一的六条入口与剩余范围。**未改动其它架构文档。** |
| `docs/roadmap.md` | 仅改「中断/异常 dispatch 统一」一行：记录已统一内容（事件结构、共同 dispatcher、六条入口、四种控制流）与逐项列出的剩余范围；**Spec A / M4 状态保持不变**。 |
| `docs/changelog.md` | 新增 `## 2026-10-24` 条目（含 m3-probe 失败与基线判定的如实记录），并把头部「截至」日期改为 2026-10-24。 |
| `docs/superpowers/reports/2026-10-24-intr-dispatch-phase1-validation.md` | 本报告（新增）。 |

**没有**改动任何生产代码、测试代码或构建脚本。

---

## 9. 结论

- 双架构构建（x86 `kernel.bin`、aarch64 `aarch64-uefi`）退出码 0；树中**没有**破坏性注入 flag、没有临时排除、没有调试插桩。
- x86 验收矩阵 6 条命令**全部退出码 0**；按 case 名核对的用户信号（`54b`/`54c`/`54d`/`54e`）、COW/mmap、`uaccess`、`msr_safe` 全部 PASS；`uaccess` 的 Task 5 标记恰一次出现。
- aarch64 验收矩阵：`sync-fault` PASS（原字段 + 首报 + 无 returned）、`smp` 9/9 PASS、`gic-spi` PASS、`test-aarch64-audit` PASS；**`m3-probe` 失败**，已定位为 `045cba3f` 引入的、既有的 runner/内核 wire 标记命名漂移（`master` 与本阶段基线之上均已存在），并用「同配置把前缀对齐后即 PASS」的对照实验证明探针本身正常；本报告**不把它记为通过**。
- 调用链审计对 x86 与 aarch64 两个**真实** ELF 均判定 `OK`。
- spec §1–11 每条要求都有对应的任务与证据（§5）。
- 本期**没有**触碰生产内核行为；唯一未闭环的 gate（`MODE=m3-probe`）已如实记录并给出根因与两种修法建议。

**因此本期状态为「主体闭环 + 一项既有失败如实记录」，不是「全绿」。**

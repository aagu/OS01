# GUI-2 输入与焦点 — 验证报告（2026-10-22）

> **执行日期说明**：plan Task 13 要求把文件名与标题的日期从计划日 `2026-10-20` 调整为**实际执行日**。本报告的实际执行日是 **2026-10-22**（本地时区 UTC+8）。回归运行在 UTC 时间轴上跨越 `2026-10-21T20:26Z – 2026-10-21T20:42Z`，对应本地时间 `2026-10-22 04:26 – 04:42`；归档里的 `utc_started_at` 字段同样是 UTC，因此下文所有 UTC 时间戳都比本地日期早一天。

> **本报告的 §2 / §3 描述的是 Task 13 那一轮回归的树**（HEAD `093a5fb4`）。其后的**整分支修复波**改动了 30 个文件（其中包括 DESYNC 的错误契约），因此**最终树**的门禁结果记录在 **§7**，§5.3 里被这一波关掉的条目也已就地标注。

---

## 1. 交付范围与环境

| 项 | 值 |
|----|----|
| Worktree | `/home/aagu/OS01/.worktrees/gui-2-input-focus` |
| 分支 | `feat/gui-2-input-focus` |
| Commit 范围 | 基线 `6126504a` → 回归时的 HEAD `093a5fb4`（基线之上 **33** 个 commit） |
| 回归执行时的 HEAD | `093a5fb4`（`fix(gui): reset the leak path per probe and prove the owner terminal restore`） |
| Profile | `x86_64-clang` |
| 编译器 | `clang` 23.1.3，`--target=x86_64-unknown-none -ffreestanding -fno-builtin` |
| 链接器 | `ld.lld` 23.1.3（`/usr/bin/ld.lld`） |
| 构建 | GNU Make 4.4.1 |
| 模拟器 | QEMU 11.1.2 |
| 宿主测试框架 | Python 3.14.8（`unittest`） |
| LVGL | **v9.5.0**，pin 于 `85aa60d18b3d5e5588d7b247abf90198f07c8a63`（`thirdpart/lvgl.manifest` 的 `LVGL_REVISION`）；`git describe --tags` = `v9.5.0`；子模块**未被修改**（`git submodule status` 无 `+` 前缀） |
| 宿主平台 | Linux 7.2.9-arch1-1 x86_64 |

**LVGL 版本的两处读数**：`git submodule status` 打印的是 `v9.3.0-992-g85aa60d18`（离 HEAD 最近的**可达** tag），`git describe --tags` 在子模块内打印 `v9.5.0`；manifest 声明的是 "LVGL v9.5.0 input manifest"。两者指向同一个 commit `85aa60d18`。

---

## 2. 干净回归：命令 → 退出码 → 日志路径

`make clean` **最先**执行：本分支改过 `struct devfs_ops` 等内核结构体，而本仓库没有头文件依赖跟踪，陈旧目标文件会静默错配 `sizeof`。

全部命令都从**仓库根**的 make 入口运行（没有用过 `make -C hosttests`）。日志的完整输出保存在工作树的 `build/x86_64-clang/logs/regression-task13/` 下。

| # | 命令 | 退出码 | 日志路径（工作树相对） | 摘要 |
|---|------|--------|----------------------|------|
| 0 | `make clean` | **0** | `build/x86_64-clang/logs/regression-task13/00-make-clean.log` | 清空 `build/x86_64-clang` |
| 1 | `make PROFILE=x86_64-clang test-host` | **0** | `build/x86_64-clang/logs/regression-task13/01-test-host.log` | `[run_hosttests] 128 binary(ies) run, overall exit 0` |
| 2 | `make PROFILE=x86_64-clang test-static` | **0** | `build/x86_64-clang/logs/regression-task13/02-test-static.log` | 13 项静态审计全 PASS（见 §4.3） |
| 3 | `make PROFILE=x86_64-clang test-harness` | **0** | `build/x86_64-clang/logs/regression-task13/03-test-harness.log` | `Ran 582 tests in 527.636s` / `OK (skipped=1)` |
| 4 | `make PROFILE=x86_64-clang test-qemu SUITE=gui` | **0** | `build/x86_64-clang/logs/regression-task13/04-test-qemu-gui.log` | **24/24 PASS**（逐 SMP 见 §3） |
| 5 | `make PROFILE=x86_64-clang test-qemu SUITE=gfx` | **0** | `build/x86_64-clang/logs/regression-task13/05-test-qemu-gfx.log` | `result.json` status PASS |
| 6 | `make PROFILE=x86_64-clang test-qemu SUITE=resolution` | **2（失败）** | `build/x86_64-clang/logs/regression-task13/06-test-qemu-resolution.log` | `client_estale_no_diagnostic` + `client_estale_no_exit_status`，详见 §5.1 |
| 7 | `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` | **0** | `build/x86_64-clang/logs/regression-task13/07-test-syscall.log` | `[SYS TEST] RESULT: 342 passed, 0 failed` |

**为排查 §5.1 而追加的三次运行**（都是新失败驱动的新增运行，不是重复跑已通过的 suite）：

| 命令 | 退出码 | 日志路径 | 目的 |
|------|--------|---------|------|
| `make PROFILE=x86_64-clang test-qemu SUITE=resolution`（第二次） | **2** | `build/x86_64-clang/logs/regression-task13/06b-test-qemu-resolution-rerun.log` | 判定失败是确定性还是 flake → **确定性，同两条断言** |
| `make PROFILE=x86_64-clang test-gui-incremental` | **0** | `build/x86_64-clang/logs/regression-task13/08-test-gui-incremental.log` | 复核 Task 9 的增量/sysroot 证据，见 §4 |
| `make PROFILE=x86_64-clang test-qemu SUITE=resolution` @ **基线 `6126504a`** | **2** | `build/x86_64-clang/logs/regression-task13/BASE-test-qemu-resolution.log` | **同环境基线对照**，见 §5.1 |

基线运行在**临时导出树**里完成（`git archive 6126504a | tar -x -C /tmp/os01-base-6126504a`，子模块按相同 pin 复制、gitdir 独立复制、OVMF 用同一个本地 `OVMF.fd`）。**没有**创建额外 worktree 或分支，**没有**写入 `/home/aagu/OS01` 的主检出，**没有**改动共享仓库的子模块 git 目录（基线用的是它自己的副本）。

---

## 3. 各 suite 的实际 `result.json`（逐 SMP）

以下全部读自归档里的 `result.json`，**不是**终端尾部的 `PASS` 行。

### 3.1 `SUITE=gui` — 归档 `build/x86_64-clang/logs/tests/gui/20261021T203728Z-64f7593835e94593ae708717c4c0cd6d/`

`status = PASS`，`runner_exit_code = 0`，`outcomes` 共 **24** 条。

| SMP | 场景 | 结果 |
|-----|------|------|
| 1 | `boot/handshake` | PASS |
| 1 | `smoke` | PASS |
| 1 | `normal_exit` | PASS |
| 1 | `wait_quit` | PASS |
| 1 | `busy_owner` | PASS |
| 1 | `killed_owner` | PASS |
| 1 | `fatal_owner` | PASS |
| 1 | `input_interaction` | PASS |
| 1 | `input_isolation` | PASS |
| 1 | `input_crash` | PASS |
| 1 | `input_owner` | PASS |
| 1 | `input_overflow` | PASS |
| 2 | `boot/handshake` | PASS |
| 2 | `smoke` | PASS |
| 2 | `normal_exit` | PASS |
| 2 | `wait_quit` | PASS |
| 2 | `busy_owner` | PASS |
| 2 | `killed_owner` | PASS |
| 2 | `fatal_owner` | PASS |
| 2 | `input_interaction` | PASS |
| 2 | `input_isolation` | PASS |
| 2 | `input_crash` | PASS |
| 2 | `input_owner` | PASS |
| 2 | `input_overflow` | PASS |

逐 SMP 统计：**SMP 1 = 12 PASS / 0 FAIL；SMP 2 = 12 PASS / 0 FAIL。**

SMP 1 的输入场景在 `outcomes` 的 `reason` 字段里留下了可核对的证据（不是笼统的 "ok"）：

- `input_interaction`：`terminal pid=2 birth=2; probe pid=17; rapid clicks -> click_count=3; drag -> slider=56; wheel -> scroll 0 -> 32 -> 0; focus navigation ok; textarea=b'aA!'; edit -> textarea=b'aA'; caps matrix ok; ctrl-c during the lease ok; long press -> 3 characters; multi-line -> textarea2=b'b\n'; probe exit detail=interaction_complete; skipped records: 4 [seq=4/6/86/88 STATE modifiers=0]`
- `input_isolation`：`isolation probe pid=22; leak command typed during the lease (/tmp/gui_input_leak_48ad24a647c045a8); probe exit detail=isolation_complete; recovery canary RECOVER_aB3a6497cD; after the isolation lease: the screen is back to the terminal`
- `input_crash`：`ctrl_q: wait status=0, canary RECOVER_aB131033cD; sigkill: wait status=137, canary RECOVER_aBa75b44cD; fatal: wait status=1, canary RECOVER_aBfe868fcD`（三条崩溃路径各自带回终端，且 `after the sigkill crash: no ghost click in the fresh lease`）
- `input_owner`：`the second gfx client failed with EBUSY; the old probe lost the ownership (wait=1); the second gfx hold (pid=47) survived`
- `input_overflow`：`the queue overflowed after 32 batches: errno=75 detail=input_dispatch; overflow wait status=1; recovery canary RECOVER_aB1b0cc9cD`

`git_dirty = False` —— 这一轮 GUI suite 是在**干净**的工作树上跑的（`result.json` 记录）。

### 3.2 `SUITE=gfx` — 归档 `build/x86_64-clang/logs/tests/gfx/20261021T204043Z-c21ecba6d363448c813beb727c44b952/`

`status = PASS`，`runner_exit_code = 0`。该 suite 是**标记式**的（不是逐用例 outcome），`outcomes` 为 **0** 条；`stdout.log` 里有 4 条标记：`[GFX TEST] PASS marker observed`、`[TETRIS] SMOKE PASS marker observed`、`[TERM SCREEN TEST] PASS marker observed`、`[DESKTOP] SMOKE PASS marker observed`。`image_sha256_before == image_sha256_after`（suite 没有改动活动镜像）。

### 3.3 `SUITE=resolution` — 归档 `build/x86_64-clang/logs/tests/resolution/20261021T204052Z-b2548a73bc804ef999569e6a698d8ad8/`

`status = FAIL`，`runner_exit_code = 1`，`outcomes` 为 **0** 条（该 suite 不产出逐用例 outcome，失败明细走 stdout）。**失败**，细节见 §5.1。

### 3.4 `OS01_SYSTEST=1 test-syscall` — 归档 `build/x86_64-clang/logs/tests/systest/20261021T204147Z-dfda6c9d16d74d40b1e15e6e0740a33e/`

`status = PASS`，`runner_exit_code = 0`，`outcomes` 为 0 条；`stdout.log` 末行：`[SYS TEST] RESULT: 342 passed, 0 failed`，以及 `[TEST] END suite=systest total=99 passed=98 failed=0 skipped=1`。

---

## 4. Task 9 的增量构建 / sysroot 证据（复核）

### 4.1 增量契约

`make PROFILE=x86_64-clang test-gui-incremental` → **退出码 0**，四个阶段全部 PASS（`build/x86_64-clang/logs/regression-task13/08-test-gui-incremental.log`）：

| 阶段 | 断言 | 结果 |
|------|------|------|
| 1/4 `noop` | 源码没变时重复构建**不得**重编 | `gui incremental build audit: PASS`（5 个目标依赖 `gui_input.h`） |
| 2/4 `touch` | 触碰 `gui_input.h` **必须**重编依赖者并重新链接 | PASS |
| 3/4 `install` | install **必须**清掉 staging 下的孤儿文件 | PASS |
| 4/4 `publish` | 新 generation **必须**干净、旧 generation **必须**保留 | PASS（`[sysroot] published generation 1`） |

### 4.2 发布面

```
$ ls build/x86_64-clang/sysroot/usr/include/gui/
gui.h
gui_input.h
gui_platform.h
```

发布树**恰好 3 个文件**。整个已发布 sysroot 里**没有** `lv_*` 头、没有 `*_private.h`、没有 `driver/input_ps2.h`、没有 `driver/input_route.h`、没有 `gui_input_test*` 注入接口。

### 4.3 静态门禁

`make PROFILE=x86_64-clang test-static` 的 13 项审计全 PASS：`runtime-audit`、`stack-canary-audit`、`validate-kernel`、`runtime-link-order`、`kernel-runtime-link`、`syscall-boundary`、`kernel-layout`、`kernel-canary-contract`、`header-object-audit`、`stack-frame-audit`、`driver-model-boundary`、**`gui-build-audit`**、`user-canary`。

### 4.4 已发布文件未被就地改动

`test-gui-incremental` 的 `touch` 阶段会触碰 `libgui/include/gui/gui_input.h` 的 mtime 并在最后用 `touch -d` 还原。运行前后对三个公开头做 SHA-256 校验：

```
libgui/include/gui/gui_input.h: 成功
libgui/include/gui/gui.h: 成功
libgui/include/gui/gui_platform.h: 成功
```

（`sha256sum -c`，对照运行前记录。**内容逐字节未变。**）

---

## 5. 失败与限制（全部如实记录）

### 5.1 `SUITE=resolution` 失败 —— **pre-existing，不是本分支的回归**

**失败的断言**（来自 make 目标的 stdout；该 suite 的 `log()` 输出没有进归档，见 §5.4）：

```
FAIL: client_estale_no_diagnostic
FAIL: client_estale_no_exit_status
resolution production suite: 2 failure(s)
```

第二次运行（`06b-test-qemu-resolution-rerun.log`）报出**完全相同**的两条断言 ⇒ 确定性失败，不是 flake。

**位置**：`qemutests/test_resolution_switcher.py:1237-1264` 的 `_client_estale_case()`。

**观察到的现象**（归档 `round-trip/serial.log`）：

```
# { /bin/test_lvgl wait; echo RES_LVGL_EXIT=$?; } &
fork: pid=3 returned 30
# fork: pid=30 returned 31
[test_lvgl] Starting LVGL v9.5.0 showcase...
[test_lvgl] Memory pool: total=4186392, free=4154000, used_pct=1%
[TEST PASS] LVGL compatibility smoke test succeeded.
[test_lvgl] Interactive mode: Press 'q' or ESC to exit...
[term-probe] wakes/s=16 presents/s=10 ...
RES_LVGL_EXIT=0
```

客户端一进入交互循环就以 **0** 退出，**发生在 `setres 1280x720` 让它的 view 失效之前**。逐字节核对 `Interactive mode` 与 `RES_LVGL_EXIT=0` 之间的串口流：只有 terminal 的周期统计行，**没有任何输入字节**。因此客户端不是通过 `'q'`/`'Q'`/ESC/`0x03` 退出的（那会留下 TTY 回显），而是走了 `user/test_lvgl/test_lvgl.c:563-590` 的另一条出口：`read()` 返回 0（EOF）或 `POLLERR|POLLHUP|POLLNVAL` → `break` → `ret` 仍为 0。

**后果**：`setres 1280x720` 使 view 失效时已经没有活着的客户端，于是既看不到 stale-view 诊断（`display mode changed, restart the app`），也看不到非零退出码 —— 两条断言因此失败。

**同环境基线对照（决定性证据）**：在**同环境**（同样的 clang 23.1.3 / ld.lld 23.1.3 / QEMU 11.1.2、同一个 `OVMF.fd`、同一台宿主）下把基线提交 `6126504a` 导出到临时目录并跑同一个 suite：

```
FAIL: client_estale_no_diagnostic
FAIL: client_estale_no_exit_status
resolution production suite: 2 failure(s)
```

基线归档 `/tmp/os01-base-6126504a/build/x86_64-clang/logs/tests/resolution/20261021T204829Z-2db5e0299126409da0f851ba3c6c009a/`：`result.json` 的 `status = FAIL`、`runner_exit_code = 1`，其 `round-trip/serial.log` 里是**同一段**客户端提前退出的签名（`Interactive mode: Press 'q' or ESC to exit...` 紧跟 `RES_LVGL_EXIT=0`）。

⇒ **该故障在基线提交上同样存在，不是本分支引入的回归。** 本分支虽然改动了 `kernel/driver/fb_state.c`（+223）、`kernel/driver/gfx.c`（+134）与 `kernel/include/driver/fb_state.h`（+45），但基线对照排除了这些改动作为成因。

**本任务的处置**：**不修、不声称已修、也不代任何门禁宣布豁免**。这是一个需要 owner 决定的既有 parked fault（分辨率切换器的客户端策略，见 `docs/roadmap.md` 的 "QEMU 分辨率切换器收尾 follow-up" 段），本报告只负责把它如实暴露出来。

**其它两点相关事实**：

- **CI 不跑这个 suite**（`.github/workflows/ci.yml` 只跑 `SUITE=phase-0` 与 `SUITE=systest`），所以分辨率 suite 的失败对 CI 不可见。
- 同一次运行还有一条环境限制：`ENVIRONMENT BLOCK: OVMF did not provide 1280x960; booted 1280x800 instead` —— 当前 OVMF 构建没有提供 1280x960 模式。

### 5.2 N-4 flake —— **在 Task 13 干净回归中未复现**（后续修复见 §8）

> **后续更新**：下面保留 Task 13 当时的调查与处置记录。§8 已用确定性的 echo-only 复现测试确认匹配缺陷，并修复 `_shell_block`；下文的“仍在代码里”“没有包含”仅描述当时的树。

- **本次干净回归没有复现**：GUI suite **24/24 PASS**（SMP 1 与 SMP 2 各 12/12，见 §3.1）。因此按 plan Task 13 的仲裁规则，**不在本任务里修改 `_shell_block`**。
- **历史上失败的是什么**：Task 12 评审者的**第一次** `make PROFILE=x86_64-clang test-qemu SUITE=gui` 在 SMP 1 失败，报 `[input_interaction] after the Ctrl+C: the terminal's /proc status carries no Pid or BirthId field`；**紧接着的重跑 24/24 通过**。
- **失败时的归档**：`build/x86_64-clang/logs/tests/gui/20261021T194347Z-82520d9949d349b4b608ff870e445935/` —— **该归档已被本次回归开头的 `make clean` 删除**（`make clean` 会清掉整个 `build/<profile>`），因此下面的根因描述来自评审者当时对归档的分析，**不是**我这次重新观察到的字节。
- **根因（评审者当时的结论）**：guest 的控制台把 shell 自己的 `fork: pid=3 returned 18` 诊断**插进了回显命令的中间**，导致抓到的 `/proc/<pid>/status` 块在 `Pid:\t` 之后被截断，`Pid`/`BirthId` 字段始终没有到达。脆弱点是 `qemutests/gui_input_test.py` 的 `_shell_block`：它用 `rfind` 在一条**同时承载命令回显**的流上匹配，因此可能挑到错误的那一次出现。
- **我在本次任务里独立复核到的、与该根因一致的事实**（这些是本次重新验证的，不是转述）：
  1. `kernel/syscall/sys_proc.c:275` 在**每一次** `fork()` 时用 `log_info` 打印 `fork: pid=%d returned %d`；guest shell 每跑一条外部命令都要 fork，所以这条诊断**例行地**出现在 runner 正在解析的同一条串口流上。
  2. `_shell_block` 发出的命令是 `printf '<begin>\n'; <cmd>; printf '<end>\n'` —— **回显出来的命令行本身就含有两个 marker 字符串**，因此单纯的 `rfind` 无法区分「回显」与「命令自己的输出」。
  3. procfs 格式化器**不可能**产出半截字段：`kernel/fs/procfs_fmt.c:22-34` 的 `emit()` 在任何截断上都返回 `-EOVERFLOW` 且调用方丢弃整个缓冲。所以「`Pid:\t` 后面没有值」是**串口字节流**被截断，而不是 procfs 缓冲被截断。
  4. **失败路径是既有代码**：`_shell_block`、`_step_check_terminal`、`_status_field`、`_shell` 四个函数在引入它们的提交 `d0ffe93f`（Task 11）与 HEAD 之间**逐字节相同**（用函数体抽取比对）。⇒ Task 12 的提交没有动过这条失败路径。
- **残留风险（如实记录）**：`_shell_block` 的 marker 匹配歧义**仍然在代码里**。要根治，marker 应当只允许在**行首**匹配（`_shell` 自己的 `__EXIT_` 正则已经这么做了：`(^|\n)__EXIT_{nonce}_(-?\d+)__(\n|$)`），这样回显出来的命令永远不可能被误认成命令自己的输出。这属于测试侧改动，**没有包含在本次提交里**。
- **没有**声称已修，**没有**宣布门禁豁免。

### 5.3 分支累积的既有 Minor（来自各任务评审的 deferred Minor，Task 13 收口时逐条带出）

这些**不是**本次回归新发现的问题，是 Tasks 3–12 评审记录下来的遗留项。

> **2026-10-22 更新**：随后的**整分支修复波**（`fix(gui): close the final whole-branch review findings`，`ac3f3eb6`）关掉了其中若干条。凡是被关掉的条目在下文**标注了对应的修复项**，不再作为"未修"列出；**没有被这一波碰过的条目原样保留**，仍然开放。凡是本节仍列着的条目，都**没有**被这一波关掉。

- **Task 3（内核路由）**：route 可以不带锁初始化（仍开放）；`dev_private=NULL` 在锁外读取（**已修：C2** —— 清理在路由锁内完成）；溢出路径可能刷屏日志（**已修：C3** —— 每个溢出 episode 只记一次，警告本身从不消失）；`drain_bytes` 会把不同的 errno 合并（**已修：C4** —— errno 原样返回，边界矩阵逐个区分）；host mock 绕过 ops 分派（仍开放）；ACTIVE poll 列表是共享的（仍开放）；NORMAL 路径上跨 epoch 的帧可能被吞掉（仍开放 —— 这是**既有观察**，`progress.md:104` 记录；这一波没有碰它）。
- **Task 4（gfx bind）**：weak-symbol 守卫是 fail-open 的（仍开放）。
- **Task 5（解码）**：`GUI_INPUT_SYNTHETIC` 的注释没有写明离屏约束（仍开放）；NULL-ignore 契约没有测试（**已修：C6** —— 契约被写进头文件并被用例钉住）。
- **Task 6（生命周期）**：被应用消费掉的按键仍会被武装重复（**已修：C8** —— 重复只对真正进入控件路径的按键武装）；mock 在故障分支之前记录 flags（仍开放）；公开头有合并冲突风险（仍开放）。
- **Task 7（焦点）**：`focus_sync` 取第一个候选而不是当前焦点**之后**的第一个（**已修：A3** —— 改为取旧焦点**之后**的第一个可用项，旧的已删除才回落到第一个）；`lv_group_get_focused` 回落被去掉（仍开放）；报告的 §G.2 变体表缺一行（仍开放）；对焦点组唯一成员调用 `gui_input_focus_remove()` 会发 `LV_EVENT_DEFOCUSED` 而不调用焦点回调，因此没有标记 cancel（仍开放）；应用可见 / 控件可见两条流的分歧未记录（**已修：C9** —— 公共头 `gui_input.h` 现在写明应用流是控件流的**超集**以及三种分歧情形）。
- **Task 8（指针）**：「拖动进行中」其实是更宽的「左键按住」窗口；`move_pointer_to_abs()` 把 int32 位移截断成 int16；`wheel_scroll_by()` 返回的是它**以为**滚动的像素数（这三项均仍开放）。
- **Task 9（构建/发布）**：已发布的 mbedtls 树不是自包含的（仍开放）；三个已发布的内核头对使用者来说是坏的（仍开放）；`qemutests/test_ci_workflow.py` 没有断言两个新 CI 步骤（**已修：A4** —— 两个步骤现在都有断言，去掉任一步骤即失败）；`hosttests/Makefile` 里有三份并行的 wrapper 列表（仍开放）；两个 commit body 仍带着已被推翻的「32 false positives」表述（仍开放 —— 属于历史，未被改写）。
- **Task 10（探针）**：指针位置从不上报，所以 Task 11 必须重新 home 光标（**已修：C10** —— 探针现在上报 `pointer_x` / `pointer_y` / `pointer_seen`，runner 断言位置，位置不对即失败）；`fatal` 的 harness 判据是「没有 EXIT 记录」（**已修：C11** —— 探针在退出前上报 `lvgl_assert=1`，runner 等待并断言这个**正向**事实）；报告里有一个进程隐患（仍开放）。
- **Task 11（QMP 矩阵）**：CLI `--timeout` 默认值 180→410 同时放宽了 GUI-1 的外层上界（仍开放）；有一次空 amend `2842cffe → d0ffe93f`（**已核实并记录，不是缺陷**：两者的 parent、tree、subject 完全相同，只有提交元数据不同；**没有**改写任何历史 —— 见 §7.3）；多行输入的提前到达残余余量只吸收**一次**额外的串口读（仍开放）；报告的 §3.1/§3.2 仍是第 1 轮的数值（**已澄清**：§3 描述的是 Task 13 那一轮（`093a5fb4`）的树，**最终树**的数字在 §7）。
- **Task 12（所有权/溢出/崩溃）**：变异活动保存在 `/tmp` 里、**没有**进仓库（fixture 进了）；卡住的 Shift 也会把数字变成符号，所以 canary 的大小写检查是形状级的；`input_overflow` 在真实 guest 里需要 32 批，落在 64 批 / 40 秒窗口内，但在更慢的宿主上余量更小；`_wait_prompt` 屏障可以由串口 shell 自己的提示符满足（真正的 PS/2 证据是 canary）；`SC_OWNER` 探针以非零码退出，而它的 EXIT 记录写着 `result=PASS detail=owner_loss_detected`；以及 §5.2 的 N-4 flake（marker 匹配缺陷已在 §8 修复，其余仍开放）。
- **Task 13（分支级回归）**：`docs/build/build.md` 的审计计数过期（**已修：A5** —— 现在是 13 项，并逐项列出审计名）；`SUITE=resolution` 的既有红灯（仍开放，见 §5.1，**不是**本分支的回归，也**没有**门禁豁免）。

**两条已明确告知用户的范围决定**：PS/2 wheel knock 的修复（`af42c90e`）修正了一个**既有内核驱动 bug**（在本功能分支范围内，经独立审计并被 Task 11 复审维持）；鼠标的 IntelliMouse knock 现在是 `{0xF3,200,0xF3,100,0xF3,80,0xF2}`，滚轮恰好 32 px/刻度。

### 5.4 其它观察（不是失败，但应当被记录）

- **归档的 stdout 捕获有缺口**：`gui` 与 `resolution` 两个归档在 `result.json` 里声明了 `stdout_log` / `stderr_log` 路径，却**根本没有创建这两个文件**（`gfx` 与 `systest` 的归档里有）。因此这两个 suite 的 `log()` 输出（包括分辨率 suite 的 `FAIL: ...` 行）只到终端，**不进归档**。分辨率 suite 的真实失败断言只能从 make 目标的 stdout 里取回（本次已取回，见 §5.1）。这是一处独立的 harness 捕获问题。
- `gfx` 与 `systest` 的 `result.json` 记录了 `git_dirty = True`：那两轮跑的时候我正在并发写文档，**产品代码树未变**（`docs/*.md` 不参与构建）。关键的 GUI suite 那一轮 `git_dirty = False`，是在干净树上跑的。
- 分辨率 suite 的归档里 `duration_s = 0.0`、`child_exit_code = null`、`stopped_by_runner = false`，但 `status = FAIL` —— 该 suite 不填这几个字段，不能拿它们判断有没有跑。

### 5.5 **没有**运行的东西（如实列出）

- **plan 的 checkbox 与任务 commit 没有更新** —— 按 Task 13 brief，plan 的 checkbox 由 controller（执行者）更新，不是本任务的范围。`docs/superpowers/plans/2026-10-20-gui-2-input-focus.md` 未被改动。
- **没有**做整条回归的基线对照 —— 只为**失败的那一个** suite（resolution）在基线 `6126504a` 上做了同环境基线。其余 suite 在基线上没有跑。
- **没有**跑 `make test-contract`、`make test-kernel-selftest`、`make test-aarch64`（不在 brief 的清单内）。
- **没有**跑 `FB_RESOLUTION_TEST=1` 的故障注入子集（分辨率 suite 的 `test` 用例）。
- **没有**做 aarch64 的任何构建或测试（GUI-2 明确不扩到 aarch64 用户态 GUI）。
- **没有**跑 GUI-3 / GUI-5 / IME / USB HID 的任何内容。
- **没有**重跑已经通过的 suite（除 §5.1 的第二次 resolution 运行与 §4 的增量契约外）。
- Task 12 的变异活动仍只在 `/tmp`，**没有**进仓库。

---

## 6. 结论

- GUI-2 自身的验收证据是**绿的**：干净回归下 `test-host` / `test-static` / `test-harness` / `test-gfx` / `test-syscall` 全部退出码 0，GUI suite **24/24**（SMP 1 与 SMP 2 各 12/12），Task 9 的增量/sysroot 契约四阶段全 PASS，已发布的 GUI 树恰好 `{gui.h, gui_input.h, gui_platform.h}`。
- 回归里唯一红的是 `SUITE=resolution`，其两条失败断言在**基线提交上同样复现**，因此**不是**本分支引入的回归；本报告不修它、也不替它宣布门禁豁免。
- N-4 flake 在本次干净回归中**未复现**；它被如实记录为一次观察到的 flake，**没有**被声称已修，其残留的匹配歧义也如实列出。
- 计划、范围与硬约束均未被违反：没有新增产品接口，发布面未变，键盘 raw payload 与 `mouse_event_t` 8 字节 ABI 未动，TTY `ISIG` 未被关闭，所有测试都从仓库根 make 入口运行，`GUI_HOST_REGISTERED_CASES` 仍是唯一列表，LVGL 子模块未被修改，`thirdpart/lvgl/` 未被触碰。

---

## 7. 最终树的门禁结果（本节描述的是**最终树**，不是 §2/§3 那一轮）

> §2 与 §3 记录的是 Task 13 那一轮回归（树 = `093a5fb4`）。其后的**整分支修复波**改动了 30 个文件（`ac3f3eb6`），随后的**第二轮修复波**又改动了 7 个文件（含 R2-1 的 libgui 改动、R2-4 的内核控制台写改动、R2-2 的注释、以及本报告自身）。因此**最终树**的门禁结果记在本节。

### 7.1 六条门禁命令：命令 → 退出码 → 摘要

全部从**仓库根**的 make 入口运行（没有用过 `make -C hosttests`）。脚本 `/tmp/gate3.sh`，完整输出 `/tmp/gate3.log`，退出码 `/tmp/gate3.exit`。

| # | 命令 | 退出码 | 摘要 |
|---|------|--------|------|
| 1 | `make PROFILE=x86_64-clang test-gui-host CASE=all` | **0** | 10 个 GUI 宿主二进制全 PASS；`test_gui_input_lvgl` **Total: 967 / Passed: 967 / Failed: 0**（第一轮修复波结束时是 949 个断言点 —— 第二轮新增的两个用例补上了差额） |
| 2 | `make PROFILE=x86_64-clang test-host` | **0** | `[run_hosttests] 128 binary(ies) run, overall exit 0` —— 与基线 `ed725324` 的 128 一致，**无回归** |
| 3 | `make PROFILE=x86_64-clang test-static` | **0** | **13** 项静态审计全 PASS：driver-model-boundary、gui-build-audit、header-object-audit、kernel-canary-contract、kernel-layout、kernel-runtime-link、runtime-audit、runtime-link-order、stack-canary-audit、stack-frame-audit、syscall-boundary、user-canary、validate-kernel |
| 4 | `make PROFILE=x86_64-clang test-harness` | **0** | `Ran 590 tests in 538.508s` — `OK (skipped=1)`（Task 13 基线 582，本分支修复波新增 +8） |
| 5 | `make PROFILE=x86_64-clang test-qemu SUITE=gui` | **0** | **24/24 PASS**（12 个用例 × SMP 1、SMP 2 各 12）；归档 `build/x86_64-clang/logs/tests/gui/20261022T065127Z-f82d62ce9e214448845fe220be1492d0/`；SMP1 331 条 / SMP2 332 条 `[gui-input]` 协议记录，**0 条**含拼接；`[term-probe]` 行数 85 / 86（终端输出与修复前一致） |
| 6 | `make PROFILE=x86_64-clang test-gui-incremental` | **0** | 四个增量断言（touch / compile / install / publish）全 PASS |

`/tmp/gate3.exit` 原文：

```
gui-host-all rc=0
test-host rc=0
test-static rc=0
test-harness rc=0
qemu-gui rc=0
gui-incremental rc=0
```

**发布面复核**（同一棵树）：已发布的 GUI 头树仍**恰好**是 `{gui.h, gui_input.h, gui_platform.h}` 三个文件（`build/x86_64-clang/staging/gui/usr/include/gui/`），没有私有头泄漏。

**已知红、且本报告不声称它是绿的**：`make PROFILE=x86_64-clang test-qemu SUITE=resolution` 在基线 `6126504a` 上以**同样两条**断言失败（见 §5.1）。它**不在**本轮门禁的六条命令里，本轮也**没有**声称它变绿，更**没有**宣布门禁豁免。本轮对分辨率路径**零改动**：`kernel/driver/fb_state.c` 与 `libgfx/gfx.h` 只改了**注释**，没有触碰任何分辨率代码、ioctl 或模式切换路径。

### 7.2 DESYNC 错误契约（最终树）

最终树的 DESYNC 契约是 **`-EOVERFLOW`**，并且**在整个租约生命周期内粘住** —— 依据设计文档 §4.2：

| 场景 | 错误表面 | 粘住范围 |
|------|----------|----------|
| ACTIVE 队列溢出，或一个序列跨越 epoch 边界被吞掉 | **`EOVERFLOW`** | read 与 poll 在**整个租约生命周期内**持续报告它，直到应用交还租约并重新绑定（新租约从已知一致状态开始） |

配套的文档已经同步到最终树：`docs/driver/driver.md:489`、`docs/gui/gui.md:360`、`docs/gui/gui.md:384`（"跨 epoch 的序列 → `EOVERFLOW`，与队列溢出同一个 errno"）、`docs/changelog.md:13`。

**这一点与本报告 §2/§3 那一轮（Task 13）不同**：那一轮描述的是**一次性** `-ESTALE` 的旧行为，因此 §2/§3 的数值**不能**当作最终树的契约证据 —— 最终树的契约以本节为准。

### 7.3 §5.3 已就地更正

§5.3 的条目已**逐条**就地标注：被修复波关掉的条目标了对应的修复项（C2 / C3 / C4 / C6 / C8 / A3 / C9 / A4 / C10 / C11 / C12 / A5），**仍然开放**的条目明确写"仍开放"，并保留了既有观察（例如 "NORMAL 路径上跨 epoch 的帧可能被吞掉"，`progress.md:104`）。

**没有任何一条是被"顺手关掉"的** —— 凡是被标为已修的，都确实有对应的改动与 RED→GREEN 证据；凡是没有被这一波碰过的，**仍然开放**，原样保留。

关于 §5.3 里 Task 11 的那条"空 amend `2842cffe → d0ffe93f`"：这是**已核实并记录的事实，不是缺陷** —— 两者的 parent、tree、subject 完全相同，只有提交元数据（committer 时间戳）不同；本轮**没有**改写任何历史。完整的逐项证据见 `.superpowers/sdd/2026-10-20-gui-2-input-focus/final-fix-report.md` 的 D2 与 R2-3 两节。

### 7.4 本轮新增的一条观察（不是失败）

第二轮修复期间，QEMU GUI suite 在最终树上一度变红（4 次运行、3 种失败形态）。根因是**既有的**内核缺陷：`kernel/fs/devfs.c` 的 `serial_write()` **逐字节**释放控制台锁，因此另一路控制台写者可以插进一条协议记录中间，把记录劈成两半（实测 `[term-probe]` 的统计行落在 `... pointer_seen=0 fat` 与 `al_trigger=1` 之间）。该缺陷已在 `kernel/fs/devfs.c:109-142` 修复（整条缓冲在一次 `spin_lock_irqsave` 内发出），终端**打印内容不变**，runner 的解析器**未削弱**。详见 fix report 的 R2-4 一节。


### 7.5 本轮新增的第二条观察（不是失败）：guest 侧 `test_lvgl` 的用户态 SIGSEGV

第二轮修复期间在最终树上连续跑了 4 次 `SUITE=gui`，其中**第 3 次**的 SMP 1 `smoke` 用例失败：

```
/bin/test_lvgl smoke alias failed (ok=True, rc=139)
Killing task 5 (user fault at RIP=000000000049884D, signal=11)
```

`0x49884D` 解析到 `build/x86_64-clang/user/test_lvgl.elf` 的 `memset`，且崩溃发生在 `[test_lvgl] Rendering and pixel assertions passed.` **之后**。

**这不是这两轮修复引入的**：第二轮在 `libgui` 内的唯一改动是 `gui_input_lvgl_repeat_after_down` 里的一次比较，不可能损坏 `memset`；对 `kernel/driver/fb_state.c` 与 `libgfx/gfx.h` 的改动是**纯注释**。该现象在第 4、5 次运行与 §7.1 的最终干净门禁中**均未复现**。

因此它被记录为一条**独立的、未定位的 guest 侧故障**：**未修**、**未**打包进任何修复，也**未**声称已修。后续需在能复现时抓 guest 侧寄存器/栈再定位。该项已同时登记到 `docs/roadmap.md` 的 Parked 段。

## 8. `055fd67b` 后的 code review 修复

本节对应 worktree `.worktrees/gui-2-input-focus` 的后续九项评审修复，覆盖 §7 之后的代码变更；旧轮次结果保留作历史记录。

### 8.1 修复与回归覆盖

| 评审项 | 修复与测试 |
|---|---|
| revoke 后 held input 泄漏 | unbind、final close、owner revoke、替换租约均隔离仍按住的键；已租用鼠标的 held buttons 同样隔离到物理释放。覆盖 typematic、break、fresh press 和 keyboard-only replacement。 |
| 跨 epoch 修饰键污染 NORMAL | 先判定 epoch 与 isolation，再更新 logical state；覆盖 E0 跨 revoke 后的 Ctrl+C，确认不生成幽灵 Ctrl。bind 时 held modifiers 也隔离，UAPI 仍只带 Caps latch。 |
| 左右修饰键互相覆盖 | logical state 按物理键分别记录，再聚合 Shift/Ctrl/Alt；覆盖释放一侧仍保持另一侧。 |
| bind 保留 NORMAL raw backlog | sink reset 在 route 锁内清空所租用设备的 raw queue 并唤醒 poller；不清空已投递的 TTY/PTY 内容。 |
| Delete 键码错误 | 使用 E0 53（`0x153`）；覆盖真实 Delete 及 keypad decimal 反例。 |
| 焦点失效后的幽灵点击 | 每个 FIFO key 前、native callback 后和 LVGL callback 后同步焦点有效性；覆盖自身/祖先 hidden、disabled 及同批回调。取消受阻时保留 FIFO head，保存 prepared/consumed，native callback 仅调用一次，避免忙等。 |
| repeat 延迟遗漏 deadline | next delay 取 repeat 剩余时间的最小值；覆盖 399/400/439/440 ms 边界。 |
| 删除无关控件取消当前输入 | 仅删除当前 pressed target 才取消；覆盖文字 repeat 和 Enter click。 |
| shell marker 误匹配命令回显 | 只接受完整独占行且带换行的 begin/end marker；新增 echo-only、延迟分片、嵌入 marker、未完整结束行四项测试。 |

新增回归在修复前失败、修复后通过。全量 host 首次重建发现 `test_gfx_input` 仍断言 unbind 后 held typematic 可立即进入 NORMAL；更新为重复 make 与 break 均吞掉、fresh make 可读后，全量重跑通过。

### 8.2 本轮验证

内部结构变更后先执行 `make clean`，以下 make 均从 worktree 仓库根入口执行。

| 命令 | 退出码 | 结果 / 日志 |
|---|---|---|
| `make -j4 PROFILE=x86_64-clang test-host` | 0 | 128 个二进制通过；真实 LVGL 57 cases / 1169 assertions，route 618 assertions。`/tmp/gui2-fix-host-rerun.log` |
| `make -j4 PROFILE=x86_64-clang test-static` | 0 | 13 项审计通过。`/tmp/gui2-fix-static.log` |
| `TMPDIR=/tmp make PROFILE=x86_64-clang test-qemu SUITE=gui`（沙箱外） | 0 | SMP=1/2 各 12/12，共 24/24 PASS。归档 `build/x86_64-clang/logs/tests/gui/20261022T152525Z-eea51d82f7474166ad205b56b5a959f7/`，输出 `/tmp/gui2-fix-qemu-final.log`。 |
| `python3 -m unittest qemutests.test_gui_runner` | 0 | 85 tests，包含四项新 shell marker 回归。`/tmp/gui2-review-fix-runner.log` |
| `python3 -m unittest qemutests.test_gui_runner.GuiInputShellBlockTests qemutests.gui_input_test.GuiInputProtocolTests qemutests.gui_input_test.GuiInputQmpPayloadTests` | 0 | 56 tests 通过。 |

QEMU 的两次沙箱内尝试均未进入 guest 验证：首次因 `/var/tmp` 只读无法打开 firmware 临时文件；设置 `TMPDIR=/tmp` 后仍无法建立 QMP 连接。随后沙箱外重跑完成上述 24 项，不将启动失败算作产品用例结果。

内核与 libgui/Python 分别经独立子 agent 复审，均 **APPROVE**；全量测试中调整的 gfx held-key 断言也经再次复审 **APPROVE**。

本轮未重跑完整 `test-harness`、resolution 或 syscall suite；§7 的历史结果不能代替这些套件在本轮代码上的验证。复审另记录既有范围外限制：无活动键待取消时直接禁用 LVGL indev，初始 DOWN 仍可能丢失，本轮未修改该通用路径。

# EL0 FP/SIMD 上下文剩余项设计（roadmap P2 ①②③）

- 日期：2026-11-21
- 分支：`feat/el0-fpsimd-context`（worktree `.worktrees/el0-fpsimd-context`）
- 锚点：`docs/roadmap.md` P2「EL0 FP/SIMD 上下文（剩余项）」；`docs/arch.md` §aarch64 用户态现状「仍保留的边界」
- 前序落地：`734f67bc`（CPACR_EL1.FPEN 放行、per-task FP 保存恢复、userland/libc 解除 `-mgeneral-regs-only`）

## 意图与动机

roadmap 该项剩余三件事。探索核实后的关键事实修正了 ① 的前提：

1. **① 的「EL0 浮点指令仍会陷入」不成立**。FPEN=0b11 已在全部 CPU 落地（BSP `kernel/arch/aarch64/cpu/user_task.c:290-297`、AP 经 `ap_sched.c` 复用同一函数、EL2 层 `kernel/arch/aarch64/head.S:97-98` / `secondary_entry.S:25-26` 置 `CPTR_EL2.TFP=0`）。按 ARM 规范 FPEN=0b11 时 EL0 的 FP/SIMD 指令直接执行、不产生陷入；`AARCH64_USER_TASK_PSTATE=0x340` 的 F 位（bit6）只是 FIQ 掩码，与 FP 陷入无关。roadmap/arch.md 引用的 EC 也有误：FP 访问陷入的正确 EC 是 **0x07**（`ESR_ELx_EC_FP_ASIMD`）；0x2C 是 `FP_EXC64`（FPCR 陷阱使能的浮点**异常**，本内核 FPCR 恒 0 不会触发）、0x2D 是 GCS，均与访问陷入无关。`user_image.h:58-60` 的注释「the kernel explicitly disables FPEN」是 FPEN 放行之前的过期文字。
2. **② 是真隐患**：signal handler 一旦使用 FP（如 `printf %f`），会把被打断上下文的 FP 寄存器毁掉——sigreturn 只恢复通用寄存器。两架构 signal 帧均无 FP 区（x86 侧同样缺失，但本任务范围限 aarch64）；Linux arm64 在 signal 帧里保存/恢复 fpsimd。
3. **③ 缺运行时证据**：`user/test_fp.c` 已能编译出 `build/<profile>/artifacts/user/test_fp.elf`（两架构，未被任何排除清单挡住），但不装盘、无任何 harness 引用，EL0 FP 是否真的不陷入没有实测证据。

成功标准：aarch64 产品用户态里 `/bin/test_fp` 实测通过（fork 穿透 + 上下文切换 FP 存活 + libc FP printf），sigreturn 对 FP 状态的破坏路径关闭，文档与代码注释不再传播错误前提。

## 目标 / 非目标

**目标**

- ①（重新定界）：修 `user_image.h:58-61` 过期注释；修 `docs/roadmap.md:55` 与 `docs/arch.md` 的 EC/F 位表述；EL0 来源的 EC 0x07 陷入显式分支 → fatal 停机诊断（FPEN 不变量被破坏）；`intr_decode.c` 补 EC 0x07 → `INTR_REASON_PROTECTION` 分类（覆盖 EL1h 来源路径的事件记录精度）；用 ③ 的实测证明 EL0 FP 不陷入。
- ②：`struct aarch64_sigframe` 增设 FP 保存区；信号交付时把存活 FP 状态存入帧，`sigreturn` 校验后恢复。既有偏移（`regs@0` 272 B、`blocked@272`）保持稳定；以 fixture 新增的 FP 往返断言作为实测验收。
- ③：`test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=fp`——产品盘加装 `/bin/test_fp`，经 busybox ash 实跑，harness 断言 `test_fp: PASS`；CPU 矩阵 1/2/4/8 对齐 product case。
- 文档闭环：roadmap 项、arch.md 边界段、changelog 2026-11-21、`docs/build/build.md` user-runtime 说明。

**非目标**

- x86 signal 帧的 FPU 区（同款隐患，另立后续任务，在 roadmap 留痕）。
- lazy FP（trap-enable）重设计：eager 方案已落地，hobby OS 无此性能诉求。
- SVE/SME、FPCR 陷阱使能（FP_EXC 0x28/0x2C）处理。
- aarch64 时间 syscall 组（`nanosleep` 等）解禁——`test_fp` 在 aarch64 上容忍 `-ENOSYS`。

## 方案取舍

**① 怎么做**：采纳「重新定界」（验证 + 修注释/文档 + EC 0x07 兜底解码），否决按 roadmap 原文建 lazy FP 陷入路径（推翻已落地 eager 方案、复杂度高、无收益）。用户已认可（2026-11-21 会话）。

**② 帧布局**（三选一）：

| 方案 | 内容 | 裁决 |
|------|------|------|
| a. 追加 FP 区（采纳） | `blocked@272` 后 8 B 保留对齐，`fp@288`（528 B），帧总 816 B；既有偏移全部不动，`fp@288` 天然 16 字节对齐满足 `stp q` 要求 | ✅ |
| b. Linux ucontext 兼容格式 | 按内核 `fpsimd_context` 头+体组织 | ❌ 本仓无用户态解析方，libc trampoline 只 `svc 43` 不碰布局；兼容格式徒增格式风险 |
| c. 前插/重排 | FP 区放 regs 前 | ❌ 破坏 fixture 手写偏移（`x19@152`、`spsr@264`）与既有 ABI |

FP 区内部沿用 `kernel/include/arch/aarch64/fp.h` 既有布局（q0–q31 @0..511、fpsr@512、fpcr@516、520..527 保留），与 `AARCH64_FP_STATE_SIZE=528` 的 per-task 保存区同构，两处可用同一组汇编 helper。

**sigreturn 顺序**（关键兼容性决策）：第一段范围检查保持 280 B 原样（裁决点：跟随新结构体长度变成 816 会先吃到 `-EFAULT`，破坏 fixture 负例记录在案的 `-EINVAL` 语义）→ 复制前 280 B（regs+blocked）→ 过既有 forged-frame 守卫（spsr 校验）→ 第二段经 `copy_from_user_ft`（自带边界检查）复制 `sp+288` 处 528 B FP 区 → 全部复制成功后才统一恢复（regs、blocked、FP 一次性恢复，**不做部分恢复**；任一复制失败按各自语义原样返回 `-EFAULT`）。理由：fixture 的负例（伪造 spsr=0x5 → 期待 `-EINVAL`；未对齐 sp → 期待 `-EFAULT`）手写 280 B 帧；若先复制整个 816 B，靠近栈顶的伪造帧会先吃到 `-EFAULT` 而不是期待的 `-EINVAL`。守卫前置也顺带省掉对伪造帧的无谓 FP 复制。

**③ 接线机制**（三选一）：a. 产品盘 + `USER_RUNTIME_CASE=fp`（✅ 采纳——证据最强：真实产品用户态 init/ash/libc，复用 product case 的成熟流程）；b. m4-1 嵌入式 fixture 自测（❌ 跨树工件嵌入复杂，fixture 需 libc 链接的 C 程序不合既有 .S 模式）；c. 新增独立 MODE（❌ 比扩 case 族重）。

## ② 的 ABI 与实现细节

```
struct aarch64_sigframe v2（总 816 B，_Static_assert 更新）
  0    .. 271   pt_regs_t regs        （不动）
  272  .. 279   uint64_t  blocked     （不动）
  280  .. 287   uint8_t   reserved[8] （对齐垫）
  288  .. 815   fp 区 528 B（q0–q31、fpsr、fpcr、保留）
```

- **交付**（`aarch64_do_signal_delivery`，`user_task.c:992-1064`）：`new_sp = (regs->sp_el0 - 816) & ~0xf`；构建本地帧时对存活硬件 FP 状态调 `aarch64_fp_save(frame.fp)`（交付始终运行在受害者 CPU 的 eret 前，活跃寄存器即被打断任务的 FP 状态；helper 是纯汇编，内核 C 侧 `-mgeneral-regs-only` 不受影响）；`copy_to_user_ft` 尺寸 280→816；复制失败仍保持现语义（还原 pending 位、不装帧）。handler 入场不清 FP 寄存器（继承被打断状态，Linux 同语义）。本地 816 B 帧在内核栈上，余量充足。
- **sigreturn**（`arch_syscall_sigreturn`，`user_task.c:1066-1095`）：对齐检查（不动）→ 范围检查保持 280 B（裁决，见「方案取舍」）→ 复制 280 B → forged-frame 守卫（不动，`(spsr & ~0xf0000000)==0x340`）→ 第二次 `copy_from_user_ft` 取 `sp+288` 的 528 B → 全部成功后统一恢复 `*regs`、`blocked`、`aarch64_fp_restore(frame.fp)`（无部分恢复）。恢复后 eret 回 EL0，活跃寄存器即被打断上下文的 FP 状态。无条件恢复（无魔数/可选区），OS01 自有 ABI 保持简单；exec 后伪 sigreturn 恢复旧 FP 数据无安全影响（纯数据寄存器）。
- **fixture 兼容与扩展**：`kernel/selftest/fixtures/aarch64_user_signal.S` 正例帧由内核写（816 B），负例帧只写 280 B 且守卫在 FP 复制前 → 语义不变（builder check 已核实 1 页自测栈 sp−816 仍在页内，argc==0 时 payload 在 `initial_sp` 上方约 88 B，`initial_sp−848` > 栈基址；兜底措施保留：若实施中实测越界，给该 fixture 换 `AARCH64_PRODUCT_STACK_PAGES`）。**新增 FP 往返断言**：main 在解除阻塞前设已知 d8–d15 + fpsr/fpcr → handler 破坏 FP 寄存器（清零/毒化）→ sigreturn 返回后校验 d8–d15 与 fpsr/fpcr 逐位完整——把「sigreturn 对 FP 状态的破坏路径关闭」从架构推理变成实测证据（builder check Q2 采纳）。
- **libc 零改动**：trampoline（`libc/arch/aarch64/sigreturn_trampoline.S`）只做 `svc 43`。

## ① 的实现细节

- `kernel/include/arch/aarch64/user_image.h:58-61`：注释改写为「FPEN=0b11 已放行 EL0/EL1，per-task FP 状态在 switch_to 保存恢复；PSTATE F 位是 FIQ 掩码，保持掩蔽是标准做法（D/A/F 置位、I 放行）」。
- **EL0 来源 EC 0x07 → fatal 停机诊断**（用户裁决 2026-11-21）：在 EL0 同步异常路径（`aarch64_user_task_sync`，`user_task.c:596-635`，当前仅接受 DATA_ABORT_LOWER 进入 `aarch64_user_vm_fault`）前置显式分支：EC==0x07 → `log_err` 报「FP access trap at EL0 with FPEN=0b11 — FPEN invariant violated」附 esr/elr → 停机（对齐既有 fatal 模式）。FPEN 只在每 CPU 初始化写一次且永不清除，出现即内核 bug；逐个杀任务会掩盖系统性问题（所有用 FP 的任务将逐个死亡）。
- `kernel/arch/aarch64/intr/intr_decode.c:97-116`：新增 EC 0x07 常量与 case → `INTR_REASON_PROTECTION`、`INTR_ACCESS_UNKNOWN`（`kernel/include/intr/dispatch.h:49-53` 枚举无需扩展；builder check 已核实 reason 不参与路由，此改动提升 EL1h 来源路径的事件记录精度）。
- `docs/roadmap.md:55`、`docs/arch.md`「仍保留的边界」段：改写为真实剩余项（sigframe FP 区 + 运行时证据），修正 EC 表述（0x07 为访问陷入类，0x2C/0x2D 引用删除），并在 roadmap 留 x86 sigframe FPU 区的后续任务行。

## ③ 的实现细节

- `mk/components/image.mk`（aarch64 产品镜像块，295-301 附近）：`mcopy $(USER_ARTIFACT_DIR)/test_fp.elf ::/bin/test_fp`（仅 aarch64 产品盘；FAT 无 exec 位概念，busybox 同法可 exec）。
- `mk/components/run.mk`：`USER_RUNTIME_CASES_VALID := irq asid smp product fp`；`fp` 的 prep 与 product 相同（`$(MAKE) aarch64-uefi`），run 用 product 镜像/kernel 传 `--case fp`。注意 run.mk 是源码文本断言目标（`test_harness_static` 等 pin），改动需跑相关静态审计。
- `qemutests/aarch64_user_runtime.py`：`--case` choices 加 `fp`；fp 流程镜像 product（1/2/4/8 各启动一次，等 shell 证据、发 `/bin/test_fp\n`、断言独立 `test_fp: PASS` 行、发 `exit`）；复用 product 的内核 ELF 测试符号黑名单检查（fp case 的内核同样是产品内核）。
- `user/test_fp.c`：child 的 `nanosleep` 在 aarch64 返回 `-ENOSYS`（时间 syscall 组未解禁）——现返回值本就被忽略，补注释说明容忍语义，不改行为。
- `docs/build/build.md`：user-runtime MODE 说明行补 `fp` case 与期望输出。

## 测试与验收

1. `make test-host`、`make test-static`：全绿（sigframe 无静态 pin，已核实；`test_harness_static` 等 run.mk pin 项随本次 run.mk 改动必须重跑）。
2. m4-1 内核自测（`PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 run-aarch64-uefi` 手动观察串口）：28 例全过，重点 `aarch64_user_signal`（验证 sigreturn 顺序重排 + 816 B 帧对 fixture 正负例的兼容 + **新增 FP 往返断言**：d8–d15/fpsr/fpcr 穿越信号交付与 sigreturn 逐位存活）。
3. **验收主证据**：`make OS01_PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=fp` → PASS，1/2/4/8 核均见 `test_fp: PASS`。
4. 回归：`USER_RUNTIME_CASE=product` PASS（产品盘新增 /bin/test_fp 后启动不受影响）。

## 风险与缓解

- fixture 1 页栈的 sp−816 余量 → builder check 已核实页内成立（兜底：实测越界则换 `AARCH64_PRODUCT_STACK_PAGES`）。
- `copy_to_user_ft` 816 B 跨用户栈页边界 → 依赖既有 copy 语义（失败还原 pending），栈页连续映射下不成立风险。
- busybox ash 实跑路径 → 与 x86 systest exec 模式同构、busybox aarch64 已合入（b78bf6f8），风险低。

## 历史方案记录

- lazy FP（EC 陷入驱动使能）：否决，理由见上。
- Linux ucontext 兼容帧格式：否决，理由见上。
- m4-1 嵌入式 fixture 方式接 ③：否决，理由见上。

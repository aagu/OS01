# EL0 FP/SIMD 剩余项实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 关闭 aarch64 signal 帧 FP 保存恢复缺口，并把 EL0 FP 不陷入的实测证据接入 `test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=fp`。

**Architecture:** eager FP（FPEN=0b11 + per-task switch_to 保存恢复）保持不动；sigframe 追加 528 B FP 区（280→816 B）；sigreturn 两段复制（280 B GPR 段过守卫 → FP 段），EL0 来源 EC 0x07 陷入升级为 fatal 停机诊断；运行时证据走产品盘 `/bin/test_fp` + busybox ash + user-runtime fp case。

**Tech Stack:** C/AArch64 汇编（内核）、GNU make + mtools 镜像、Python QEMU harness（qemutests/aarch64_user_runtime.py）、clang aarch64 target。

**Spec:** [docs/superpowers/specs/2026-11-21-el0-fpsimd-design.md](../specs/2026-11-21-el0-fpsimd-design.md)（执行时连同本计划一起读）

## Global Constraints

- 内核 C 全量 `-mgeneral-regs-only`：FP 指令只允许出现在 `.S` 文件；`.S` 规则用 `$(filter-out -mgeneral-regs-only,$(ALL_CFLAGS))`（先例：`kernel/Makefile:1033`）。
- 内核日志用 `log_err`/`log_warn`/`log_info`，不用 `serial_printk`/`color_printk`（AGENTS.md）。
- 整数用固定宽度类型；本任务不触碰 `boot_context` ABI（无需跑 test_bootinfo_abi，但 `make test-host` 全量必跑）。
- `mk/components/run.mk` 是源码文本断言目标（`test_harness_static`/`test_make_qemu_failure` 等 pin），改后必须跑 `make test-host`。
- 后台/脚本里跑 make 用 `TMPDIR=/tmp`（patch(1) 临时文件落 `/` 会 Permission denied）。
- commit 用 conventional 风格（`feat:`/`fix:`/`test:`/`docs:`），每 Task 一提交。
- 验证命令一律在 worktree `/home/aagu/OS01/.worktrees/el0-fpsimd-context` 下运行。

## Review Focus

1. **handler 用 FP 毁掉被打断上下文**——sigreturn 恢复路径错误（步长/fpsr-fpcr 互换/偏移错位）会导致 d8–d15 或 fpsr/fpcr 逐位不符 → fixture 断言退出码非 0x66 → `aarch64_user_signal` 自测 FAIL（Task 3 钉住）。
2. **伪造帧负例语义回退**——sigreturn 首段范围检查若误用 816 B，栈顶附近伪造帧会以 `-EFAULT`（而非记录在案的 `-EINVAL`）失败 → fixture 负例 #2 退出码 0x02（Task 2/3 的 m4-1 运行钉住）。
3. **FP 复制部分恢复**——第二段复制失败后必须不做任何恢复；实现若先恢复再复制会留下撕裂状态（Task 2 代码评审 + 负例回归钉住）。
4. **产品盘回归**——`/bin/test_fp` 加入 FAT 后 mkdisk/启动链路不得回归 → `USER_RUNTIME_CASE=product` PASS（Task 4/6 钉住）。
5. **run.mk 文本 pin 回归**——改 `USER_RUNTIME_CASES_VALID`/prep/run 分支后 `test_harness_static` 等静态断言必须全绿（Task 4 钉住）。

---

### Task 1: ① EC 0x07 常量、EL0 fatal 分支、解码分类与注释修正

**Files:**
- Modify: `kernel/include/arch/aarch64/sync_fault.h:72-75`（EC 常量区）
- Modify: `kernel/arch/aarch64/cpu/user_task.c:598-605`（`aarch64_user_task_sync` 非 SVC 分支头部）
- Modify: `kernel/arch/aarch64/intr/intr_decode.c:97-116`（`aarch64_intr_decode_sync` switch）
- Modify: `kernel/include/arch/aarch64/user_image.h:58-61`（注释）

**Interfaces:**
- Consumes: `AARCH64_ESR_EC_MASK`/`AARCH64_ESR_EC_SHIFT`（sync_fault.h:29-30）；`log_err`；`arch_cpu_halt`（user_task.c 已用）。
- Produces: `AARCH64_EC_FP_ACCESS`（= 0x07ULL，后续 Task 引用）。

- [ ] **Step 1: sync_fault.h 加 EC 常量**（紧跟 `AARCH64_EC_PC_ALIGNMENT` 一组，命名风格对齐）：

```c
/* EC 0b000111: trapped AdvSIMD/FP instruction (EL0/EL1, CPACR_EL1.FPEN
 * disable trap).  OS01 runs FPEN=0b11 everywhere, so reaching this
 * class means the FPEN invariant is broken. */
#define AARCH64_EC_FP_ACCESS         0x07ULL
```

- [ ] **Step 2: `aarch64_user_task_sync` 非 SVC 分支头部插 fatal 分支**（`if (ec != A64_EC_SVC_LOWER) {` 的函数体第一条，`aarch64_user_vm_fault` 调用之前）：

```c
        /* FP access at EL0 with CPACR_EL1.FPEN=0b11 is a kernel
         * invariant violation (FPEN is set once per CPU at init and
         * never cleared).  Stop loudly instead of killing FP-using
         * tasks one by one. */
        if (ec == AARCH64_EC_FP_ACCESS) {
            log_err("[user-task] FATAL: FP access trap at EL0 with "
                    "FPEN=0b11 esr=%lx elr=%lx\n",
                    (unsigned long)esr, (unsigned long)regs->elr_el1);
            for (;;)
                arch_cpu_halt();
        }
```

（对齐本文件既有 fatal 模式：`log_err` + `for(;;) arch_cpu_halt()`，见 `aarch64_user_runtime_init`。）

- [ ] **Step 3: `intr_decode.c` switch 加 case**（`AARCH64_EC_PC_ALIGNMENT` case 之后）：

```c
    case AARCH64_EC_FP_ACCESS:
        out->reason = INTR_REASON_PROTECTION;
        out->access = INTR_ACCESS_UNKNOWN;
        break;
```

（builder check 已核实 reason 不参与路由，此 case 只提升 EL1h 来源路径的事件记录精度。）

- [ ] **Step 4: `user_image.h:58-60` 注释改写**（宏值 0x340 不动）：

```c
/* Initial EL0 PSTATE: EL0t (M = 0) with D/A/F masked and I enabled.
 * User code runs with interrupts on.  F masks FIQ — it has nothing to
 * do with FP/SIMD access (that is controlled solely by
 * CPACR_EL1.FPEN, which the kernel sets to 0b11 on every CPU; per-task
 * FP state is saved/restored by switch_to()). */
#define AARCH64_USER_TASK_PSTATE  UINT64_C(0x340)
```

- [ ] **Step 5: 构建验证**

Run: `TMPDIR=/tmp make PROFILE=aarch64-clang aarch64-uefi -j$(nproc)`
Expected: 内核与镜像零告警构建完成（新 EC 常量无冲突）。

- [ ] **Step 6: 静态与宿主回归**

Run: `TMPDIR=/tmp make test-host -j$(nproc) && make test-static`
Expected: 全绿。

- [ ] **Step 7: Commit**

```bash
git add kernel/include/arch/aarch64/sync_fault.h kernel/arch/aarch64/cpu/user_task.c kernel/arch/aarch64/intr/intr_decode.c kernel/include/arch/aarch64/user_image.h
git commit -m "feat(aarch64): classify FP-access traps and stop on FPEN invariant violation"
```

### Task 2: ② sigframe FP 区 + 交付保存 + sigreturn 两段恢复

**Files:**
- Modify: `kernel/include/arch/aarch64/syscall.h:26-37`（struct + 断言 + include）
- Modify: `kernel/arch/aarch64/cpu/user_task.c:1039-1042`（交付帧构建）
- Modify: `kernel/arch/aarch64/cpu/user_task.c:1076-1090`（sigreturn）

**Interfaces:**
- Consumes: `aarch64_fp_save(void *raw)` / `aarch64_fp_restore(const void *raw)`（`kernel/include/arch/aarch64/fp.h:20-21`，`AARCH64_FP_STATE_SIZE=528`）；`copy_from_user_ft`/`copy_to_user_ft`（现有调用同款）。
- Produces: `struct aarch64_sigframe` v2（816 B：regs@0 / blocked@272 / reserved[8]@280 / fp[528]@288）——Task 3 fixture 与文档依赖此布局。

- [ ] **Step 1: `syscall.h` 扩展结构体**（include 区补 `#include <stddef.h>` 与 `#include <arch/aarch64/fp.h>`，保留既有 `regs` include）：

```c
struct aarch64_sigframe {
    pt_regs_t regs;                    /* 272 bytes: full exception frame */
    uint64_t  blocked;                 /* saved BSD-style blocked mask */
    uint8_t   reserved[8];             /* keeps fp 16-byte aligned */
    uint8_t   fp[AARCH64_FP_STATE_SIZE]; /* fp.h layout: q0..q31, fpsr, fpcr */
};
_Static_assert(sizeof(struct aarch64_sigframe) ==
               280 + 8 + AARCH64_FP_STATE_SIZE, "sigframe v2 size");
_Static_assert(offsetof(struct aarch64_sigframe, fp) == 288 &&
               (offsetof(struct aarch64_sigframe, fp) & 0xf) == 0,
               "fp area 16-byte aligned at 288");
```

（既有 280 B 断言删除/替换；`regs@0`、`blocked@272` 偏移不变，fixture 手写偏移不受影响。）

- [ ] **Step 2: 交付保存**（`aarch64_do_signal_delivery`，frame 初始化之后、`new_sp` 计算之前插一行）：

```c
        /* The interrupted task's FP state is live in the hardware at
         * delivery time (the kernel never touches FP/SIMD); stash it
         * so the handler may clobber the registers freely.  sigreturn
         * restores it from frame.fp. */
        aarch64_fp_save(frame.fp);
```

（`new_sp` 与 `copy_to_user_ft` 都用 `sizeof(struct aarch64_sigframe)`，自动变 816；复制失败还原 pending 位语义不变。）

- [ ] **Step 3: sigreturn 两段复制**（替换现有 `syscall_check_user_range`+整帧 `copy_from_user_ft` 块）：

```c
    struct aarch64_sigframe frame;
    /* Two-stage copy: the GPR half first, so a forged frame gets the
     * documented -EINVAL from the PSTATE guard before the FP-area copy
     * can hit an unmapped stack tail with -EFAULT.  Nothing is
     * restored until both copies succeed (no partial restore). */
    if (!syscall_check_user_range(regs->sp_el0,
                                  offsetof(struct aarch64_sigframe, reserved),
                                  false))
        return -EFAULT;
    if (copy_from_user_ft(&frame, (const void *)(uintptr_t)regs->sp_el0,
                          offsetof(struct aarch64_sigframe, reserved)) < 0)
        return -EFAULT;

    /* Require exact AARCH64_USER_TASK_PSTATE (0x340: EL0t with D/A/F masked
     * and I enabled) in all non-NZCV bits; reject forged EL1 modes or DAIF edits. */
    if ((frame.regs.spsr_el1 & ~UINT64_C(0xf0000000)) != AARCH64_USER_TASK_PSTATE)
        return -EINVAL;

    if (copy_from_user_ft(frame.fp,
                          (const void *)(uintptr_t)(regs->sp_el0 +
                              offsetof(struct aarch64_sigframe, fp)),
                          sizeof(frame.fp)) < 0)
        return -EFAULT;

    *regs = frame.regs;
    task_t *cur = arch_current_task();
    cur->blocked = (int64_t)(frame.blocked &
                             ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1))));
    aarch64_fp_restore(frame.fp);
```

（`AARCH64_USER_RUNTIME_TEST` 钩子与后续返回 0 保持原位。）

- [ ] **Step 4: 构建验证**

Run: `TMPDIR=/tmp make PROFILE=aarch64-clang aarch64-uefi -j$(nproc)`
Expected: 零告警。

- [ ] **Step 5: m4-1 信号自测（旧行为回归——此时 fixture 尚无 FP 断言）**

Run: `TMPDIR=/tmp timeout 300 make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi > /tmp/m41-t2.log 2>&1 || true`，然后 `grep -E "\[TEST\] (PASS aarch64_user_signal|END suite=kernel-selftest)" /tmp/m41-t2.log`
Expected: 串口日志出现 `[TEST] PASS aarch64_user_signal` 且 `[TEST] END suite=kernel-selftest ... failed=0`。伪造帧负例（`-EINVAL`）与未对齐负例（`-EFAULT`）均按原语义通过（timeout 到期停 QEMU 属预期，以 grep 结果为准）。

- [ ] **Step 6: Commit**

```bash
git add kernel/include/arch/aarch64/syscall.h kernel/arch/aarch64/cpu/user_task.c
git commit -m "feat(aarch64): save/restore FP state in signal frames"
```

### Task 3: fixture FP 往返断言 + 汇编规则解除 general-regs-only

**Files:**
- Modify: `kernel/Makefile:1018`（`aarch64_user_signal.o` 规则）
- Modify: `kernel/selftest/fixtures/aarch64_user_signal.S`（头注释、step 7 前的 FP 设置、handler 毒化、sigreturn 后校验）

**Interfaces:**
- Consumes: Task 2 的 sigframe v2（fp@288）；`US_EXIT` 宏（fixture 内已有）。
- Produces: `aarch64_user_signal` 自测用例含 FP 往返断言；退出码 0x0f/0x10/0x11/0x12 为 FP 校验失败码（仅失败路径使用，驱动 C 侧不变）。

- [ ] **Step 1: 改汇编规则**（`kernel/Makefile:1018`，对齐 `aarch64_user_runtime.o` 先例）：

```make
$(BUILD_DIR)/selftest/fixtures/aarch64_user_signal.o: selftest/fixtures/aarch64_user_signal.S
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(filter-out -mgeneral-regs-only,$(ALL_CFLAGS)) -MD -MP -c $< -o $@
```

- [ ] **Step 2: fixture 头注释补一行**（`aarch64_user_signal.S` 块注释 `2.` 项后加）：

```
 *        2b. FP round-trip: main sets d8=pi/d15=e + fpsr(QC)/fpcr(RZ);
 *            handler poisons them; sigreturn must restore every bit
 *            (exit codes 0x0f/0x10/0x11/0x12 on mismatch).
```

- [ ] **Step 3: step 7 区插入 FP 设置**（`8:` 标签之后、`movz x19, #0xbeef` 之前）：

```asm
    /* FP round-trip setup: known callee-saved d8/d15 + fpsr/fpcr. */
    movz    x9, #0x2d18
    movk    x9, #0x5444, lsl #16
    movk    x9, #0x921f, lsl #32
    movk    x9, #0x4009, lsl #48           /* pi = 0x400921fb54442d18 */
    fmov    d8, x9
    movz    x9, #0x5769
    movk    x9, #0x8b14, lsl #16
    movk    x9, #0xbf0a, lsl #32
    movk    x9, #0x4005, lsl #48           /* e = 0x4005bf0a8b145769 */
    fmov    d15, x9
    mov     x9, #(1 << 27)                 /* FPSR.QC (sticky, read-back) */
    msr     fpsr, x9
    mov     x9, #0x00c00000                /* FPCR.RMODE=RZ (no FP arith runs) */
    msr     fpcr, x9
```

- [ ] **Step 4: handler 毒化**（`sigusr1_handler` 中 g_sig_count 自增之后、x19 改写之前）：

```asm
    /* Poison FP state the interrupted context expects back after
     * sigreturn (the kernel must have saved it into the sigframe). */
    movi    v8.16b, #0
    movi    v15.16b, #0
    msr     fpsr, xzr
    msr     fpcr, xzr
```

- [ ] **Step 5: sigreturn 返回后校验**（`12:` 标签与 fork 块之间；新标签 20–23 避开既有 13/14）：

```asm
    /* FP round-trip: handler poisoned d8/d15/fpsr/fpcr; sigreturn
     * must have restored every bit from the sigframe FP area. */
    fmov    x9, d8
    movz    x10, #0x2d18
    movk    x10, #0x5444, lsl #16
    movk    x10, #0x921f, lsl #32
    movk    x10, #0x4009, lsl #48
    cmp     x9, x10
    b.eq    20f
    US_EXIT 0x0f
20:
    fmov    x9, d15
    movz    x10, #0x5769
    movk    x10, #0x8b14, lsl #16
    movk    x10, #0xbf0a, lsl #32
    movk    x10, #0x4005, lsl #48
    cmp     x9, x10
    b.eq    21f
    US_EXIT 0x10
21:
    mrs     x9, fpsr
    mov     x10, #(1 << 27)
    cmp     x9, x10
    b.eq    22f
    US_EXIT 0x11
22:
    mrs     x9, fpcr
    mov     x10, #0x00c00000
    cmp     x9, x10
    b.eq    23f
    US_EXIT 0x12
23:
```

- [ ] **Step 6: m4-1 自测验证（FP 断言生效）**

Run: 同 Task 2 Step 5 命令
Expected: `[TEST] PASS aarch64_user_signal` 且 `failed=0`。若 FP 恢复缺失/错位，fixture 以 0x0f–0x12 退出、用例 FAIL——FAIL 即测试在起作用，修复内核后复跑至 PASS。

- [ ] **Step 7: Commit**

```bash
git add kernel/Makefile kernel/selftest/fixtures/aarch64_user_signal.S
git commit -m "test(aarch64): pin FP round-trip through signal delivery and sigreturn"
```

### Task 4: ③ test_fp 接入产品盘与 user-runtime fp case

**Files:**
- Modify: `mk/components/image.mk:269,295-303`（aarch64 产品盘 prereq + mcopy）
- Modify: `user/test_fp.c:92-94`（nanosleep 容忍注释）
- Modify: `mk/components/run.mk:382-403`（`USER_RUNTIME_CASES_VALID`、prep/run 分支）
- Modify: `qemutests/aarch64_user_runtime.py`（choices、`_run_product_cpu`、`parse_product_boot_evidence`、`run_boot` 矩阵分支）
- Modify: `docs/build/build.md:80-85`（user-runtime 说明行）

**Interfaces:**
- Consumes: `$(USER_ARTIFACT_DIR)/test_fp.elf`（user 分组规则统一构建，`mk/components/user.mk:35`）；`$(AARCH64_UEFI_DISK)`；product case 流程（`_run_product_cpu`/`parse_product_boot_evidence`）。
- Produces: `USER_RUNTIME_CASE=fp`——`test-aarch64 MODE=user-runtime` 新合法值；harness 断言串口出现独立 `test_fp: PASS` 行。

- [ ] **Step 1: test_fp.c 容忍 aarch64 无 nanosleep**（`check_fork_and_switch` child 的 `nanosleep` 行改为）：

```c
        /* aarch64 hides the time syscall group (roadmap 能力齐平):
         * nanosleep returns -ENOSYS immediately, which is fine — the
         * exit path itself forces the switch under test. */
        (void)nanosleep(&ts, NULL);
```

- [ ] **Step 2: image.mk 产品盘加装 test_fp**——prereq 行（269）的 userland 过滤里 `terminal.elf` 之后追加 `$(USER_ARTIFACT_DIR)/test_fp.elf`；product mcopy 块 `::/bin/terminal` 行之后追加：

```make
	@# FP/SIMD runtime smoke (2026-11-21): run as /bin/test_fp from the
	@# busybox ash in USER_RUNTIME_CASE=fp; extension-less like /bin/init.
	@mcopy -i $@ $(USER_ARTIFACT_DIR)/test_fp.elf ::/bin/test_fp
```

- [ ] **Step 3: run.mk 放行 fp case**（三处）：

```make
USER_RUNTIME_CASES_VALID := irq asid smp product fp
```
prep 分支：
```make
_test-aarch64-prep-user-runtime:
ifeq ($(USER_RUNTIME_CASE),$(filter $(USER_RUNTIME_CASE),product fp))
	$(MAKE) aarch64-uefi
else
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_USER_RUNTIME_TEST=$(USER_RUNTIME_CASE) aarch64-uefi
endif
```
run 分支的 `--image` 与 `--elf` 两个 `$(filter product,...)` 改为 `$(filter product fp,...)`。

- [ ] **Step 4: harness 支持 fp**——五处：
  1. `main` 的 `--case` choices 加 `"fp"`；
  2. `audit_elf`：`if case == "product":` → `if case in ("product", "fp"):`（fp 的 kernel ELF 是产品内核，走 forbidden-prefix 检查而非 variant 的 REQUIRED_SYMBOLS）；
  3. `run_boot`：`if args.case == "product":` → `if args.case in ("product", "fp"):`（矩阵循环复用；`variant` 行不动，fp 落 `user-runtime-fp`）；
  4. `_run_product_cpu`（函数已持有 `args`，无需新参数）：`check_shell_io = (cpus == 2) or (args.case == "fp")`；shell IO 段按 case 分叉——fp 顺序为 `send(b"/bin/test_fp\n")` → `wait_for(lambda text: any(l.strip() == "test_fp: PASS" for l in text.splitlines()))` → `send(b"exit\n")` → 原有 respawn banner 等待不变；末尾 `parse_product_boot_evidence(text, check_shell_io=check_shell_io, test_fp_pass=(args.case == "fp"))`；
  5. `parse_product_boot_evidence` 加参数 `test_fp_pass=False`：为真时以独立行 `test_fp: PASS` 断言替代 `hi` 断言（banner≥2/prompt≥2 检查共用）。

- [ ] **Step 5: build.md 文档行**（user-runtime MODE 说明处补 `fp` case 与期望输出 `test_fp: PASS`，对齐既有 irq/asid/smp/product 行文）。

- [ ] **Step 6: 静态回归**

Run: `TMPDIR=/tmp make test-host -j$(nproc) && make test-static`
Expected: 全绿（重点 `test_harness_static`/`test_make_qemu_failure` 对 run.mk 的 pin）。

- [ ] **Step 7: fp case 实跑（本任务核心证据）**

Run: `TMPDIR=/tmp make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=fp`
Expected: `PASS: user-runtime case=fp cpus=1,2,4,8 (...)`；每核串口见 `/bin/test_fp` 输出独立行 `test_fp: PASS`。

- [ ] **Step 8: product 回归**

Run: `TMPDIR=/tmp make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product`
Expected: `PASS: user-runtime case=product cpus=1,2,4,8`（新增 /bin/test_fp 不影响启动与 shell IO）。

- [ ] **Step 9: Commit**

```bash
git add user/test_fp.c mk/components/image.mk mk/components/run.mk qemutests/aarch64_user_runtime.py docs/build/build.md
git commit -m "test(aarch64): run test_fp on the product disk as USER_RUNTIME_CASE=fp"
```

### Task 5: 文档闭环

**Files:**
- Modify: `docs/roadmap.md:55`（P2 项改写）与该项附近适当位置留 x86 后续任务
- Modify: `docs/arch.md`（§aarch64 用户态现状「仍保留的边界」条目）
- Modify: `docs/changelog.md`（当日条目）

**Interfaces:** 无代码接口；行文锚点以 spec 为准。

- [ ] **Step 1: roadmap.md:55 改写**——「已落地部分」保持；「剩余」段替换为：sigframe FP 区（regs@0/blocked@272/reserved@280/fp@288、816 B）、EC 0x07 fatal 兜底、`USER_RUNTIME_CASE=fp` 运行时证据；删除「F 位掩码导致陷入」与「EC 0x2C/0x2D」错误表述（0x2C=FP_EXC64、0x2D=GCS，均非访问陷入类）；追记 x86 signal 帧同样无 FPU 区、另列后续（不在本项）。

- [ ] **Step 2: arch.md「仍保留的边界」改写**——删除「EL0 浮点指令仍会陷入」句，改为「EL0 FP 已实测可跑（USER_RUNTIME_CASE=fp）；sigreturn 已保存/恢复 FP（sigframe v2 816 B）」，EC 表述同步修正。

- [ ] **Step 3: changelog 条目**——按现有当日格式（先读 `docs/changelog.md` 头部确认锚点风格；若已有 2026-11-21 条目则追加小节，否则新建）记录：sigframe v2、EC 0x07 fatal、fp case、链接 spec/plan。

- [ ] **Step 4: Commit**

```bash
git add docs/roadmap.md docs/arch.md docs/changelog.md
git commit -m "docs: close the EL0 FP/SIMD remaining-items ledger item"
```

### Task 6: 最终验收门

**Files:** 无新改动（纯验证；发现问题回到对应 Task 修复后重跑本门）。

- [ ] **Step 1: 宿主与静态全量**

Run: `TMPDIR=/tmp make test-host -j$(nproc) && make test-static`
Expected: 全绿。

- [ ] **Step 2: m4-1 全量自测**

Run: 同 Task 2 Step 5 命令（`timeout 300 make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi > /tmp/m41-final.log 2>&1 || true` 后 grep）
Expected: `[TEST] END suite=kernel-selftest total=28 passed=28 failed=0 skipped=0`（含 `aarch64_user_signal` FP 往返）。

- [ ] **Step 3: fp + product 双 case**

Run: `TMPDIR=/tmp make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=fp && TMPDIR=/tmp make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product`
Expected: 两条 `PASS: ... cpus=1,2,4,8`。

- [ ] **Step 4: 工作区核对**

Run: `git status --short && git log --oneline master..HEAD`
Expected: 干净工作区；5 个 feature commit + 文档 commit 依次排列。

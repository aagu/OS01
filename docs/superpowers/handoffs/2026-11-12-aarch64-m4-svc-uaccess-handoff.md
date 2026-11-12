# Handoff — AArch64 M4 正式 SVC、异常分派和 uaccess

日期：2026-11-12。执行方式：`superpowers:executing-plans` 内联（native，非 worktree / 非子代理）。

## 工作区与状态

- 仓库：`/home/aagu/OS01`（主工作区直接实施）。
- 分支：`feat/aarch64-m4-svc-uaccess`，从 `master` 的 `35ed5f9a` 创建。
- HEAD = `c48c9954`（任务 2 完成）。**工作区干净**，`git status` 无输出。
- 计划：`docs/superpowers/plans/2026-11-12-aarch64-m4-svc-uaccess.md`（4 个 task）。
- Spec：`docs/superpowers/specs/2026-11-12-aarch64-m4-svc-uaccess-design.md`（§15–20 原生 SVC ABI 与 syscall 表、§22–28 同步异常分派、§29–34 正式 uaccess 契约）。
- 提交策略：**每个 task 一项 commit**，共 4 项，落在上述 feature 分支；合并回 master 用 `git merge --ff`（见 AGENTS.md）。
- 本仓库无 `.superpowers/sdd/` 台账文件，本 handoff 即为唯一进度真源。

## 已完成并通过验证的部分

| 任务 | commit | 证据 |
|---|---|---|
| 任务 1　共用四个 syscall 表项及处理函数 | `1ea0b7f9` `feat(syscall): share four native syscall handlers across architectures` | `make PROFILE=x86_64-clang kernel.bin` rc=0；`make PROFILE=x86_64-clang test-syscall-boundary` PASS；`make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall` → `PASS: all 118 syscall tests passed`；TDD RED 已实证（原表上子集二进制报 `shared syscall 2 did not reach its own handler`） |
| 任务 2　正式低 EL 同步事件进入公共分派 | `c48c9954` `feat(aarch64): route formal low-EL sync events through common dispatch` | 解码用例 129/129、后端用例 132/132；`python3 -m unittest qemutests.test_intr_dispatch_audit` rc=0；aarch64 普通内核构建 rc=0；`make PROFILE=aarch64-clang test-aarch64-audit` → `PASS: intr-dispatch-audit` / `PASS: stack-frame-audit`；EL0 自测变体构建 rc=0、QEMU 自测 `total=21 passed=21 failed=0`；`make PROFILE=x86_64-clang test-header-object-audit` PASS |

**计划文末定向宿主命令在当前 HEAD 实测退出码 0**（2026-11-12 复核，输出：syscall dispatch PASS ×2、`test_compat_linux: PASS`、nofile 345/345、解码 129/129、后端 132/132）：

```sh
make -s -f Makefile -f - PROFILE=x86_64-clang m4-svc-host-check <<'MAKE'
.PHONY: m4-svc-host-check
m4-svc-host-check:
	$(call os01_submake,hosttests,test-syscall-dispatch test-compat-linux test-nofile-limits $(HOST_TEST_BUILD_DIR)/test_intr_decode_aarch64.elf $(HOST_TEST_BUILD_DIR)/test_intr_backend_aarch64.elf)
	$(HOST_TEST_BUILD_DIR)/test_intr_decode_aarch64.elf
	$(HOST_TEST_BUILD_DIR)/test_intr_backend_aarch64.elf
MAKE
```

### 任务 1 留下的既有形态（接手者不要重构）

- `kernel/syscall/sys_native.c` 是**架构中立**的新单元，含 `int64_t sys_native_{exit,getpid,getppid,clock_gettime}(syscall_ctx_t *ctx)`。它**不新增公开头**，与 `sys_fs.c`/`sys_proc.c` 家族一致，在 `dispatch.c` 中前置声明。
- `kernel/syscall/dispatch.c` **只有一份**固定编号与名称的表。四项（`SYS_exit`/`SYS_getpid`/`SYS_getppid`/`SYS_clock_gettime`）放在 `#ifndef OS01_SYSCALL_A64_SUBSET` **之外**，其余 73 项在条件块内。AArch64 / 定向宿主子集配置用 `OS01_SYSCALL_A64_SUBSET=1` 隐藏其余项。
- 表项文本形态必须保持 `[SYS_x] = { handler, "name" }`：`qemutests/syscall_boundary_audit.py` 按源文本正则检查 77 项、trace 名一致性、仅 `getpeername` 可为 NULL、76 个已实现 handler。
- `syscall_dispatch()` 保留「空上下文 / 缺项 → `-EINVAL`」契约；**所有 x86 表项保留**。
- `sys_native_exit()` 把 `(ctx->args[0] & 0xff) << 8` 交给 `__attribute__((noreturn)) void arch_syscall_exit(uint64_t status)`。x86 实现在 `kernel/arch/x86_64/intr/syscall_frame.c`（写 `current->exit_code` → `do_exit()`；意外返回则 `arch_local_irq_disable()` + `hlt` 停机，满足 `_Noreturn`）。**AArch64 实现留到任务 3。**

### 任务 2 留下的既有形态（接手者不要重构）

- `kernel/include/arch/aarch64/sync_fault.h` 是 **ESR_EL1.ISS 布局的唯一真源**：FSC mask `0x3f`、WnR bit6、**S1PTW bit7**、CM bit8、FnV bit10、EC 常量、SPSR 模式常量、FSC 白名单区间 `0x04..0x0f`。
- `kernel/include/arch/aarch64/user_task.h` 中的 `static inline bool aarch64_user_frame_matches(regs, expected_frame, mm_root_pa, image_root_pa, installed_root_pa)` 是**纯谓词**（EL0t + 帧地址精确相等 + 三个非零根相等），可被宿主用例直接调用/覆盖。
- `aarch64_user_frame_is_valid()`（`kernel/arch/aarch64/cpu/user_task.c`）是**运行时准入**：BSP per-CPU 指针、`scheduler_ok`、当前任务是 `PF_PROCESS` 的正式用户任务、所有者存在、`current->mm` 与映像一致、硬件 TTBR0 与三个根一致。`__attribute__((no_stack_protector))`。
- `aarch64_user_sync_frame()` = `aarch64_user_task_frame(current)` = `stack_top - PT_REGS_SIZE`（`restore_pt_regs` 在 `eret` 前把 SP_EL1 抬回 `stack_top`，故异常帧恰落在 `stack_top - PT_REGS_SIZE`）。已由 EL0 QEMU 自测 21/21 实证。
- `aarch64_user_current_mm()` 返回当前正式用户任务的 `mm`（任务 3 的 uaccess 要用）。
- 正式 EL0 分派路径：`aarch64_el0_sync_route()` → `arch_intr_dispatch()` → `arch_intr_handle_sync()` → `aarch64_user_task_sync(regs, event)`。事件只活在当前任务栈（route 的局部变量），不落全局。
- 后端 EC/模式一致性：EL1h 帧禁止 lower-EL abort EC（0x20/0x24），EL0t 帧禁止 same-EL abort EC（0x21/0x25）；其它 EC 放行。EL1 分支保留原 fatal / bad-event。
- 旧探针优先分支保留原 bool 返回，**只有该分支**可以使用旧 continuation。

## 任务 3 当前状态（进行中：**仅完成勘察，尚未写任何代码**）

计划任务 3 的文件清单：`kernel/arch/aarch64/memory/{uaccess.c,user_copy.S}`、`kernel/include/arch/aarch64/uaccess.h`、`kernel/arch/aarch64/intr/{entry.S,trap.c,syscall.c}`、`kernel/include/arch/aarch64/syscall.h`、`kernel/arch/aarch64/cpu/user_task.c`、`kernel/Makefile`、任务 2 的审计用例。

### 已勘察的现状事实（省接手者时间）

| 事实 | 位置 |
|---|---|
| `kernel/arch/aarch64/memory/uaccess.c` **整个文件**被 `#if AARCH64_EL0_TEST` 包住，目前只有旧探针的 `aarch64_copy_{from,to}_user()` 与 `aarch64_uaccess_fixup()` | 需拆分为「普通镜像正式实现」+「测试宏内的旧探针分支」 |
| `kernel/arch/aarch64/memory/user_copy.S` **整个文件**被 `#if AARCH64_EL0_TEST` 包住；含真实 `LDTRB`/`STTRB` 循环、指令标签 `aarch64_uaccess_load`/`aarch64_uaccess_store`、跳板 `aarch64_uaccess_fault` | 真实指令、标签与跳板要编入**普通镜像**（标签必须与正式窗口登记的 PC 精确匹配） |
| `kernel/include/arch/aarch64/uaccess.h` 目前只声明探针 API（`aarch64_copy_*_user`、`aarch64_copy_*_user_raw`、`aarch64_uaccess_fixup`） | 需补正式 API 与窗口登记结构 |
| `kernel/include/memory/uaccess.h` 已声明 `copy_to_user_ft_res()`/`copy_from_user_ft_res()`/`syscall_check_user_range(addr,len,writable)` 及 `copy_*_ft` inline 包装 | aarch64 侧必须提供这三个符号的实现（x86 实现在 `kernel/memory/uaccess.c`，该文件**不进 aarch64 构建**） |
| `arch_user_range_accessible()` 的 aarch64 实现在 `kernel/arch/aarch64/memory/user_root.c`；签名 `void *pgtbl`（传的是直映后的根指针，不是 PA） | — |
| `AARCH64_USER_MIN=0x1000`、`AARCH64_USER_LIMIT=(1ULL<<48)`、`ARCH_PAGE_OFFSET=0xffff000000000000` | `kernel/include/arch/mmu.h` aarch64 段 |
| `aarch64_pt_root_publish()` / `aarch64_pt_root_unpublish_local()` / `aarch64_pt_root_is_published(const uint64_t *root)` | `kernel/include/arch/aarch64/vmm_gate.h` |
| `mm_struct.pgdir` 是 `uint64_t *`，注释说明 aarch64 上即 TTBR0 根 PA | `kernel/include/sched/task.h` ~line 90 |
| `aarch64_user_image_root_pa(image)` / `aarch64_user_image_mm(image)` | `kernel/arch/aarch64/memory/user_image.c` |
| `kernel/arch/aarch64/intr/syscall.c` **不存在**；`kernel/include/arch/aarch64/syscall.h` **不存在** | 任务 3 新建 |
| `arch_syscall_exit()` **只有 x86 实现**，aarch64 无定义 | 任务 3 在 `user_task.c` 转交既有 `aarch64_user_task_exit()` |
| `kernel/Makefile` aarch64 白名单在 70–119 行，是**显式列表**（不是 wildcard）；x86 侧用 `$(wildcard syscall/*.c)` 自动包含 | 任务 3 需把 `syscall/dispatch.c`、`syscall/sys_native.c` 加入 aarch64 白名单 |
| `ARCH_SOURCE_DIRS := boot intr memory smp cpu platform runtime` | `kernel/arch/aarch64/cpu/user_task.c`、`intr/syscall.c`、`memory/uaccess.c` 都在普通 aarch64 镜像内；`user_copy.S` 由 `$(ARCHDIR)/memory/*.S` wildcard 收集 |
| `kernel/arch/aarch64/intr/entry.S`：`el1_sync_entry` 调 `aarch64_el1_sync`（普通镜像下不返回，落到 `daifset #0xf` + WFI 兜底）；`el0_sync_entry` 调 `aarch64_el0_sync_route`，返回 true → `restore_pt_regs` | 任务 3 要让普通 `el1_sync_entry` 在 `aarch64_el1_sync()` 返回后恢复帧 |
| `aarch64_user_task_sync()` 现有 immediate 分支：YIELD(0x4d40) / EXEC(0x4d41) / EXIT(0x4d42)，其余 `-ENOSYS` | 任务 3 在此增加 immediate 0 → `aarch64_user_syscall()` |
| `arch_local_irq_save()`/`arch_local_irq_restore()` 在 `kernel/include/arch/irq.h` 的 aarch64 段可用（DAIF 语义） | — |

### 已定的设计决策（照做即可）

1. **正式用户复制入口**（`kernel/arch/aarch64/memory/uaccess.c`）：
   - `n == 0` 直接成功返回 0，指针可为空，无故障回调。
   - 非零长度须可用 `ssize_t` 表示；范围须在 `[AARCH64_USER_MIN, AARCH64_USER_LIMIT)` 内，用**减法**检查结束边界（`len > LIMIT - addr`）。
   - 根 PA **只**从当前正式任务的 `mm->pgdir` 取：`uint64_t root_pa = (uintptr_t)mm->pgdir`。
   - 开窗**前**用 `aarch64_pt_root_is_published(&root_pa)` 查登记；再 `(void *)Phy_To_Virt(root_pa)` 交给 `arch_user_range_accessible()` 校验**全范围**（读/写方向）。
   - 普通调用先 `arch_local_irq_save()`；**发布表与页表查询可能取锁，所有锁必须在开窗前释放**；随后才打开 BSP 私有窗口。
   - 窗口登记：当前任务、`mm`、根 PA、方向、半开用户范围。**拒绝嵌套**且保留外层窗口。
   - 复制返回后**先清本次窗口，再恢复调用方的 IRQ 状态**，再返回全长或 `-EFAULT`。
   - `on_fault` 回调**至多一次**，且在恢复 IRQ 状态**之后**才调用。
   - 预检失败**不写入**；硬件 abort 后的已复制前缀可以存在，调用方以返回值判断整次操作是否成功。
2. **正式 EL1 修复**（`aarch64_uaccess_fixup()` 改为「先纯谓词、后窗口」）：
   - 纯谓词部分只比较入参：ESR（EC= same-EL Data Abort、FSC ∈ `0x04..0x0f`、非 S1PTW、非 CM、FAR 有效）、`SPSR_EL1 & 0x1f` **完整 M[4:0]** 等于 EL1h（0x05）、故障 PC 精确等于当前方向的用户访存标签、WnR 与方向一致、FAR 等于该指令保存的用户指针**且落在窗口范围内**。
   - 匹配后才读活动窗口与运行期状态：当前任务仍持有窗口中的 `mm`，`mm->pgdir` 与硬件 `TTBR0` 均与登记的根 PA 一致。
   - **正式修复不查发布表或页表**（发布表仅在建窗前查询）。
   - 只修改异常帧 ELR 到 `-EFAULT` 跳板；跳板返回到 C 包装器清窗。
   - 内核缓冲区访存指令（普通 `LDRB`/`STRB`）或窗口外访问**不恢复**，维持致命路径。
   - 旧探针的复制和修复分支继续受测试宏及活动探针约束。
3. **SVC #0 接线**（新建 `kernel/arch/aarch64/intr/syscall.c` + `kernel/include/arch/aarch64/syscall.h`）：
   - `void aarch64_user_syscall(pt_regs_t *regs)`：用**完整 x8** 调 `syscall_has_handler()`；无处理函数则在入口写 `x0 = -ENOSYS`。
   - 从 x0–x5 组装 `syscall_ctx_t.args[0..5]`；`ctx.nr = regs->x8`；`ctx.arch_frame = regs`；`ctx.suppress_writeback = false`。
   - 调 `syscall_dispatch(&ctx)` 后，按 `ctx.suppress_writeback` 决定是否写回 `x0`。
   - 成功/错误都保留其它 GPR、SP、NZCV 与硬件保存的返回 PC，**不再手动递增 PC**。
   - 由 `aarch64_user_task_sync()` 的 **immediate 0** 分支调用（内部 immediate 0x4d40/0x4d41/0x4d42 语义不变；旧探针变体活跃时仍优先由旧回调解释 immediate）。
4. **`arch_syscall_exit()`（aarch64）**：在 `kernel/arch/aarch64/cpu/user_task.c` 转交既有 `aarch64_user_task_exit()`。
5. **宿主纯 abort 匹配负例**：承载在 `hosttests/cases/test_intr_decode_aarch64.c`（计划指定），覆盖实际 bit 7 / CM / FnV / 错模式 / 错 EC-FSC / 错 PC / WnR 与窗口方向不符 / 错操作数 FAR / 越窗 FAR / 无窗口 / 不同任务·mm·根。**匹配器只比较入参，测试不得伪造硬件恢复证据。**

### 任务 3 剩余步骤清单（对照计划逐条）

- [ ] 在既有 AArch64 宿主解码测试中覆盖共用的纯 abort 匹配（含全部拒绝条件）。
- [ ] `uaccess.c` 实现 `syscall_check_user_range()` 与两个 `copy_*_ft_res()`（`n==0` 直成功；irq_save；根 PA；发布表查询；全范围预检；开窗；登记；清窗；恢复 IRQ；`-EFAULT`；回调至多一次；拒绝嵌套）。
- [ ] `user_copy.S` 的真实 LDTRB/STTRB、指令标签、故障跳板编入普通镜像；`aarch64_uaccess_fixup()` 改为「纯 ESR/SPSR/PC/FAR 谓词 → 正式窗口与当前任务/mm/硬件根匹配」；旧探针分支仍受测试宏约束；内核缓冲区普通访存维持致命。
- [ ] 普通 `el1_sync_entry` 在 `aarch64_el1_sync()` 返回后恢复帧；`aarch64_el1_sync()` 只有命中修复点才返回，其余维持原 fatal/bad-event。
- [ ] 扩展早期链审计与合成 fixture：正向检查 fixup 与恢复边；把修复的早期可达 helper 纳入无锁 / 无栈 canary / 无普通日志审计。
- [ ] `intr/syscall.c` 实现 `aarch64_user_syscall()`；`user_task.c` 的 immediate 0 分支调用它；aarch64 `arch_syscall_exit()` 转交 `aarch64_user_task_exit()`；`kernel/Makefile` aarch64 白名单加 `syscall/dispatch.c`、`syscall/sys_native.c`；构建普通与自测配置。
- [ ] 审计：正向检查 `aarch64_user_task_sync → aarch64_user_syscall → syscall_dispatch`（接受编译器生成的普通调用或尾调用边）；审计普通产物中的复制符号与 EL1 修复/恢复边；**排除**旧探针复制包装器 `aarch64_copy_from_user`、`aarch64_copy_to_user`，以及新增的 `aarch64_uaccess_test_copy`、`aarch64_user_svc_uaccess_test_hook` 和 fixture ELF 符号。

## 任务 4 待办（一次单核 QEMU 验收）

计划任务 4 的文件清单：`kernel/selftest/test_aarch64_user_svc_uaccess.c`、`kernel/selftest/fixtures/aarch64_user_svc_uaccess.S`、`kernel/Makefile`、`kernel/selftest/{selftest,test_aarch64_scheduler}.c`、`kernel/include/arch/aarch64/boot.h`、`kernel/arch/aarch64/{intr/syscall.c,memory/uaccess.c}`、`kernel/include/selftest/aarch64_user_svc_uaccess.h`、`docs/build/build.md`。

要点：复用第 2 块的 ELF 嵌入规则与链接布局；RW 段覆盖连续两页、紧邻第三页留空；正式任务用 `clock_gettime` 把 16 字节结构写到两页边界；向代码页写回、向 RW 段末尾跨空页写回均返回 `-EFAULT`（后者保留前缀哨兵）；fixture 把 PID/PPID 存入 x19/x20；测试观察点在 PID 首次调用 `clock_gettime` 时读回两值；验证未知 immediate、未开放编号、高位污染 x8；用原生 exit 退出；在 `aarch64_sched_observe_reap()` 入口加测试专用 PID 过滤回调（放在既有 `reclaim_batch_active` 判断**之前**），核对 `PF_SELF_REAP` 与编码后的 `exit_code`；复用第 2 块 guard fault 回归。

变体期望：注册单个 late case `aarch64_user_svc_uaccess`，变体 late 期望从 **14 改为 15**。

串口要求新增 PASS 及 `[TEST] END suite=kernel-selftest total=22 passed=22 failed=0 skipped=0`（当前基线是 21）。

## 关键约束（违反即回归）

**AGENTS.md 硬性项**

- 源目录 ↔ 头目录一一对称；头统一在 `kernel/include/` 单一根解析（`-Iinclude`），`#include <subsys/foo.h>`。禁止头散落源目录旁。
- 禁止裸 `serial_printk`/`color_printk`，一律 `log_err`/`log_warn`/`log_info`/`log_debug`。
- `make test-syscall` **不得**带 `KERNEL_SELFTEST=1`（会引发假 fork hang）。
- `GS base` 只设一次；`get_current_task()` 用 `RSP & ~(STACK_SIZE-1)`。
- `Phy_To_Virt()` 必须先于解引用。
- `set_intr_gate_raw` 只收汇编 stub。

**仓库审计项**

- `qemutests/syscall_boundary_audit.py`：`kernel/syscall/**/*.c` 禁止 include per-arch 头、禁止 `pt_regs_t`/`struct pt_regs`、禁止访问架构寄存器字段；dispatch 表须 77 项、trace 名一致、仅 `getpeername` 可 NULL、76 个已实现 handler；`docs/syscall/syscall.md` 编号表须与 UAPI 一致。
  → **注意**：`kernel/syscall/sys_native.c` 必须保持架构中立（不得 include per-arch 头），这正是它把退出委托给 `arch_syscall_exit()` 的原因。
- `qemutests/intr_dispatch_audit.py`：`_A64_EARLY_CHAIN = ("arch_intr_dispatch", "aarch64_intr_decode_sync", "aarch64_el1_sync", "arch_intr_handle_sync", "arch_intr_bad_event")`，其直接调用边不得命中禁用符号（`log_`/`percpu_`/`get_current`/`schedule`/`kmalloc` 等前缀或精确名），且每条边必须在 ELF 符号表可解析；`arch_intr_handle_sync` 必须调用 `aarch64_intr_decode_sync`、`aarch64_intr_bad_event`、`aarch64_el1_sync_fatal`；`arch_intr_handle_irq` 必须恰好调用一次 `gic_dev_dispatch`。
  → 任务 3 把 fixup 接入 `aarch64_el1_sync` 后，**该函数的早期可达 helper 必须仍满足无锁 / 无栈 canary / 无普通日志**审计（fixup 走 `no_stack_protector` + 纯谓词）。
- `qemutests/run_hosttests.py`：非 `--exit-status` 白名单的宿主二进制必须有非空证据输出。
- `hosttests/mk/registry.mk`：`register_host_test(id, group, acceptance)`；group ∈ {framework, libc, memory, sched, fs, intr, drivers, net, display, terminal, monitor, gui, doom}；acceptance ∈ {protocol-or-legacy, exit-status, expected-failure}。
- `qemutests/test_host_build_contract.py`：registry ID 唯一、inventory 与 registry 一致、不同 mock 宏必须产出不同 object 文件。

## 已验证可用的命令形状

```sh
# 宿主用例（直接进 hosttests 目录构建）
make -C hosttests OS01_PROFILE_FILE="$(pwd)/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang \
  "$(pwd)/build/x86_64-clang/host-test/test_intr_decode_aarch64.elf" \
  "$(pwd)/build/x86_64-clang/host-test/test_intr_backend_aarch64.elf"
./build/x86_64-clang/host-test/test_intr_decode_aarch64.elf
./build/x86_64-clang/host-test/test_intr_backend_aarch64.elf

# aarch64 普通内核 + 产物审计
make PROFILE=aarch64-clang aarch64-uefi-kernel
make PROFILE=aarch64-clang test-aarch64-audit

# aarch64 EL0 自测变体（构建 + QEMU，timeout 结束属正常）
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi
timeout 90s make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi

# x86 内核 / 源审计 / syscall E2E
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test-syscall-boundary
make PROFILE=x86_64-clang test-header-object-audit
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall

# 合成审计单测
python3 -m unittest qemutests.test_intr_dispatch_audit
```

## 已知坑

- **stale sysroot lease**：`build/x86_64-clang/leases/654792.kernel` 曾在 11:08 留下属主已死的 lease，阻塞 sysroot 发布，报 `ERROR: sysroot generation leases still held after 60s`。清理前**先确认属主进程已死**，再用 `/home/aagu/.minimax/bin/mavis-trash` 移入回收站（不要用裸 `rm`）。
- `hosttests/cases/test_compat_linux.c` 有 5 处既有 `%lu` vs `uint64_t` 格式告警（HEAD 即存在，本次未改格式串）——不必在任务 3 顺手改，会扩大 diff。
- aarch64 串口行首带 `\r`；runner 的 END gate 不用 `^` 锚。
- 结构体/宏改动后建议 `make clean`（无头依赖），否则可能出现陈旧产物。
- 宿主 mock 的 `arch_cpu_halt` 在 armed 时 longjmp（生产 fail-stop 不返回）。
- 任务 3 会新增 `kernel/syscall/{dispatch,sys_native}.c` 到 aarch64 构建 —— `sys_native.c` 依赖 `current`（`sched/task.h`）、`copy_to_user_ft()`（`memory/uaccess.h`）、`arch_clocksource_read_ns()`（`arch/clocksource.h`）。**aarch64 构建不编 `kernel/memory/uaccess.c`**，因此 `copy_to_user_ft_res()` 必须由 `kernel/arch/aarch64/memory/uaccess.c` 提供，否则链接缺符号。

## 完成后仍需执行的收尾

1. 计划文末定向宿主命令退出码确认（当前 HEAD 已实测 rc=0，改动后需复跑）。
2. `git diff --check`（无输出为通过）。
3. 四个 task 对应四个 commit；提交信息风格沿用近期 log：`feat(aarch64): …` / `test(aarch64): …` / `docs(aarch64): …`。

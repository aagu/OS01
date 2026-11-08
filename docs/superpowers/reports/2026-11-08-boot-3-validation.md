# BOOT-3 启动失败处理与能力降级验收报告

**日期**: 2026-11-08  
**分支**: `master`  
**实施 Plan**: `docs/superpowers/plans/2026-11-08-boot-3-failure-policy.md`  
**状态**: 全部 5 个任务通过验收 (ALL GATES PASSED)

---

## 1. 核心架构与契约保证

BOOT-3 建立了确定性的启动失败与降级策略，消除了启动过程中的静默吞错、隐式继续执行及损坏状态泄漏到用户态的隐患：

1. **注册完整性 (Registration Integrity)**:
   - 彻底修复 `register_subsys` 与 `register_subsys_percpu` 对 `-EEXIST`、`-EINVAL`、`-ENOSPC` 错误的忽略。
   - `arch_register_subsys()` 与 `arch_register_subsys_percpu()` 捕获首个注册错误并立即锁存。若注册表有任一错误，直接触发 `boot_failure_stop("registration", name, rc, -1)`，绝不进入 Phase 3+ 阶段。

2. **强归因致命停止与能力降级 (Structured Fatal Halt & Capability Degradation)**:
   - **必需子系统 (`required-subsys`)**: `subsys_init_phase` 检测到非 optional 错误时，调用 `boot_failure_stop("subsys", entry->name, rc, -1)` 立即终止，并打印 `[boot-3] FATAL stage=subsys component=<name> rc=<rc>`。
   - **可选子系统 (`optional-subsys`)**: 发生错误时调用 `boot_failure_warn("subsys", entry->name, rc, -1)` 打印 `[boot-3] WARN ...`，记录内部状态后降级继续。
   - **状态与原始返回查询**: `subsys_status()` 明确返回 `1` (OK), `-1` (FAIL), `0` (UNINIT), `-ENOENT` (`-2`, UNKNOWN)；`subsys_result()` 返回 `0` 或原始负数错误码。
   - **Per-CPU 子系统**: 统一支持 required 致命停止与 optional 警告降级，错误信息包含 `cpu=<cpu_id>`。

3. **根文件系统与终端能力门限 (Root & Terminal Preconditions)**:
   - 根文件系统 `/` 挂载为必需能力；挂载失败直接调用 `boot_failure_stop("root", "/", rc, -1)`。
   - 辅助挂载点 `/proc`, `/dev/shm`, `/tmp` 明确为可选降级能力；挂载失败调用 `boot_failure_warn("mount", target, rc, -1)` 并继续执行。
   - 终端能力：控制台 TTY (`tty_boot_init`) 和伪终端 master (`pty_init`) 是交互用户态启动的前置条件，分配或注册失败直接触发 `boot_failure_stop`。

4. **初始任务 stdio 表与继承完整性 (Initial Stdio & Inheritance Barriers)**:
   - `sched_boot_stdio_init()` 负责在用户态启动前为初始任务构建前三项文件描述符（fd 0, 1, 2）绑定到 `/dev/tty`。任何分配或安装失败直接触发 `boot_failure_stop("stdio", ...)`。
   - `task_init()` 在派生 PID 1 `/bin/init` 前校验 stdio 初始化状态与 `/bin/init` 存在性；在任务创建时严格保证 `files_dup` 拷贝的完整性。若子进程 fd 表复制失败，直接调用 `boot_failure_stop("init", "/bin/init", rc, -1)` 停机，绝不启动无 stdio 或继承损坏的 PID 1。

5. **内存与日志格式化安全 (Zero-Allocation & Dual-Arch String Formatting)**:
   - `boot_failure_stop` 与 `boot_failure_warn` 零堆内存分配，无需 `kmalloc`/`malloc`。
   - 针对 AArch64 早期微型格式化器（mini-formatter）仅稳定支持 `%s` 的约束，`boot_failure.c` 在栈上实现专用的无堆十进制整数格式化器（`_fmt_dec`，正确处理正负数和 `INT64_MIN`），格式化后统一以 `%s` 输出到 `log_err` / `log_warn`。
   - 致命停机调用 `arch_boot_fatal_halt()`（x86_64: `cli; for (;;) hlt;`；AArch64: `for (;;) wfi;`），不依赖已初始化的调度器，不在致命路径产生二次锁竞争或死锁。

---

## 2. 17 组 QEMU 启动矩阵验收结果

通过 `make PROFILE=x86_64-clang test-qemu SUITE=boot-failure` 完整运行全部 17 组 QEMU 测试，全部结果均为 PASS：

| 用例 (Case) | CPU 核心数 | 类型 | 判定标准 | 结果 | 归档日志 |
|---|---|---|---|---|---|
| `normal` | 1 | 正常基线 | 无 fault 标记，成功启动 PID 1 与用户态 | PASS | `logs/tests/boot-failure/20261108T053251Z-009f1e1bcadc42a6bf4bf3f6b2d290c3/` |
| `normal` | 2 | 正常基线 | 多核正常启动 PID 1 | PASS | `logs/tests/boot-failure/20261108T053254Z-b27e8524622b4baab640d995ff74ce0f/` |
| `normal` | 4 | 正常基线 | 4 核正常启动 PID 1 | PASS | `logs/tests/boot-failure/20261108T053257Z-888497faae5a4220b3eaef794689b9a6/` |
| `required-subsys` | 1 | 必需子系统致命 | 捕获 `[boot-3] FATAL stage=subsys component=boot3-required rc=-5`，无后哨兵/root/PID1 | PASS | `logs/tests/boot-failure/20261108T053303Z-62283a0058b84931a29352e008a0d24c/` |
| `required-subsys` | 2 | 必需子系统致命 | 双核捕获相同 FATAL 停机 | PASS | `logs/tests/boot-failure/20261108T053306Z-3665ffb433ce4bc688f98a2eb24c6536/` |
| `optional-subsys` | 1 | 可选子系统降级 | 捕获 `[boot-3] WARN stage=subsys component=boot3-optional rc=-5`，后哨兵证实 status=-1/raw=-5，正常启动用户态 | PASS | `logs/tests/boot-failure/20261108T053311Z-d88e0ec0ef7d4e5fbcda214fb0fbb595/` |
| `optional-subsys` | 2 | 可选子系统降级 | 双核捕获相同 WARN 降级并正常启动 | PASS | `logs/tests/boot-failure/20261108T053315Z-3d0cfcf1c1a94b59ab1314e30b6e921c/` |
| `registration` | 1 | 注册完整性致命 | 捕获 `[boot-3] FATAL stage=registration component=boot3-duplicate rc=-17`，不进入 Phase 3 | PASS | `logs/tests/boot-failure/20261108T053320Z-bc65476a6d6e42b29235e37330fe6df2/` |
| `root` | 1 | 根文件系统缺失 | 捕获 `[boot-3] FATAL stage=root component=/ rc=-5`，无 terminal/PID1 | PASS | `logs/tests/boot-failure/20261108T053346Z-a7a2fa26e5fc4666ae4a070eec335c05/` |
| `tty` | 1 | 控制台 TTY 缺失 | 捕获 `[boot-3] FATAL stage=terminal component=tty rc=-12`，无 stdio/PID1 | PASS | `logs/tests/boot-failure/20261108T053351Z-59695b2879504f21b790d1956e1841ca/` |
| `pty` | 1 | PTY master 缺失 | 捕获 `[boot-3] FATAL stage=terminal component=pty rc=-12`，无 stdio/PID1 | PASS | `logs/tests/boot-failure/20261108T053357Z-5cf29fe0275d4d3cbca71fc4ea7c36a6/` |
| `stdio` | 1 | stdio 分配失败 | 捕获 `[boot-3] FATAL stage=stdio component=boot_stdio rc=-12`，无 init-spawn/PID1 | PASS | `logs/tests/boot-failure/20261108T053402Z-ff7878347f074d289069dcfefcbe0c92/` |
| `stdio` | 2 | stdio 分配失败 | 双核捕获相同 FATAL 停机 | PASS | `logs/tests/boot-failure/20261108T053405Z-8e5c829e7c3c43fcab6ea53fe26987aa/` |
| `init` | 1 | init 文件不存在 | 捕获 `[boot-3] FATAL stage=init component=/bin/init_nonexistent rc=-1`，无 PID1/后续服务 | PASS | `logs/tests/boot-failure/20261108T053410Z-9feeb7fb1c2b4c1aa5e4f48b0c441c94/` |
| `init` | 2 | init 文件不存在 | 双核捕获相同 FATAL 停机 | PASS | `logs/tests/boot-failure/20261108T053413Z-e374ffc5db3847acab7018c6422b9186/` |
| `child-fds` | 1 | fd 表继承损坏 | 捕获 `[boot-3] FATAL stage=init component=/bin/init rc=-12`，无 PID1/后续服务 | PASS | `logs/tests/boot-failure/20261108T053417Z-b271d50c7fc04639b97779fdf85a1a11/` |
| `child-fds` | 2 | fd 表继承损坏 | 双核捕获相同 FATAL 停机 | PASS | `logs/tests/boot-failure/20261108T053420Z-ef84a1a0ae5047b19ce80261053744dc/` |

### 变体构建与镜像隔离审计
- `boot-failure.normal.before` 与 `boot-failure.normal.after` 的 sha256 校验完全一致，证明 14 组故障测试从未污染普通生产镜像。
- 普通生产 ELF 符号审计：`llvm-nm` 验证普通 `kernel.elf` 绝不包含任何故障变体标识或测试夹具注入标记。

---

## 3. 全局门禁与回归验证记录

| 验证门禁命令 | 测试项数 / 范围 | 退出码 | 结果 |
|---|---|---|---|
| `make PROFILE=x86_64-clang test-boot-failure-host CASE=all` | 7 个宿主测试二进制，326 个断言 | 0 | **PASS** |
| `make PROFILE=x86_64-clang test-host` | 完整宿主测试套件 (PMM, Slab, VFS, Poll, GIC, APIC 等) | 0 | **PASS** |
| `make PROFILE=x86_64-clang test-static` | 15 项静态审计 (Layout, Frame, Canary, Link Order, Syscall Boundary 等) | 0 | **PASS** |
| `make PROFILE=x86_64-clang test-harness` | 43 个 Python 测试框架模块 (含 `qemutests.test_boot_failure`) | 0 | **PASS** |
| `make PROFILE=x86_64-clang test-qemu SUITE=boot-failure` | 17 组 QEMU 启动矩阵 + 镜像哈希不变守卫 | 0 | **PASS** |
| `make PROFILE=x86_64-clang OS01_SYSTEST=1 KERNEL_SELFTEST= test-syscall` | 99 项系统调用 E2E 套件 (228 个测试点) | 0 | **PASS** |
| `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` | 8 核内核内自测 (boot + scheduled markers) | 0 | **PASS** |
| `make PROFILE=aarch64-clang aarch64-uefi` | AArch64 UEFI 内核构建与磁盘映像生成 | 0 | **PASS** |
| `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | AArch64 4 核 SMP 启动与调度验收 | 0 | **PASS** |

---

## 4. 提交序列与修改清单

1. `54409447 fix(boot): enforce subsystem failure barriers and registration integrity` (Task 1)
   - `kernel/core/boot_failure.c`, `kernel/include/core/boot_failure.h`
   - `kernel/subsys/subsys.c`, `kernel/arch/x86_64/subsys.c`, `kernel/arch/aarch64/subsys.c`
   - 宿主测试: `hosttests/cases/test_boot_failure_*.c` (subsys, percpu, log, query)
2. `bd0a930e fix(boot): require root and terminal capabilities before userspace` (Task 2)
   - `kernel/fs/boot.c` (强制 root 检查, 可选 proc/tmpfs/shm 降级)
   - `kernel/tty/boot.c`, `kernel/tty/pty.c` (强制 tty/pty 终端能力检查)
   - 宿主测试: `hosttests/cases/test_boot_failure_mounts.c`, `test_boot_failure_terminal.c`
3. `06a808d3 fix(sched): fail initial userspace startup on stdio inheritance errors` (Task 3)
   - `kernel/sched/boot_stdio.c`, `kernel/include/sched/boot_stdio.h`
   - `kernel/arch/x86_64/cpu/sched_boot.c` (`task_init` 初始 stdio 表与继承检查)
   - 宿主测试: `hosttests/cases/test_boot_task_init.c`
4. `4da8a735 test(boot): exercise required and optional startup failure policies` (Task 4)
   - `qemutests/boot_failure.py`, `qemutests/test_boot_failure.py`
   - `kernel/selftest/boot_failure_fixture.c`, `kernel/include/selftest/boot_failure_fixture.h`
   - 构建系统集成: `mk/components/run.mk`, `mk/project.mk`, `mk/tests/catalog.mk`, `config/test-scenarios.json`
5. `(current) docs(boot): record BOOT-3 failure contracts and validation` (Task 5)
   - 验收报告与多架构文档收尾。

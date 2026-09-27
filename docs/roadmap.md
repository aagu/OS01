# OS01 优化路线图

> **基准**: `6b84fb1`（本次修订前的 master HEAD，2026-09-26）
> **日期**: 2026-09-26

roadmap 只列**未完成 / 进行中**的规划项；所有已完成工作见 `docs/changelog.md`（最新 2026-09-26；本版本相较前一版新增 09-19 ~ 09-26 这一周 ~50 个 commit：AAGU-1/2/3/4 全套 + 5.6/5.7/5.8 + 6/7/7.1/8/29 + arch source groups + compiler_rt 目录裁撤 + build harness consolidation + Generic Timer Phase 1/2 全栈 + aarch64 IPI fix + AAGU-4 残留清理（compat/ 裁撤 + softirq/tick/clocksource/driver-net ifdef 清扫））。

## 当前状态（一句话）

- **Phase 1-9**：全部就绪（COW/mmap、调度/信号/SMP、文件系统、设备驱动、用户态、poll/select、网络、时间系统）
- **工程治理**：AAGU-1/2/3/4/5.6/5.7/5.8/6/7/7.1/8/29 + arch source groups + compiler_rt 目录裁撤 + build system harness consolidation 全部就绪
- **aarch64 适配**：Generic Timer 全栈 + GICv2 Phase 1 + IPI fix 已闭环；统一 kernel_main / arch dispatch 统一仍在路上

详细背景、commit 记录、经验教训见 `docs/changelog.md` + 各专题 closure 文档（`docs/aarch64-*-closure-*.md` / `docs/aarch64-ipi-fail-handoff-2026-09-26.md`）+ 主题 docs。

---

## 待实施路线图（按 5 优先级）

> **P0 工程基础** ✅ → **P1 安全加固** → **P2 aarch64 适配** → **P3 GUI** → **P4 硬件适配** → **P5 ABI 扩展/兼容性**

### 🔒 P1 安全加固

前置链（已完成，详见 `docs/changelog.md`）：`getrandom` → 统一用户态启动方式 → 用户栈 canary + `AT_RANDOM` → AAGU-5 arch entropy facade（STRONG-only 控制流）。ASLR 的随机种子须由内核 STRONG-only 接口提供；`AT_RANDOM` 已就绪，但不是 mmap 选址函数的直接输入。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| ASLR-A：mmap 基址 | 先限 x86_64 用户态：每次新地址空间建立时随机化 mmap 搜索基址；fork 继承现有布局；保留 MAP_FIXED 语义，并验证地址窗口、冲突与溢出 | STRONG-only `kernel_random_get_strong()` ✅；需先定熵不可用时的失败语义与用户 VA 窗口 | |
| ASLR-B：ET_DYN/PIE | 单独实施可重定位 ELF 的加载偏移、静态 PIE 重定位及用户程序构建迁移（含 BusyBox） | ASLR-A 验收；当前 ELF loader 仅支持 ET_EXEC、用户构建使用 `-fno-pie -no-pie`；需设计重定位、linker script、启动 ABI/auxv 与回退测试 | |
| UBSan + KASan | 内核编译期 instrument | 独立 | ArvernOS |
| 堆加固 | malloc double-free/溢出检测 | 独立 | |
| NX 页 | 栈/堆不可执行 + mmap `PROT_EXEC` 审计 | 独立 | |

ASLR 分期实施，不把 A/B 合成一个小任务。当前用户栈固定在 `USER_STACK_BASE=0x800000`；栈随机化另列后续范围，完成 A/B 后也不能称为完整用户态 ASLR。aarch64 phase 1 尚无用户态，本项先不扩大到 aarch64。

### 🏗 P2 aarch64 适配

已完成：v25 arch-cleanup / PMM arch-neutral / 页表原语 / Generic Timer Phase 1 + Phase 2 #1~#5 / GICv2 Phase 1 + GIC probe fix / AAGU-3 subsys_stub convergence / AAGU-29 libk.a link / IPI TPIDR_EL1 fix（详见 `docs/changelog.md` + `docs/aarch64-*-closure-2026-09-18.md` + `docs/aarch64-libk-aarch64-closure-2026-09-24.md` + `docs/aarch64-ipi-fail-handoff-2026-09-26.md`）。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| head.S + MMU 剩余 | user 页表 / uaccess facade（`mmu.h::arch_user_range_accessible` aarch64 仍 fail-closed stub）；boot 2MiB block 与 4K 原语合并（`boot_fixup.c` 自述 future work）；page_table 原语 host 测试为零 | 页表原语 ✅ | ArvernOS |
| 统一 kernel_main | 单 `kernel_main` 按固定顺序调 `arch_early_init()` / `arch_late_init()` / `scheduler_init()`；aarch64 必须先 dtb 才能用 DTB 信息；init 顺序契约需明确。**Spec A 中断 dispatch / Spec B SMP+timer / Spec C kernel_main 单一入口** | PMM ✅, SMP ✅, 调度器 core, arch_irq ✅, rtc ✅, pt_regs_t ✅, head.S ✅, GIC ✅, Timer ✅, UEFI 链 ✅ | 长项 spec |
| 中断/异常 dispatch 统一 | `arch_intr_dispatch(vector, pt_regs*)` 单入口抽象；x86_64 IDT vs aarch64 VBAR_EL1 vector tables 各自封装；x86_64 IST stack vs aarch64 SP_EL1 切换。**最大代码量减少**：`x86_64/trap.c` 3078 行大部分是 x86 register decode；aarch64 `intr/trap.c` 31 行（IRQ 已接 GIC dispatch，sync 异常仍 `b .` 占位）。`arch_irq_dispatch` 已落地，剩余是 trap.c 内部 x86 register decode 的 arch 剥离 | arch_irq ✅, head.S ✅, GIC ✅ | Linux do_IRQ |
| SMP 启动统一 | `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)` 单入口；内部 aarch64 PSCI CPU_ON / x86_64 INIT-SIPI + trampoline 各自实现 | GIC ✅, 启动链 ✅ | opuntiaOS |
| 上下文切换统一 | `arch_task_switch(prev, next)` + `arch_thread_entry()`；aarch64 ret 到 user vs x86_64 sysret/iret | SMP 启动统一 | Tilck |
| CPU 特性探测 | `arch_cpu_features()` 返回统一位图（has_fpu / has_virt / has_cache_coherency）；x86 CPUID vs aarch64 ID_AA64* 各实现一份 | 独立 | Linux cpufeature |
| GICv2 Phase 2 | SError/真机覆盖率；`aarch64_main` 接 GIC init；handler 表扩容（SGIs 0-15 + PPI 16-31 全注册）；dtb_gicd_base 解耦 | GICv2 Phase 1 ✅ | |

**距离单一 kernel_main 还差多远（粗估，一个人全职）**：~4–8 周。详见 `docs/arch.md` 末段 3 个 spec/plan 增量推进。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。靠 arch 抽象层封装，统一接口、不统一实现。

### 🖥 P3 GUI

基座（已完成）：fb、fb mmap、terminal 双缓冲 + alt-screen、键盘扫描码、PS/2 鼠标驱动（i8042 共享控制器层 + `/dev/mouse` 8 字节事件 ABI + 500 ms 有界探测，2026-09-27 落地，见 `docs/driver.md`）。Tetris 游戏已落地（见 `docs/gui.md`）。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 2D 图形 API | fb 之上画线/矩形/位图 blit | 独立 | |
| 可缩放字体渲染器 | 矢量/位图缩放 | 2D API | HackOS |
| Window Server + compositor | 多窗口管理 + 合成 | 字体/2D/鼠标 | opuntiaOS + HackOS |

### 🔧 P4 硬件适配

无基座，全部 pending。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| USB 驱动栈 | HID/存储/网络 | 独立 | |
| 真机启动 (USB) | 建立硬件验证路径 | USB 存储 | Tilck |
| NVMe 驱动 | 替代 AHCI | 独立 | |
| HPET clocksource | 真实硬件跨平台时间源 | 独立 | |
| ACPI | 电源管理/关机 | 独立 | |

### 📐 P5 ABI 扩展/兼容性

已完成（详见 `docs/changelog.md`）：symlink/readlink、exec symlink ABI、syscall 边界审计、AAGU-4 跨边界符号全套、AAGU-5 entropy facade 全套、arch source groups、AAGU-29 libk.a link、arch entropy strong overrides（RDSEED/RNDRRS = STRONG）。

依赖链：`ELF loader ✅ → 动态链接器 → 共享 libc → Alpine apk/musl`；`futex ✅ → clone → pthread`；`socket ✅ → AF_UNIX`；`mbedTLS ✅ → HTTPS`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 动态链接器 | PT_INTERP + ld.so + 共享 libc | ELF ✅ | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构 | |
| clone/pthread | 线程模型 + pthread_create | futex ✅ | |
| readv/writev | scatter-gather I/O | 独立 | |
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立 | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立 | |
| 作业控制（部分完成）| setpgid/setsid/getpgid/getsid ✅、tcgetpgrp/tcsetpgrp ✅、tty ISIG + VINTR/VQUIT ✅、kill pid=0/-pid/-1 ✅；**剩余**：SIGWINCH 派发、SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs） | tty termios ✅ | |
| /proc 完善 | status（signal mask/ppid/utime/stime）+ cmdline + stat | 独立 | |
| sysroot 增量重编译 | genid 变化即 `-B` 全量重编；refinement：generation 内 .d 路径相对化/软链引用 | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget | mbedTLS ✅ | |
| AF_UNIX/socketpair | 本地 socket IPC | socket ✅ | |
| 更多 applet | grep/sed/find，先补 libc regex/fnmatch | libc | |
| libc 完整性 | printf `%f/%F/%e/%E/%g/%G` + `%ld/%lu` + `%x/%o`、strtod、getopt ✅；**仍缺**：stdio 行读取/seek、getcwd、cut/paste 的 getopt 解析 | 独立 | |
| Alpine apk/musl | musl 二进制包兼容路线 | 动态链接器 | cavOS |

### Parked（未闭环 follow-on，随时可拾起；2026-09-26 逐项对照代码核实）

| 项 | 状态 | 说明 |
|----|------|------|
| sysroot 头文件级增量重编 | **已完成**（2026-09-26） | genid 变化即 `-B` 全量重编已移除。机制：① kernel 编译/链接（`-isystem`、`-include stdint.h`、`-L`、`KERNEL_RAW_LIBDIR`）全部走稳定 `$(SYSROOT)` symlink 而非 resolved generation 路径 → flags/.d 跨 republish 稳定（`.cflags` stamp 不再被 genid 变化打爆）；② publisher 复制改 `cp -p` 保留 mtime（staging 层早已 `--preserve=timestamps`），未变头文件在新 generation 中 mtime 不变；③ publish 增加 lease 等待（60s 预算），防 republish 在编译中途换 header；④ `kernel.mk` 删除 `.sysroot-generation` stamp 与 genid→`-B` 逻辑（保留 runtime receipt `-B`）。**顺带发现并修复两个预先存在的缺陷**：`-MMD` 语义是跳过 system header，kernel 的 `.d` 从未记录过任何 `-isystem` 引入的 libc 头（仅 `-include` 强制的 stdint.h）→ 改 `-MD`；runtime grouped rule 的 receipt 无条件重写 + archive 永不更新 → 每 kernel 构建都 relink（stage1+kallsyms）→ receipt 写入改内容门控（cmp）。`.cflags` fingerprint 加 `deps=system-headers` 版本前缀使现存 build 目录一次性自动重建。回归：`test-contract` 新 mode `sysroot-headers`（单头编辑只重编依赖者而非全量 / stamp 不动 / 还原后一致 / 零源码污染） |
| `kernel/arch/aarch64/subsys_stub.c` / `idle_resume_stub.c` 替换 | **已完成**（2026-09-26） | `subsys_stub.c` 已由 AAGU-3 闭环删除（真实 `kernel/subsys/subsys.c` 已编入 aarch64，`serial_printk`/`strcmp`/`num_cpus` 均有真实实现）。`idle_resume_stub.c` 正名为 `kernel/arch/aarch64/cpu/idle.c`：`wfi` 永循环对无调度器的 aarch64 就是正确的 idle 行为（各 CPU 睡眠等待 CNTP tick / GIC 中断），非占位符；need_resched 感知的 per-CPU idle 再入随 §P2 上下文切换统一落地 |
| 同一变体的编译参数缓存失效（CFLAGS-only） | **已完成**（2026-09-26，先验证后修复按建议执行） | 验证：`KERNEL_SELFTEST=1` 确已使用独立 `KERNEL_VARIANT=selftest` 目录（169 个 .o 全落 `kernel/selftest/`，不再是复现例）；但同一变体内注入 `-D` 探针实测 0 个 `.o` 重编，`DEBUG=1`/`LOG_TARGET` 等确被静默复用。修复：`kernel/Makefile` 新增 `$(BUILD_DIR)/.cflags` fingerprint stamp——3 条编译 pattern rule 的前置依赖，resolved `ALL_CFLAGS`+`ALL_C_ONLY_FLAGS` 变化才更新 → 全量重编（与 sysroot-generation `-B` 同粒度；`make -n` 安全）；DEBUG 通道 / LOG_TARGET / genid -isystem 路径均被覆盖。回归：`test-contract` 新 mode `flags-cache`（identical 重建 0 重编 / flag 变化 stamp 移动+重编 / flag 还原重编 / 工作区零污染）+ `KERNEL_EXTRA_CFLAGS` 透传旋钮作测试钩子。kernel/Makefile x86_64+aarch64 共用 → 两 arch 同时闭合 |
| `kernel/include/compat/` 整个目录裁撤 | **已完成**（AAGU-4 残留清理） | AAGU-4.3.6 commit `bfb314d` 已 `git rm` 整个 `kernel/include/compat/` 目录；Makefile:129 注释确认「AAGU-4.3 closes the kernel/include/compat/* mirror (6 sub-issues landed). The compat/ path is no longer needed.」AAGU-29 libk.a link 落地后进一步消除依赖 |
| x86-only 驱动目录重定位 | **已完成**（AAGU-4 §3.3 🟡） | arch source groups commit `c3412da` 已搬 `pic/8259A.c`、`apic/lapic*.c` 至 `kernel/arch/x86_64/intr/`，原 ifdef 已移除；AAGU-4 残留清理 Task 4 再清掉 `kernel/driver/*.c` + `kernel/net/net.c` 的 5 处冗余 `#ifdef __x86_64__` SUBSYS_INITCALL 守护 |
| `kernel/intr/softirq.c` `#ifdef __x86_64__` 残留 | **已完成**（AAGU-4 §3.3 ❌） | AAGU-4 残留清理 Task 1：原子操作实现从 `kernel/arch/<arch>/cpu/atomic.c` 外部函数迁移到 `kernel/include/arch/<arch>/atomic_bitops.h` `static inline + always_inline`；softirq.c 直接调 `arch_atomic_or_u64()` / `arch_atomic_and_u64()`，无 ifdef |
| `LWIP_RAND`/AT_RANDOM 种子 | **已完成**（2026-09-17，worktree `feat/user-stack-canary`；2026-09-22 AAGU-5 重构无回归） | `kernel/net/lwip_sys_arch.c` `LWIP_RAND()` 已替换为 `lwip_getrandom_u32()`（直接走 `get_random_bytes()` ChaCha20）；auxv 构造 `setup_user_stack()` 单站点已压 `AT_RANDOM(16B CSPRNG)` + `AT_PLATFORM("x86_64")`（commits `e35c763` + `ae472a6`）—— 留作历史记录 |

> 旧 Parked 项（devfs mount entry / PMM 非-RAM 类型 / `__vfs_lookup_raw` consumed 路径 / `arch_kernel_thread_entry` panic-on-call / aarch64 -I libc/include path 残留）已分别在 `0809100` / `beb351c` / `b0c95e3` / AAGU-6 + v25 / AAGU-29 + Phase 2 #5 闭环，从 Parked 表移除。

---

## 相关文档

| 文档 | 内容 |
|------|------|
| `docs/README.md` | 文档索引（按 0-4 层组织，按阅读顺序排列） |
| `docs/architecture.md` | 启动链、内存布局、初始化序列（全局骨架图） |
| `docs/boot.md` | UEFI 引导 + `boot_context` v2 ABI + v25 E820 拆分 |
| `docs/arch.md` | arch-neutral facade + per-arch 强覆盖模式（weak default / strong override）+ v25 arch-cleanup 系列 7 子系统矩阵 |
| `docs/arch/cross-boundary-symbols.md` | **跨边界符号/ABI 边界规范（AAGU-4）** —— builtin、UAPI、arch-value、libc 镜像 4 类规则 + 现状对照表 + 后续 issue 切分 |
| `docs/arch/entropy-source-facade.md` | **arch-neutral entropy facade spec（AAGU-5.3 / .5）** —— STRONG/WEAK/NONE 三档质量标签 + 控制流决策 + 禁止条款 |
| `docs/memory.md` + `docs/cow-mmap.md` | 物理/虚拟内存管理 + COW fork/mmap + v25 PMM arch-neutral + 页表层级统一 |
| `docs/interrupt.md` | 中断处理（do_IRQ、register_irq、IDT） |
| `docs/smp.md` | SMP 架构 + 负载均衡实施总结 |
| `docs/scheduler.md` + `docs/scheduler-complexity.md` | EEVDF 调度器设计与复杂度评估 |
| `docs/syscall.md` | 71+ syscall 表 + 用户指针边界语义 + syscall 边界审计触达清单 |
| `docs/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结 |
| `docs/gui.md` | Tetris 游戏实施总结 + P3 GUI 路线图 |
| `docs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/network.md` + `docs/lwip-debugging-experience.md` | lwIP 网络栈 + 正确性加固 |
| `docs/filesystem.md` | VFS, FAT32, ext2, devfs, procfs, tmpfs, GPT, block device |
| `docs/driver.md` | 驱动子系统（keyboard, serial, ahci, pci, e1000, virtio-net, fb） |
| `docs/subsys.md` | 子系统注册框架（`SUBSYS_INITCALL()` + phase 顺序） |
| `docs/log.md` | 日志级别、DEBUG_CHANNELS、LOG_TARGET、NDEBUG |
| `docs/build.md` | GNU Make profile 化构建体系 + v25 UEFI 残留排除 |
| `docs/build-run-debug.md` | 端到端构建运行调试 |
| `docs/build-system-harness.md` | **构建/测试 harness 权威 target taxonomy** —— 6 个 bucket test 目标 + capability gate + alias policy（build system harness consolidation） |
| `docs/debug.md` | 调试通道 |
| `docs/applet-verification.md` | busybox applet 验证清单 |
| `docs/decisions.md` | 关键设计决策总账 |
| `docs/structure.md` | 目录结构逐目录说明 |
| `docs/references.md` | 开源 OS 项目借鉴表 |
| `docs/changelog.md` | **历史完成记录**（按时间倒序，最新 2026-09-26） |
| `docs/superpowers/specs/` + `docs/superpowers/plans/` | 历史设计 spec + 实施 plan（过程档案） |
| `docs/aarch64-ipi-fail-handoff-2026-09-26.md` | aarch64 IPI cpus≥2 FAIL 根因（TPIDR_EL1 误读）+ 修复记录（`d695020`） |
| `docs/aarch64-libk-aarch64-closure-2026-09-24.md` | AAGU-29 闭项报告：aarch64 kernel link libk.a |
| `docs/aarch64-timer-phase1-closure-2026-09-18.md` | aarch64 Generic Timer Phase 1 闭项 |
| `docs/aarch64-timer-phase2-closure-2026-09-18.md` | aarch64 SUBSYS_INITCALL Phase 2 闭项 |
| `docs/aarch64-timer-phase2-cntp-closure-2026-09-18.md` | aarch64 cntp_tick_handler 集成 Phase 2 #2 闭项 |
| `docs/aarch64-timer-phase2-smp-closure-2026-09-18.md` | aarch64 per-CPU timer / SMP timer Phase 2 #3 闭项 |
| `docs/aarch64-udivti3-hoist-closure-2026-09-18.md` | `__udivti3` hoist Phase 2 #4 闭项 |
| `docs/aarch64-libc-include-policy-closure-2026-09-18.md` | `-I libc/include` cleanup Phase 2 #5 闭项（5/5 P2 follow-ups 闭环） |

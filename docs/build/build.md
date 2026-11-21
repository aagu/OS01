# 编译、运行与调试指南

> 本文档合并自 `docs/build.md`、`docs/build-run-debug.md`、`docs/build-system-harness.md`（2026-10-01）。
> `docs/build/toolchain.md` 保留为独立文档，覆盖 x86_64 toolchain override 契约。

本系统使用原生 Clang 交叉编译（`x86_64-unknown-none` 目标），无需单独的 GCC 交叉工具链。可重入（re-entrant）中断处理通过在 CFLAGS 中禁用 red zone 来保证。

构建体系自 2026-09-02 起按 **profile** 组织（GNU Make 重构，见 [`docs/superpowers/specs/2026-09-02-build-system-design.md`](../superpowers/specs/2026-09-02-build-system-design.md)）：每个 profile 完整定义目标架构、能力集、编译器、链接器、sysroot、UEFI 固件与 QEMU 参数。所有中间产物与最终产物都位于 `build/<profile>/` 下，不同 profile 互不污染。

> **x86_64 工具链覆盖变量**（`CLANG=clang-N`、`LLVM_NM=`、`UEFI_CLANG=` 等）、允许的开关白名单和受控环境规则：参见 [`toolchain.md`](toolchain.md)。

## 第 1 章 · 编译过程

### Profile 与用户入口

调用格式为 `make PROFILE=<name> <target>`；未指定时 `PROFILE=x86_64-clang`。

| Profile | 能力 | 产物 |
| --- | --- | --- |
| `x86_64-clang`（默认） | `kernel userland rootfs uefi` | `kernel.bin`、用户 ELF + BusyBox、`image/disk.img`、BOOTX64.EFI |
| `aarch64-clang` | `kernel uefi userland` | `kernel.elf`、BOOTAA64.EFI、用户 ELF + BusyBox（2026-11-21 起；无 mbedTLS/gfx 应用）、192 MiB GPT 双分区 `image/aarch64-uefi.img`（2026-11-21 起与 x86 同布局：ESP FAT32 = BOOTAA64.EFI + kernel.elf → /boot，ext2 rootfs → / 含全部解锁用户程序（x86 `ROOTFS_USER_PROGRAMS` 同款过滤器：除 sh/压测/probe 外的 USER_PROGRAMS）、/bin/busybox、/etc/inittab；/bin/sh 为 busybox 软链） |

**能力契约**：每个入口 target 都是能力感知的。在缺少对应能力的 profile 上执行会立即失败（解析期报错，不会等到编译）：

```text
make PROFILE=aarch64-clang run
make: *** PROFILE='aarch64-clang' lacks capability 'rootfs'.  Stop.
```

- `run` / `run-kvm` / `run-virtio` / `run-usb` / `debug` / `test-*`（x86 E2E）需要 `rootfs`。
- `aarch64-uefi` / `aarch64-uefi-kernel` / `run-aarch64-uefi` 需要 `uefi`。
- `lib` / `user` 需要 `userland`。
- `validate-profile` 不要求 `rootfs`；`make test` 要求支持完整宿主回归的 `rootfs` 能力，但不构建客体镜像。

**用户入口**（默认 profile 即用，其它 profile 以 `PROFILE=<name>` 前缀使用）：

```text
make                  默认 profile 的磁盘镜像（= build/<profile>/image/disk.img 的根目录兼容副本）
make kernel.bin       默认 profile 的内核（项目根兼容副本）
make disk.img         默认 profile 的磁盘镜像（项目根兼容副本）
make image            当前 profile 的磁盘镜像（镜像路径，见下文 variant）
make lib              默认 profile 的 sysroot 库（stamp）
make user             默认 profile 的用户 ELF 与 BusyBox
make run              默认 profile 启动 QEMU（-display gtk；无显示环境用 run_test.py 的方式串口验证）
make run-aarch64-uefi aarch64-clang profile 启动 QEMU（-display none -serial stdio；默认 SMP=1 即单核产品用户态运行——aarch64 user-mm 为单核 v1，显式 SMP=N 仍生效但多核启动按设计仅内核态）
make print-run-paths  默认 profile 打印 firmware=/image= 绝对路径（手动 QEMU 用，见下文）
make validate         内核 ELF / EFI 产物验证 + profile 信息打印
make clean            清理指定 profile（默认 profile 还删除项目根兼容文件）
```

**Test entry points** — seven buckets (3 with flags, 4 fixed):

| Bucket | Purpose | Default |
| --- | --- | --- |
| `make test-qemu SUITE=phase-0\|systest\|inittab-phase\|network\|gfx\|resolution\|driver-model\|monitor\|terminal-present\|gui\|intr-x86\|boot-lifecycle\|boot-irq-time\|boot-failure\|boot-contract\|boot-services\|usb-boot\|arch10-capacity\|arch6-fsstress\|doom` | QEMU E2E against the matching variant image (or the normal image for `gfx`/`resolution`/`driver-model`/`monitor`/`terminal-present`/`gui`/`usb-boot`; `intr-x86`/`boot-lifecycle`/`boot-irq-time`/`boot-failure`/`boot-contract`/`boot-services`/`doom` build their own isolated fault/test variants). | `phase-0` |
| `make test-host` | hosttests + PMM boot reservation | — |
| `make test-static` | runtime / layout / canary / link-order audits (16 audits) | — |
| `make test-aarch64 MODE=smp\|no-ack\|gic-spi\|sync-fault\|serror\|exception-trace\|m3-probe\|sched\|sched-ap\|sched-wake\|sched-tick\|boot-contract\|m1-*\|lma-layout` | AArch64 PSCI / GIC / sync-fault / serror / exception-trace / scheduler / boot-contract acceptance harness | `smp` |
| `make test-contract PROFILE=x86_64-clang\|aarch64-clang` | CI build-contract check | `x86_64-clang` |
| `make test-kernel-selftest` | Boot selftest image + parse `[selftest]` markers | — |
| `make test-harness` | Python unittest framework regression (no QEMU, never discovers QEMU scripts) | — |
| `make test-doctor` | Compare local `clang`/`ld.lld`/`qemu` versions against `ci/Dockerfile` pins (reports `OK`/`DRIFT`/`SKIPPED(no ci pin)`, exits 2 only if a tool cannot execute) | — |
| `make ci-local SUITE=<name>` | Run `test-qemu SUITE=<name>` (or `MAKE_ARGS=...`) inside `ghcr.io/aagu/os01-ci:latest` via `docker`/`podman` (no auto-pull) | — |

`make test-qemu-usb-boot` 是 USB boot M0 的专用固件门禁（`USB_BOOT_CASE=firmware`，断言 `fs/root rc=-19`）。它使用共享的 `qemutests/harness/qemu_cmd.py` 的 `storage="usb"` 拓扑，不是 `test-qemu SUITE=` 的目录套件。**该用例已过期**：MSC 驱动落地后 USB-only 引导不再停在 `rc=-19`，而是走通到 shell，因此该用例现在超时失败——详见本节「现状」段与 `docs/roadmap.md` P4 的对应条目。

AArch64 EL1 调度器验收（`MODE=sched`）：`make PROFILE=aarch64-clang test-aarch64 MODE=sched [SCHED_CASE=<v>]`。该 MODE 通过受控子构建产出隔离的 `KERNEL_SELFTEST` 调度器镜像变体：`SCHED_CASE=positive`（默认组成部分）构建 `kernel/sched` + `image/sched`（正例：严格 14 用例协议 + 全部冻结 `[sched-evidence]` 字段证据矩阵），四个故障值 `sp-top`/`sp-low`/`pc-low`/`pc-data` 各构建 `kernel/sched-fault-<v>` + `image/sched-fault-<v>`（编译期选中的 `aarch64_sched_fault_inject` 注入钩子损坏保存 SP/PC，验收要求 fatal 发生在**切栈前**且受害者零提交切换）。`SCHED_CASE` 省略时为 `all`（正例矩阵 + 全部四个负例）。选择器 `AARCH64_SCHED_TEST` 与 `SCHED_CASE` 在解析期拒绝非法值；`MODE=sched`（即使未带选择器）与 `AARCH64_SYNC_FAULT_TEST`/`AARCH64_M1_TEST`/`KERNEL_TEST_FORCE_NO_RNDRRS`/`KERNEL_CANARY_SELFTEST`/`OS01_SYSTEST`/非零 `AARCH64_SMP_TEST_NO_ACK_CPU` 的任意组合均解析期报错。runner（`qemutests/aarch64_scheduler.py`）只消费 make 传入的显式镜像/固件/ELF 路径，绝不隐式重建；每次 QEMU 有 120 s 绝对期限（tick 不延长），完成后 1 s 排空窗口，崩溃/超时永远 FAIL；`llvm-nm` 符号审计要求普通 normal/selftest/positive ELF 不含注入符号而每个 fault ELF 含有。普通镜像 BSP idle 冒烟仍是同一 runner 的 `--idle-smoke` 模式（`--cpus 1 2 4 8`）。

AArch64 AP 协作式调度验收 P1（`MODE=sched-ap`）：`make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap [AP_SCHED_CASE=<c>]`。`AP_SCHED_CASE=positive`（默认）构建 `kernel/sched-ap` + `image/sched-ap`（编译选择器 `AARCH64_AP_SCHED_TEST=ap`），在 accepted AP 全部进入协作调度后运行七个冻结生产 case（ready/entry/yield/reap/work/admission/default-mask），每个录用 CPU 一行、BSP 聚合行 cpu=0，包络 `[ap-sched] START phase=ap cpus=N accepted_mask=M`（要求 `M=(1<<N)-1`，缺失 AP 不得降级）与 `[ap-sched] END phase=ap failed=0`；runner（`qemutests/aarch64_ap_scheduler.py`）按冻结字段与数值严格解析，1/2/4/8 核各 2 次 boot。`AP_SCHED_CASE=fatal-owner` 构建 `kernel/sched-ap-fatal-owner` + `image/sched-ap-fatal-owner`（`AARCH64_AP_SCHED_TEST=fatal-owner`），独立 2 核 boot 经真实 `ap_work` 项让双 CPU 同时进入非法 SP 路径，无栈 ldaxr/stxr 仲裁单 owner 后输出恰一行 `[ap-fatal] owner=<0|1> arrivals_mask=3 entrants=1 losers=1`（缺失/重复/值错/成功 END 均失败）；`llvm-nm` 审计要求 fatal 变体含 `aarch64_ap_fatal_owner_enter` 而普通 `sched-ap` ELF 不含任何注入符号。`MODE=sched-ap` 与旧 sched/M1/sync-fault/no-ack/canary/OS01_SYSTEST 的组合在外层 make 解析期拒绝。源码级合同与 runner 负向合同见 `qemutests/test_aarch64_ap_boot_contract.py` 与 `qemutests/test_aarch64_ap_scheduler.py`（注册于 `make test-harness`）。

AArch64 每核调度 tick 记账验收（`MODE=sched-tick`）：`make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick` 构建隔离的 `kernel/sched-tick` + `image/sched-tick`（编译选择器 `AARCH64_SCHED_TICK_TEST=1`），1/2/4/8 核各两次 boot。镜像内产品 selftest 在每个已录用 CPU 上发布七个冻结 case 的证据行（tick-ready/tick-idle/tick-busy/tick-yield/tick-deadline/tick-global/tick-remote），整批证据先于对应的 `selftest_record_late()` 记录，`[sched-tick] END failed=0` 排在 selftest END 之后。runner（`qemutests/aarch64_sched_tick.py`）只消费 make 传入的显式镜像/固件/ELF 路径，按冻结字段与跨字段关系严格解析，缺字段/重复CPU/越界/AP global tick/缺END/未知ID一律拒绝，`accepted_mask` 必须等于 `(1<<N)-1`（不得用在线数冒充录用数），产物按内容审计（AArch64 ELF64 + 三个专用符号 + 同 variant 目录 + 专用符号不泄漏进普通产品 ELF），每次 QEMU 120 s 绝对期限（tick 不延长）。采样语义：**一计真实递送的 CNTP 中断 = 一个样本**，100 Hz 是名义值，不做墙钟补算。`MODE=sched-tick` 自己设置 `KERNEL_SELFTEST`，与旧 sched/AP sched/M1/no-ack/sync-fault/canary/weak/systest 的任意组合在外层 make 解析期拒绝；x86_64 profile 也直接拒绝该 aarch64 选择器。源码级合同与 runner 负向合同见 `qemutests/test_aarch64_sched_tick.py`（注册于 `make test-harness`）。

AArch64 启动块平台配置验收（`MODE=lma-layout`，2026-11-18）：`make PROFILE=aarch64-clang test-aarch64 MODE=lma-layout` 由专用 runner（`qemutests/aarch64_lma_layout.py`）在**独立私有构建根**（`AARCH64_BUILD_ROOT`，固定 `KERNEL_SELFTEST=1`）按 默认 → 改址（`0x42000000`）→ 默认 顺序构建并配对启动：每轮用 `mcopy -i` 抽出 FAT 内的 `kernel.elf`/`BOOTAA64.EFI` 与同轮发布产物逐字节比较并断言真实入口，改址与恢复默认两份镜像各通过直接 runner（`aarch64_uefi_smp.py --expect-lma-base <base>`，复用 `--diagnostic-dtb=auto` 诊断 DTB 机制）启动一次，内核侧以实测 `UEFI-A64: layout entry=<hex> context=<hex>`、boot-map selftest、完整 M1/PRUNE/AP 证据和「M1 ZONE 不含启动块」为通过条件；恢复默认的 FAT 副本换入改址内核后必须出现专用 `UEFI-A64: kernel entry mismatch` 与 `UEFI: kernel load failed` 且无内核接管证据（`--expect-lma-mismatch`，短观测窗口 + 超时分类）。全程比对默认构建树前后摘要（不得新建路径），证据归档 `test-results/aarch64-lma-layout/`。与其它编译型变体的组合在外层 make 解析期拒绝。证据 fixture 见 `qemutests/test_aarch64_harness.py::LmaLayoutEvidenceTests`（注册于 `make test-harness`），场景 `boot.lma.uefi`。

AArch64 启动块构建变量（2026-11-18）：`AARCH64_BOOT_BLOCK_BASE=<十进制或 0x 十六进制>` 选择本次构建的物理启动块 B（默认 `0x40000000`，QEMU virt）。profile 在解析期校验（单一整数、2 MiB 对齐、位于 M0 [0,0x80000000) 内、不与设备窗口 D=[0x08000000,0x0a000000) 相交、B 端之后仍有 ≥20 MiB arena 窗口；显式空值报错）。同一 `-D` 进入内核 C/汇编与 cpp 预处理的生成链接脚本 `$(KERNEL_BUILD_DIR)/linker.generated.ld`（stage1/final 链接与越界断言共用），`kernel/include/arch/aarch64/boot_layout.h` 是全部派生常量（入口/handoff/trampoline/D/M0 末端/页偏移）的单一来源，UEFI loader、M0 写槽、RAM 保留、arena 与 M1 建表全部消费它。`AARCH64_BUILD_ROOT=<绝对路径>` 把整个 profile 输出根迁到私有树（默认等于 profile `BUILD_DIR`），改址构建与默认树完全隔离；布局键变化会失效受影响的应用对象与 EFI（A→B→A 不漏重编）。**验收范围仅改址构建与 QEMU（默认与 `0x42000000`、B=0 仅静态布局验证），真机启动未验证**——允许低于 1 GiB 的配置不代表相应真机已可启动。

AArch64 M4.1–M4.14 使用同一个 kernel-selftest 镜像。单核启动：

```bash
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi
```

串口看到 `[TEST] PASS aarch64_el0` 、`[TEST] PASS aarch64_el0_irq`、`[TEST] PASS aarch64_el0_switch`、`[TEST] PASS aarch64_el0_preempt`、`[TEST] PASS aarch64_el0_task`、`[TEST] PASS aarch64_user_mm`、`[TEST] PASS aarch64_user_task_lifecycle`、`[TEST] PASS aarch64_user_svc_uaccess`、`[TEST] PASS aarch64_user_vm`、`[TEST] PASS aarch64_user_fork_cow`、`[TEST] PASS aarch64_user_waitpid`、`[TEST] PASS aarch64_user_exec`、`[TEST] PASS aarch64_user_signal`、`[TEST] PASS aarch64_vfs`（2026-11-18 VFS 门新增）和 `[TEST] END suite=kernel-selftest total=28 passed=28 failed=0 skipped=0` 后即可退出 QEMU（**2026-11-18 实测**；M4.13 / M4.14 各新增一个用例，计数由 25 升到 27，2026-11-18 的 VFS 门新增 `aarch64_vfs` 再升到 28）。该选择器产出独立的 `kernel/m4-1` 和 `image/m4-1`，使用 BSP 启动栈的高地址别名，在 GIC、定时器和 AP 启动前验证私有 TTBR0、SVC 往返、权限故障和页回收。M4.2 在同一用例中验证 SVC 跨页读写、坏指针返回 `-EFAULT`，以及两条用户复制指令发生 EL1 data abort 后的恢复。M4.3 将探针编译为嵌入式静态 ELF，验证 ELF 入口、`.data` 初值、`.bss` 清零、坏入口拒绝和 4 KiB 页回收。M4.4 在单核定时器启动后、调度器接管前再次运行该 ELF：私有 TTBR0 保持安装，GIC/串口走 TTBR1 设备映射；自测要求至少两次来自 EL0 的 CNTP IRQ、寄存器和栈完好及 SVC 退出。M4.5 在 AP 启动前让两个私有地址空间通过 SVC 按 A → B → A → B 切换，验证相同虚拟地址的数据隔离、寄存器和栈恢复，以及退出后的页回收。M4.6 在单核定时器启动后复用两个私有地址空间，要求 A → B → A → B 三次切换均由来自 EL0 的 CNTP IRQ 触发，且在 GIC 应答后才替换返回帧；SVC 只查询状态和报告结果。M4.7 在 BSP 调度器启动后，由一个固定到 CPU0 的内核线程运行同一 ELF，验证 SVC 退出码、启动页表恢复、用户页和线程资源回收，以及后续内核线程仍能运行。M4 用户页表验收（`aarch64_user_mm`）在 BSP 调度器就绪后由 CPU0 内核线程创建 mm 私有用户根，全程经 M3 4 KiB 入口映射探针代码（复制后按 `dc cvau` + `ic iallu` 同步，RO/X 安装）、栈与数据页，三轮进入同一张页表：第一轮读写已知值并在 SVC 回调中验证 IRQ 屏蔽下 map/update/unmap 全部 `-EBUSY`；同一 VA 同一 PA 降为 RO/NX 后第二轮验证写入触发 EL0 权限异常且原数据不变；解映射并核对返回 PA 后第三轮验证访问触发翻译异常；每轮核对启动 `TTBR0` 恢复。线程上下文再验证重复映射 `-EEXIST`、内核 VA、裸 `VM_USER_RW`、未分配 PA、换 PA 更新 `-EINVAL`、空槽更新 `-ENOENT`，独立 L2 分支建表后解映射留下空表，仍有叶子时销毁 `-EBUSY`（前后 4 KiB 分配数不变、两侧仍可操作），清理全部叶子与数据页后销毁成功且 4 KiB 计数回到基线。M4.8 正式用户任务验收（`aarch64_user_task_lifecycle`）在 BSP 调度器就绪后由 CPU0 内核线程发布两个真正的 `PF_PROCESS` 用户任务：A 与 B 跑同一张 VA 布局的静态 ELF，只有数据初值不同，各自拥有私有 `mm`（独立用户根）、独立内核栈和独立 EL0 异常帧（帧固定在任务内核栈顶）。两个任务各做 64 次有界协作让出（`svc #0x4d40`），每次让出前内核在 IRQ 屏蔽态把 `CNTP_TVAL_EL0` 重装为 1，使真实 CNTP 在恢复 EL0 后计入 vruntime；自测核对真实切换次数、SVC 往返数、来自 EL0 的 CNTP tick 计数，以及 IRQ 分派前后 PID 与切换计数不变。随后 A 依次尝试三次映像替换：无效目录项必须返回 `-ENOEXEC`、一次性注入的分配失败必须返回 `-ENOMEM`（注入点在一页映射成功之后）、真实替换必须成功且 PID 不变、异常帧被清零重填；B 触碰用户栈下方的 guard 页，必须产生 Data Abort（翻译错误）并以 `-EFAULT` 终止，ESR 与 FAR 被记录。两个任务退出后核对各自原始退出状态、4 KiB 页计数回到基线、发布根只剩内核根，并跑一个内核见证线程确认内核仍可运行。M4.9 正式 SVC 与 uaccess 验收（`aarch64_user_svc_uaccess`）在同一镜像里发布一个 `PF_PROCESS` 用户任务运行新的静态 ELF：`svc #0` 的全 64 位 x8 经共享 syscall 表进入正式入口，`getpid`/`getppid` 结果存入 x19/x20 并由内核在首次 `clock_gettime` 时从保存帧读回、分别与该任务 PID 和常驻 BSP idle PID 比对；16 字节 `timespec` 写入跨两页 RW 段边界，向只读代码页和跨 RW 段末尾未映射页写回均返回 `-EFAULT` 且后者保留前缀哨兵；未知 immediate、未编入的编号和高 32 位污染 x8 均返回 `-ENOSYS`；任务用原生 `exit` 退出，回收观察点核对 `PF_SELF_REAP` 与编码后的 `exit_code`；内核复制自测在首个 `clock_gettime` 中验证跨页 copy-in、零长度、溢出，并分别在未映射读和只读代码页写上到达真实 EL1 修复（各一次 `-EFAULT`、一次回调），随后成功复制证明窗口已清空。M4.10 匿名 VMA 与按需缺页验收（`aarch64_user_vm`）按顺序运行六个独立静态 ELF：正常路径验证 mmap/brk 只提交元数据（返回后目标页仍无叶子）、首次读取补零页、`PROT_NONE` 保留并在恢复后读回旧数据、内层 mprotect 产生 VMA 拆分、brk 收缩清零保留尾页且再次增长不重新分配、munmap 释放已 stash 页；两次 16 字节 `clock_gettime` 从 RW 页末 8 字节越过空洞写回都必须返回 `-EFAULT`，第一次不得实例化首叶，第二次不得改写已驻留页的 8 字节哨兵。只读页写、`PROT_NONE` 读和 munmap 后读各由独立任务触发：只读写走权限异常、后两者走翻译异常，均由旧终止路径以 `-EFAULT` 结束，内核观察点记录 ESR/FAR/原 PC 与是否重试。替换夹具先建立 present + stash 页，由观察点在首个 VM syscall 上武装一次性映像失败，使第一次替换在首叶建立后返回 `-ENOMEM`；旧根、VMA 与数据保持不变，恢复 stash 后读回原数据并重新 stash，第二次替换成功并由新映像退出 `0x33`。munmap、退出与成功替换分别释放一个 stash 页，最终核对 4 KiB 分配基线与发布根计数。M4.11 原生 fork 与写时复制验收（`aarch64_user_fork_cow`）在同一次单核 BSP 自测启动中发布真实父任务并经 `SYS_fork` 建立子任务：子帧复制父 SVC 帧（x0=0）、继承 SP 并从同一后续指令继续，父子根不同；父在子仍持有引用时写共享页触发真实多引用 COW，子仍读到 fork 前的值；两侧独立补齐缺页页；stash 往返、共享堆尾在 brk 收缩前私有化、父子各自的 `clock_gettime` 写回 COW 均成功；父 munmap 后子写升级唯一引用且 PA 不变；父先退出后子在 switch tail 被回收观察，`getppid` 指向 BSP idle，随后以继承目录 exec 清理剩余映射并释放最后一个计数页。同一用例再做一次短 fork（子立即退出，父确认回收并读取共享页）和一次只读页非法写（以 `-EFAULT` 退出），最后按 PID 核对退出状态、用户根撤销与全局 4 KiB 基线。M4.12 原生 waitpid 与 ZOMBIE 回收验收（`aarch64_user_waitpid`）在同一次单核 BSP 自测启动中发布父子任务：子进程退出后保留为 `TASK_ZOMBIE`，父进程经原生 `SYS_waitpid` 收取退出状态；父先退出时子重挂到 BSP idle，无父等待的孤儿僵尸由退出路径无锁回收。共享的 `do_waitpid()` 只在目标确为 `TASK_ZOMBIE` 且已离核时才收割。M4.13 原生 exec 与用户栈参数传递验收（`aarch64_user_exec`）在同一次启动中经 `SYS_exec` 替换映像：argv / envp 深拷贝后按 AAPCS 写初始栈帧（`argc`、指针表、NULL 终止、字符串降序），寄存器 `x0`/`x1`/`x2` 与 `elr_el1` / `sp_el0` / `spsr_el1` 就位；exec 重置全部 sighandler 为 `SIG_DFL`；参数总数超限返回 `-E2BIG`。M4.14 原生信号与 `sigreturn` 验收（`aarch64_user_signal`）在同一次启动中投递信号：`SYS_kill` / `SYS_signal` / `SYS_sigprocmask` 走共享表；handler 入口把 `sigframe` 压到用户栈并跳转，`SYS_sigreturn` 取回帧并恢复寄存器与屏蔽位，伪造帧（`spsr_el1` 不匹配）返回 `-EINVAL`。验收范围为单核自测启动的运行期链路。普通 AArch64 镜像同时编入正式装载器、spawn、fork/COW 与根撤销接口，GIC/PL011 走高地址别名，低 EL 同步（`VBAR+0x400`）与 IRQ（`VBAR+0x480`）向量在普通构建中可达；旧探针入口、测试观察／故障注入符号与嵌入测试 ELF 不进普通产物。EL0 执行期间屏蔽 IRQ；普通镜像继续关闭 AArch64 用户态。

AArch64 正式用户态运行期验收（`MODE=user-runtime`，2026-11-20 ~ 2026-11-21）：`make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime [USER_RUNTIME_CASE=irq|asid|smp|migration|product|fp]`（省略 `USER_RUNTIME_CASE` 时默认 `irq`）。`irq`、`asid`、`smp`、`migration` 分别以 `KERNEL_SELFTEST=1 AARCH64_USER_RUNTIME_TEST=irq|asid|smp|migration` 构建隔离的 `kernel/user-runtime-<case>` + `image/user-runtime-<case>` 变体（`irq`/`asid` 单核，`smp`/`migration` 2 核），由 `qemutests/aarch64_user_runtime.py` 在 120 s 绝对期限内解析对应 `[user-runtime] irq-samples|asid-samples|smp-samples|migration-event` 与 `[user-runtime] PASS case=<case> cpu_mask=<mask>` 证据（超时、fatal、自退、缺失或重复必需记录均失败）；其中 `migration` 验证跨核迁移 3 次往返、8-tick 空闲平衡、VM/TLBI/COW、`uaccess`、`exec` 与零页/ASID/根泄漏；`product` 直接构建并启动普通 `aarch64-uefi` 产品镜像，覆盖 1/2/4/8 核启动至 `OS01 Shell` 及 2 核串口 `echo hi` / `exit` / respawn 交互，并用 `llvm-nm` 审计普通 `kernel.elf` 不含 `aarch64_user_runtime_test_*` / `test_aarch64_user_runtime_*` 符号；`fp`（2026-11-21 EL0 FP/SIMD 剩余项新增）同样直接启动产品镜像（盘上随产品用户态安装 `/bin/test_fp`），1/2/4/8 核各经 busybox ash 实跑 `/bin/test_fp` 并断言独立 `test_fp: PASS` 行后 `exit` / respawn——这是 EL0 浮点指令不陷入、libc FP printf、fork 穿透 FP 继承与上下文切换 FP 存活的运行时证据。runner 单元测试见 `qemutests/test_aarch64_user_runtime.py`（注册于 `make test-harness`）。

环境注意（2026-10-31 记录）：宿主 QEMU 11.1.3 + 当日 edk2-nightly 固件下，默认 FDT EFI-config-table 交付路径在 `[dtb] FATAL: UEFI handoff has no DTB` 处确定性失败（复现证据与评估见 `docs/superpowers/reports/2026-10-30-aarch64-el1-sched-validation.md`）。仓库全部 aarch64 QEMU 验收面已统一走诊断 DTB 机制（`smp`/`no-ack` 默认 `--diagnostic-dtb=auto`，`gic-spi`/`m3-probe`/M1/`sched`/idle-smoke 显式传入或自行生成 QEMU DTB 并配 `acpi=off`）；`AARCH64_UEFI_SMP_DIAGNOSTIC_DTB=0` 可强制要求生产固件路径。这是固件/宿主侧行为变化，不是仓库回归；未在本期改写固件管线。

AArch64 M1：`MODE=m1-ram` 分别构建 normal/selftest 独立镜像并跑 16 组 RAM/CPU 矩阵。其余 `m1-*` MODE 构建 `AARCH64_M1_TEST=sparse|arena-exhaust|table-exhaust|ap-bad-root` 对应的隔离 `image/m1-<case>/` / `kernel/m1-<case>/` 变体；该旗要求 `KERNEL_SELFTEST=1`，拒绝与 sync-fault、weak-selftest 或 canary 混用。稀疏 map 仅由编译旗改写，不从环境变量注入。`make PROFILE=x86_64-clang test-m1-host [CASE=m1-layout|m1-reservation|m1-arena|m1-tree|m1-contract-x86|m1-install|m1-publish]` 为原生 focused 测试入口，省略 CASE 跑整组。

Resolution Switcher：`make PROFILE=x86_64-clang test-resolution-host [RES_CASE=uapi|state|writers|bga|ioctl|pty|terminal|clients|hooks|all]` 为分辨率切换器宿主聚焦测试分发入口，省略 `RES_CASE` 时默认 `all`（运行所有已注册 case）；未知或未注册 case 报错非零退出。端到端套件为 `make PROFILE=x86_64-clang test-qemu SUITE=resolution`（生产配置：QMP surface + `setres` 会话正常用例，含 800×600 往返、空闲后台切换、白名单外初态查询、三启动镜像隔离校验，以及 `-vga cirrus` 无 BGA 设备用例——SET/枚举返回 ENODEV 而信任的 GOP framebuffer、read/surrender 与串口会话仍存活）；`make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution` 使用隔离的 `resolution-test` 镜像运行 `/dev/fbtest` 故障子集（读回失配、排空超时、terminal-ENOMEM、SIGWINCH、raw mmap sticky、回滚失败）。QMP 连接失败或截图失败一律 FAIL，不跳过。结果与截图落在 `build/x86_64-clang/test-results/resolution/`。

Standalone (not bucketed): `test-syscall-repeat` (own harness),
`test-user-canary` (subset of test-static, distinct prereqs),
`test-pmm-boot-reservation` (subset of test-host). 详见第 3 章 §3 test bucket model。

### 必要的依赖项

1. **编译工具链**
   * Clang/LLVM (用于编译内核，`x86_64-unknown-none` 目标)
   * GNU Make
   * ld.lld (链接器)

2. **构建依赖**
   * mkfs.vfat (from dosfstools)
   * mmd (from mtools)
   * mke2fs / debugfs (from e2fsprogs)
   * aarch64 启动还需要 `mkfs.fat`/`mcopy` 与 `/usr/share/edk2/aarch64/QEMU_EFI.fd`（edk2-aarch64）

3. **运行和调试**
   * QEMU（`qemu-system-x86_64`；aarch64 用 `qemu-system-aarch64`）
   * OVMF.fd（UEFI 固件；x86_64 profile 在首次使用时自动获取 profile 私有副本，见[固件与手动 QEMU](#4-固件与手动-qemuprint-run-paths)）

### 安装依赖项

#### Ubuntu/Debian 系统

```bash
sudo apt update
sudo apt install clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86

# OVMF.fd 无需手动下载：x86_64 profile 首次使用时自动获取
# （OVMF_FIRMWARE_SOURCE，默认 https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd）
# 到 build/<profile>/firmware/OVMF.fd
```

#### Arch Linux 系统

```bash
sudo pacman -S clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86_64 edk2-ovmf

# 可选：改用发行版固件而不是默认下载
# （OVMF_FIRMWARE_SOURCE 只接受 https:// URL 或已存在的绝对本地文件路径）
make PROFILE=x86_64-clang disk.img OVMF_FIRMWARE_SOURCE=/usr/share/edk2/x64/OVMF.fd
```

### 编译步骤

#### 1. 克隆项目

```bash
git clone <项目仓库地址>
cd OS01
git submodule update --init   # BusyBox 与 posix-uefi 子模块
```

#### 2. 编译整个项目

```bash
make
```

等价于 `make PROFILE=x86_64-clang disk.img`。此命令会执行：

1. **发布 sysroot generation**：按 staging manifests 安装内核头文件、libc/libk（含真实 `usr/lib/libm.a`）、mbedTLS、compat-libs（仅 `librt.a` 桩），组装不可变 generation 并原子发布 `build/x86_64-clang/sysroot` 符号链接
2. **编译引导程序**：posix-uefi runtime adapter（受控拷贝 + patch）→ BOOTX64.EFI
3. **编译内核**：`artifacts/kernel.bin`（中间文件在 `build/x86_64-clang/kernel/`）
4. **编译用户程序**：`artifacts/user/*.elf`（init/spin/sigtest/.../nettest/tetris）
5. **编译 BusyBox**：`artifacts/user/busybox.elf`
6. **创建磁盘镜像**：`build/x86_64-clang/image/disk.img`（GPT 双分区），并内容保护地复制到项目根 `disk.img`

> **失效语义（增量构建）**：sysroot 以不可变 generation 发布。任一 sysroot 内容变化（内核/libc 头文件或库）会使 generation id 递增，内核、用户程序与 BusyBox 因 generation 变化而**整体重编译**（`-B`/digest），而非只重编依赖者——这是不可变 generation 设计的取舍。组件自身源码修改仍走 .d 依赖图只重编依赖者。

#### 3. 编译单个组件

```bash
make kernel.bin      # 内核（默认 profile）
make lib             # sysroot 库（含 sysroot generation 发布）
make user            # 用户 ELF 与 BusyBox
make image           # 磁盘镜像（产物路径，不是项目根副本）
```

#### 4. 固件与手动 QEMU（print-run-paths）

x86_64 profile 的 UEFI 固件是 **profile 私有** 的，位于 `build/<profile>/firmware/OVMF.fd`（不是源码树里的 `boot/uefi/OVMF.fd`）。首次使用时由根 Makefile 的固件规则从 `OVMF_FIRMWARE_SOURCE` 获取——只接受 `https://` URL（下载到临时文件后原子 rename）或已存在的**绝对**本地文件路径（内容保护拷贝），其它值在下载前报错：

```bash
make PROFILE=x86_64-clang disk.img OVMF_FIRMWARE_SOURCE=/abs/path/to/OVMF.fd
```

所有 x86 QEMU 入口（`run`/`run-kvm`/`run-virtio`/`debug`/`test-*`）都直接使用该 profile 固件与 profile 镜像（`build/<profile>/image/disk.img`），绝不读源码树固件，也绝不把项目根 `disk.img` 作为输入。

手动启动 QEMU 时先解析路径：

```bash
make PROFILE=x86_64-clang print-run-paths
#   firmware=/home/.../build/x86_64-clang/firmware/OVMF.fd
#   image=/home/.../build/x86_64-clang/image/disk.img
```

再把这两条路径分别填入 `-drive if=pflash,format=raw,readonly=on,file=<firmware>` 与 `-drive file=<image>,format=raw,...`。

### 输出路径（profile 布局）

`x86_64-clang` profile 的全部输出位于 `build/x86_64-clang/`：

| 路径 | 内容 |
| --- | --- |
| `build/<profile>/artifacts/kernel.bin` | 内核 |
| `build/<profile>/artifacts/user/<name>.elf`、`busybox.elf` | 用户程序（variant 时在 `artifacts/user/<variant>/`） |
| `build/<profile>/artifacts/uefi/BOOTX64.EFI` | EFI 应用 |
| `build/<profile>/image/disk.img` | 普通磁盘镜像 |
| `build/<profile>/image/systest/disk.img` 等 | 测试 variant 镜像 |
| `build/<profile>/sysroot` | 指向当前 generation 的符号链接 |
| `build/<profile>/sysroot-generations/<id>/` | 不可变 sysroot generation |
| `build/<profile>/kernel`、`user`、`libc`、`uefi`、`uefi-runtime`、`thirdparty/` | 组件专有中间产物 |
| `build/<profile>/staging/<component>/` | 组件安装暂存树 |
| `build/<profile>/host-tools/mkdisk` | 宿主构建工具 |
| `build/<profile>/host-test` | 宿主测试对象与二进制 |
| `build/.locks/<profile>/publish` | 发布锁（在 `build/<profile>/` 之外，`clean` 不删除） |

项目根 `kernel.bin` 与 `disk.img` 是默认 profile 产物的**单向兼容副本**（内容保护：内容相同则不覆盖）。

### 项目结构

#### 主要目录

* `boot/` - 引导程序相关代码
  * `uefi/` - UEFI 引导程序
* `kernel/` - 内核代码
  * `kernel/` - 内核主文件（main.c, printk.c, panic.c, log.c 等）
  * `arch/x86_64/` - x86_64 架构代码（head.S, trap.c, subsys.c）
  * `intr/apic/` - APIC 子系统（acpi.c, lapic.c, ioapic.c, ipi.c, lapic_timer.c）
  * `block/` - 块设备层
  * `driver/` - 驱动程序
  * `fs/` - 文件系统（VFS, FAT32, ext2, devfs, procfs, tmpfs）
  * `intr/` - 中断处理（irq.c, softirq.c, dispatch.c）
  * `memory/` - 内存管理
  * `sched/` - 调度器
  * `subsys/` - 子系统注册框架
  * `time/` - 时钟源与定时器
  * `tty/` - TTY 子系统
  * `percpu/` - 每 CPU 数据结构
* `libc/` - 系统库（libk 供内核，libc 供用户程序）
* `user/` - 用户空间程序
* `config/` - 配置文件（busybox.config.in、rootfs.mk、inittab 模板、posix-uefi patch）
* `mk/` - 构建模块（project.mk、profiles/、targets/、toolchains/、components/）
* `hosttests/` - 宿主测试代码
* `qemutests/` - E2E 测试脚本（run_test.py）
* `tools/` - 构建工具（mkdisk）
* `docs/` - 文档
* `thirdpart/` - 第三方依赖（posix-uefi, busybox-1.36.1, mbedtls）

#### 编译产物

* `build/<profile>/artifacts/uefi/BOOTX64.EFI` - UEFI 引导程序（aarch64 profile 为 BOOTAA64.EFI）
* `build/<profile>/firmware/OVMF.fd` - profile 私有 UEFI 固件（x86_64）
* `kernel.bin` - 内核二进制（项目根目录，默认 profile 兼容副本）
* `build/x86_64-clang/artifacts/kernel.bin` - 内核 artifact
* `build/x86_64-clang/kernel/kernel.elf` - 内核 ELF（含调试符号，供 GDB 使用）
* `build/x86_64-clang/artifacts/user/*.elf` - 用户程序 ELF
* `disk.img` - GPT 双分区磁盘镜像（项目根目录，默认 profile 兼容副本）
* `build/x86_64-clang/image/disk.img` - 磁盘镜像 artifact

### 常见问题和解决方案

#### 1. 编译失败

**问题：找不到 clang 或其他编译工具**

解决方案：确保已正确安装所有编译工具和依赖项。工具链可覆盖：`make CLANG=clang-22 kernel.bin`（详见 [`toolchain.md`](toolchain.md)）。

**问题：缺少固件（OVMF.fd）**

解决方案：无需手动下载。x86_64 profile 在首次使用 QEMU 入口时自动从 `OVMF_FIRMWARE_SOURCE` 获取固件到 `build/<profile>/firmware/OVMF.fd`；用 `make PROFILE=x86_64-clang print-run-paths` 查看当前固件/镜像路径。

**问题：`PROFILE='...' lacks capability '...'`**

解决方案：该 target 在当前 profile 下不可用。改用具备对应能力的 profile，或换用与 profile 匹配的 target。

#### 2. 运行失败

**问题：QEMU 无法启动**

解决方案：检查 QEMU 是否正确安装；profile 固件会自动获取，可用 `make PROFILE=x86_64-clang print-run-paths` 确认固件/镜像路径（首次 QEMU 运行会创建 `build/<profile>/firmware/OVMF.fd`）。

**问题：系统启动后无输出**

解决方案：检查串口连接是否正确，确保 `serial_printk` 函数被正确调用。

### 开发技巧

#### 快速编译和测试

```bash
make disk.img
make run
```

#### 清理项目

```bash
make clean
```

此命令持有发布锁并确认没有 generation read lease 后，删除 `build/x86_64-clang/`（不会删除 `build/.locks/` 或其他 profile 的目录），并删除默认 profile 拥有的项目根兼容文件（`disk.img`、`kernel.bin`）。非默认 profile 的 `make PROFILE=<name> clean` 只删除该 profile 的 `build/<profile>/`，不动项目根兼容文件。

### v25 增量：UEFI 残留排除（`mk/components/uefi.mk`）

**问题**：linked git worktree 的 `git -C posix-uefi status --porcelain` 用 submodule `config.worktree` 路径解析到 MAIN checkout（不在 worktree），与 find-based digest（跑在 OS01_ROOT 看 worktree 内容）不一致，导致前次 x86_64 build 残留 `*.o` / `*.a` / `*.lib` 经 `uefi/*.o` glob 进 aarch64 `BOOTAA64.EFI` ld.lld 链接产生 **duplicate-symbol + machine-type-mismatch**。

**修复**（`5dbc63d`）：

1. `find thirdpart/posix-uefi -type f` digest 用 `! -name "*.o" ! -name "*.a" ! -name "*.lib"` 排除
2. `cp -a "$(UEFI_RUNTIME_SOURCE)/." "$(UEFI_RUNTIME_DIR)/"` 后加 `find "$(UEFI_RUNTIME_DIR)" \( -name "*.o" -o -name "*.a" -o -name "*.lib" \) -delete`

干净 checkout 无行为变化。验证 selftest 21/21 + syscall 268/268 + aarch64 UEFI BOOTAA64.EFI ARM64 PE32+。

## 第 2 章 · 构建、运行与调试操作

> 本章翻译自 `docs/build-run-debug.md`（英文）。

### 1. 运行系统（x86_64）

```bash
make run
```

运行 QEMU（`-M q35 -smp 2`）+ OVMF 固件 + 磁盘镜像，串口输出到 stdio。能力说明：`run` 需要 `rootfs` 能力，所以 `make PROFILE=aarch64-clang run` 在 capability gate 处失败。

```bash
make run-kvm        # KVM 加速
make run-virtio     # virtio-net 代替 e1000e
```

### 2. 固件（profile 私有）

x86_64 profile 拥有位于 `build/<profile>/firmware/OVMF.fd` 的 UEFI 固件——绝不在源码树中。首次使用 root-owned 固件规则从 `OVMF_FIRMWARE_SOURCE`（默认 `https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd`）获取，只接受 `https://` URL 或已存在的**绝对**本地文件路径；其它值在任何下载/拷贝之前失败。

所有 x86 QEMU 入口（`run` / `run-kvm` / `run-virtio` / `debug` / `test-*`）使用此 profile 固件与 profile 镜像（`build/<profile>/image/disk.img`）——绝不读 `boot/uefi/OVMF.fd`，绝不用项目根 `disk.img`。

### 3. 调试

```bash
make debug
```

启动 QEMU 暂停 + GDB remote server 监听 :1234。另一个终端：

```bash
gdb build/x86_64-clang/kernel/kernel.elf
# target remote localhost:1234
# break kernel_main / continue / ...
```

`.vscode` 下的 VS Code 配置也驱动 `make debug`。

### 4. aarch64 UEFI 启动

```bash
make PROFILE=aarch64-clang run-aarch64-uefi
# 默认 SMP=1：串口依次可见 init pid=1 → OS01 Shell（`echo hi` 交互可用）。
# 多核（显式 SMP=N，N≥2）按设计降级 kernel-only（串口有 warn 行）；
# 多核用户态调度是 roadmap 的独立后续任务。
```

运行 `qemu-system-aarch64 -M virt -display none -serial stdio` + GPT 双分区启动镜像（ESP FAT32：BOOTAA64.EFI + kernel.elf；ext2 rootfs；固件在 pflash）。串口启动签名：`aarch64 uefi handoff ok`，接着 `phase1 boot ok`。`make PROFILE=aarch64-clang aarch64-uefi` 构建镜像 + 固件；`aarch64-uefi-kernel` 仅构建内核 ELF。三个目标都需要 `uefi` 能力（在 x86 profile 下干净失败）。

### 5. 测试

#### 日常检查与宿主测试

```bash
make test                            # 本地快速检查（默认 x86_64 profile）
make test-host                       # 仅宿主回归
make test-monitor-host CASE=file      # 只跑指定子系统的用例
```

`make test` 运行目录漂移检查、完整宿主回归、测试框架退出码契约和断言规范审计。首次运行会为 Doom 宿主用例准备 sysroot，但不构建客体镜像、不启动 QEMU，也不运行完整框架回归或静态审计。显式指定不具备 `rootfs` 能力的 profile 会失败，不会静默跳过宿主回归。

涉及 syscall、内核内部机制或架构启动的改动，仍需分别运行 `make OS01_SYSTEST=1 test-syscall`、`make KERNEL_SELFTEST=1 test-kernel-selftest` 或相应的 `test-aarch64` 用例；不要把 syscall 和内核自测的构建开关合用。

#### QEMU E2E 测试（variant-isolated images）

每个 x86 E2E 目标将其镜像 **variant** 构建到独立目录，并通过 `DISK_IMG` 环境变量对那个精确镜像运行 `qemutests/run_test.py`。Variant 构建**绝不删除或覆盖普通镜像** `build/x86_64-clang/image/disk.img`：variant 构建前后记录普通镜像的 sha256（`image/normal.before` / `image/normal.after`）并比对，因此触碰普通镜像的 variant 构建会大声失败。

| 目标 | Variant 镜像 | 套件 |
| --- | --- | --- |
| `make test-qemu SUITE=phase-0` | 普通 `build/<profile>/image/disk.img` | 启动 + shell 提示符 |
| `make test-qemu-usb-boot` | 普通 `build/<profile>/image/disk.img`（只读 USB 拓扑；源镜像前后 sha256 由专用目标比对） | USB boot M0 固件门禁（见下方 `test-qemu-usb-boot` 小节）：证明 profile OVMF 从 USB-only `qemu-xhci` + `usb-storage` 加载 `BOOTX64.EFI` + `kernel.bin` 并进入设备初始化。**注意**：该 `firmware` 用例断言的是 **MSC 驱动落地之前**的预期失败（`[boot-3] FATAL stage=fs component=root rc=-19`）；MSC 驱动已于 2026-11-14 落地，USB-only 引导现在会一路走到 shell，**该用例因此已过期并会超时失败**——见下方小节的「现状」段与 `docs/roadmap.md` P4 |
| `make OS01_SYSTEST=1 test-qemu SUITE=systest` | `build/<profile>/image/systest/disk.img` | syscall E2E（`OS01_SYSTEST=1`） |
| `make INITTAB_FILE=config/inittab.test test-qemu SUITE=inittab-phase` | `build/<profile>/image/inittab-test/disk.img` | inittab 阶段派发（`INITTAB_FILE=config/inittab.test`） |
| `make OS01_NETTEST=1 test-qemu SUITE=network` | `build/<profile>/image/nettest/disk.img` | 网络回归（`OS01_NETTEST=1`） |
| `make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress` | `build/<profile>/image/arch6-fsstress/disk.img` | ARCH-6 四核 FS 并发压测（`ARCH6_FSSTRESS_TEST=1` 且 `SMP=4` 为强制 gate；`QEMU_SMP` 若设置必须同为 4） |
| `make test-qemu SUITE=gfx` | 普通 `build/<profile>/image/disk.img` | ring-3 gfx / terminal / desktop smoke |
| `make test-qemu SUITE=resolution` | 普通镜像（每次运行复制为私有副本） | 分辨率切换 surface/session 验收（QMP + `setres`） |
| `make FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution` | `build/<profile>/image/resolution-test/disk.img` | 隔离故障子集（`/dev/fbtest`） |
| `make test-qemu SUITE=driver-model` | 普通镜像（matrix harness 另建 fault 变体） | 驱动模型矩阵 |
| `make test-qemu SUITE=monitor` | 普通镜像（`-snapshot` 只读启动，1/2/8 核各自拉 QEMU） | 终端 monitor 1/2/8 核矩阵 + 终端生命周期（q/SIGINT/SIGTERM/`--samples`/EOF）|
| `make test-qemu SUITE=terminal-present` | 隔离变体镜像 `build/<profile>/image/terminal-present/disk.img`（套件自带 `TERM_PRESENT_TEST=1` → `USER_DEBUG=1`，普通镜像串口保持干净） | Task 6 聚焦 QEMU 套件：GFX_PRESENT_RECT 像素正确性（QMP 截图）+ terminal 内 monitor ≥30 s 稳态字节/ns/窗口门控，对照 Task-3 baseline manifest；可通过 `TERMINAL_PRESENT_BASELINE_LOG` 覆盖 baseline 路径（默认 `qemutests/baselines/terminal-present/serial.log`，仓库内持久副本；原始 /tmp 证据已丢失）|
| `make test-qemu SUITE=gui` | 普通镜像（`-snapshot` 只读启动，1/2 核各自拉 QEMU） | GUI-1 LVGL 平台后端与故障注入验收（SMP 1/2 矩阵；boot、smoke、normal_exit、wait_quit、busy_owner、killed_owner、fatal_owner） |
| `make DOOM_TEST=1 test-qemu SUITE=doom` | 隔离变体镜像 `build/<profile>/image/doom-test/disk.img`（`DOOM_TEST=1`） | Doom SMP=1/2 QEMU 集成套件（真实按键注入、截图、生命周期与状态观测） |
| `make test-qemu SUITE=intr-x86` | 隔离故障变体镜像 `build/<profile>/image/intr-x86-fault-{bp,nmi,trace-read,trace-bad-fp}/disk.img`（套件自建，普通镜像不受影响） | x86_64 破坏性中断入口与异常回溯验收：内核 `#BP`（`int3`）、QMP `inject-nmi` 真实硬件 NMI、受保护帧读取修复（`trace-read`）与坏 FP 截断 3 帧回溯（`trace-bad-fp`）。每个用例单核启动、各自编译自己的 kernel/image 变体（选择器 `X86_INTR_FAULT_TEST=bp\|nmi\|trace-read\|trace-bad-fp`），判定只看 guest 串口里的完整原始 fatal 诊断（`do_int3(3)` / `do_nmi(2)` / 3 帧回溯与 `End Kernel Stack Backtrace`）+ ≥1 s 观察窗口；`#BP` 的保存 RIP 必须等于从**变体 ELF** 解析出的续执行标签 `x86_intr_fault_bp_resume`（绝不硬编码）。超时/静默/正常完成都不算通过。运行器：`qemutests/x86_intr_fault.py`（归档在 `build/<profile>/logs/tests/intr-x86/`） |
| `make test-qemu SUITE=boot-lifecycle` | normal 组用普通镜像只读（`-snapshot`），故障组各自隔离 `build/<profile>/image/boot-lifecycle-<case>/disk.img`（套件自建，普通镜像 hash 前后受 cmp 校验） | BOOT-1 CPU 生命周期 8 组矩阵（串行）：normal 1/2/4/8、delay-online 2/4、no-online 2、no-scheduler 2。选择器 `X86_SMP_BOOT_TEST=none\|delay-online\|no-online\|no-scheduler`（经 `KERNEL_EXTRA_CFLAGS` 送达 smp.c 的注入宏；正常构建零注入）。判定只看 guest 串口：`[boot-1] ready/complete`（normal）或唯一 `[boot-1] FATAL cpu=1 stage=<online\|scheduler>`（fatal），≥1 s 观察窗口，runner 自行停止 QEMU，超时/静默/QEMU 自退一律 FAIL。运行器：`qemutests/boot_lifecycle.py`（归档在 `build/<profile>/logs/tests/boot-lifecycle/`）。每 case 90 s 是单次 QEMU 上限，不是矩阵预算 |
| `make test-qemu SUITE=boot-failure` | normal 组用普通镜像只读（`-snapshot`），故障组各自隔离 `build/<profile>/image/boot-failure-<case>/disk.img`（套件自建，普通镜像 hash 前后受 cmp 校验） | BOOT-3 启动失败与降级策略 17 组矩阵（串行）：normal 1/2/4、required-subsys 1/2、optional-subsys 1/2、registration 1、root 1、tty 1、pty 1、stdio 1/2、init 1/2、child-fds 1/2。选择器 `BOOT_FAILURE_TEST=none\|required-subsys\|optional-subsys\|registration\|root\|tty\|pty\|stdio\|init\|child-fds`。判定只看 guest 串口结构化 `[boot-3] FATAL` / `[boot-3] WARN` 与能力哨兵，超时/静默/QEMU 自退一律 FAIL。运行器：`qemutests/boot_failure.py`（归档在 `build/<profile>/logs/tests/boot-failure/`） |
| `make test-qemu SUITE=boot-contract` / `make PROFILE=aarch64-clang test-aarch64 MODE=boot-contract` | normal 组用普通镜像只读（`-snapshot`），变体组各自隔离 `build/<profile>/image/boot-contract-<case>/disk.img`（套件自建，普通镜像 hash 前后受 cmp 校验） | BOOT-4 跨架构阶段契约与依赖拓扑 14 组矩阵（双架构各 7 组：normal 1/2/4、reverse 1/2、missing 1、cycle 1）。选择器 `BOOT_CONTRACT_TEST=none\|reverse\|missing\|cycle`。判定验证三检查点 `[boot-4] checkpoint=<devices\|pre-smp\|runtime> status=ok` 全序、依赖预检与拓扑保序、missing/cycle 真实拒绝。运行器：`qemutests/boot_contract.py`（归档在 `build/<profile>/logs/tests/boot-contract/`） |
| `make test-qemu SUITE=boot-services` | normal 组用普通镜像只读（`-snapshot`），变体组各自隔离 `build/<profile>/image/boot-services-<case>/disk.img`（套件自建，普通镜像 hash 前后受 cmp 校验） | BOOT-5 调度启动与服务解耦 8 组矩阵：normal 1/2/4/8、no-net 1/2、overlap 2/4。选择器 `BOOT_SERVICES_TEST=none\|overlap`。判定验证标桩全序 `[boot-5] vfs-runtime` → `[boot-5] init-created pid=1` → `[boot-5] runtime-ready window=closed`，PID 1 优先分配与并发发布安全。运行器：`qemutests/boot_services.py`（归档在 `build/<profile>/logs/tests/boot-services/`） |
| `make test-qemu SUITE=usb-boot` | 普通镜像与独立 `mkdisk` 生成的 AHCI 镜像各自复制到作业私有副本并注入不同 `/usb-root.txt` 与 `/boot/USBESP.TXT` 标记（`persistence` 还会把测试专用 helper `usb_boot_probe.elf` 仅注入私有副本 `/bin/usb-boot-probe`） | 依次执行 `readonly`、`dual`（USB `bootindex=0` + AHCI `bootindex=1`，验证 AHCI 先注册但内核按固件 ESP GUID 挂载 USB）、`ahci`（AHCI-only 基线）、`persistence`（可写私有副本执行 `/bin/usb-boot-probe` + `sync()` + QEMU trace 校验 `WRITE(10)` `0x2a`→`SYNCHRONIZE CACHE(10)` `0x35`→重启只读回读）、`superspeed`（`p2=0,p3=4` + `speed=4`）、`hub`（USB-only `p2=4,p3=0` + `usb-hub` port 1 + `usb-storage` port 1.2）六组矩阵；单组可用 `make test-qemu-usb-boot USB_BOOT_CASE=<readonly\|dual\|ahci\|persistence\|superspeed\|hub\|firmware>`。运行器：`qemutests/usb_boot.py` |
| `make test-qemu SUITE=usb-hid` | 普通镜像复制到作业私有副本并仅向该副本注入 `usb_hid_probe.elf`（`/bin/usb-hid-probe`），源镜像前后 sha256 不变 | 依次执行 `direct`/`hub` × `SMP=1`/`2` 四组矩阵（`i8042=off`，QMP `input-send-event` 注入、租约与 PTY 隔离、正常 unbind 后 TTY/shell 恢复、共享 Hub 并发与 detach 后 BOT `WRITE(10)`→`SYNCHRONIZE CACHE(10)`、`device_del` 合成释放）；单组可用 `make test-qemu-usb-hid USB_HID_CASE=<direct\|hub> SMP=<1\|2>`。运行器：`qemutests/usb_hid.py` |
| `make CAPACITY_TEST=1 test-qemu SUITE=arch10-capacity` | `build/<profile>/image/arch10-capacity/disk.img` | 隔离容量验收变体（`CAPACITY_TEST=1`）：12 假块设备 + 40 devfs 节点 + poll32/64 入睡/唤醒 + RLIMIT_NOFILE (64) |

### `test-qemu-usb-boot`：USB boot M0 固件门禁（opt-in）

> **现状（2026-11-16 实测，如实记录）**：`USB_BOOT_CASE=firmware` **已过期并会失败**。
> 该用例断言的预期证据是 MSC 驱动落地**之前**的根挂载失败
> （`[boot-3] FATAL stage=fs component=root rc=-19`）；MSC 驱动已于 2026-11-14 落地，
> USB-only 引导现在一路走通到 shell：
> `[xhci-msc] disk=usba` → `fs: boot disk selected by GUID (usba), root=usba2 boot=usba1 (/boot ok)`
> → `init: running as PID 1` → BusyBox ash 提示符。
> 预期证据因此不再出现，runner 报 `timeout before firmware evidence`（exit 2）。
> **这不是回归，而是门禁断言落后于实现**——转换方案（改为成功门禁，或退役并由
> `readonly` 用例覆盖）见 `docs/roadmap.md` P4。

`make PROFILE=x86_64-clang test-qemu-usb-boot USB_BOOT_CASE=firmware`
是 USB 启动计划的 **M0 固件检查点**，验收目标只有一个：profile OVMF 能从
**USB-only** 拓扑（单块 `usb-storage` 挂在 `qemu-xhci` 上，`p2=4,p3=0`，
`port=1,bootindex=0`）加载 ESP 上的 `BOOTX64.EFI` 与 `kernel.bin`，并让内核
进入设备初始化。

- **预期证据**（按顺序，缺一不可）：xHCI 端口 `speed=3`（high speed）→
  `[xhci] slot N: configured with config X (iface class 08:06, ep_count=2)` →
  以 `[boot-3] FATAL stage=fs component=root rc=-19` 开头的行。
  `boot_failure_stop()` 会在该行后追加 `phase=... cpu=...`，runner 只匹配稳定前缀；
  其它 rc、OVMF 横幅单独出现、标记乱序都不算通过。
  **（以上证据链已于 MSC 驱动落地后失效，见上方「现状」段。）**
- **拓扑独立**：`qemutests/run_test.py` 的默认 argv 总会追加 AHCI 根盘，因此 runner
  通过共享构造器 `qemutests/harness/qemu_cmd.py::build_qemu_x86(storage="usb")`
  显式选择 USB 拓扑；唯一非 pflash 盘就是 `usbdisk`，没有 AHCI/virtio/隐式盘，
  也没有 `-boot order`。
- **超时与清理**：使用现有 `ProcessSession`（绝对单调截止时间，SIGTERM →
  SIGKILL 有界清理），QEMU 输出再多也不会延长期限；串口全文、argv 与 metadata
  保存在 `build/<profile>/test-results/usb-boot/<UTC>-<pid>/`。
- **镜像只读**：`test-qemu-usb-boot` 在运行前后对普通镜像做 sha256 比对，
  任何改动都会使目标失败；专用目标不构建镜像变体，因此被排除在通用
  pre/post normal-image guard 之外。
- **已登记自动化 bucket**：`usb-boot` 已在生成的 suite catalog
  （`config/test-bridge.json` → `mk/generated/test-dispatch.mk`）中，并登记为 nightly
  （strict，`config/test-ci-schedule.json:199`）。六组子用例为 `readonly` / `dual` /
  `ahci` / `persistence` / `superspeed` / `hub`（`mk/components/run.mk:1447`），
  `USB_BOOT_CASE` 默认即 `readonly`（`:1225`）。**唯一遗留的过期断言是 `firmware` 子用例**
  ——它仍断言 MSC 驱动落地前的预期失败 `[boot-3] FATAL stage=fs component=root rc=-19`
  （`qemutests/usb_boot.py:78`），而 USB-only 引导现已走通到 shell；该用例已非默认路径，
  也不在六组循环内，只可显式 opt-in。手动诊断可用
  `make run-usb`（同样的 USB 拓扑 + 交互式 display/SMP/网络设置，或 OVMF 启动菜单）。
- **xHCI 硬件支持范围（Hub 引导扩展）**：支持根端口直连与外置 USB Hub 级联枚举（深度 0–4、每 Hub 端口 1–15），目标盘为 BOT/SCSI LUN 0、512 字节逻辑扇区的 U 盘。支持 USB 2.0（FS / HS 单 TT，拒收 multi-TT alt0）与经核验的 SuperSpeed Gen1×1 Hub（校验协议段对称 5 Gb/s PSI 与标准 BOS 含 `0x0a` SuperSpeed Capability 且无 `0x0b` SuperSpeedPlus Capability；`0x2a` Hub 描述符后下发 `SET_HUB_DEPTH`；USB 2.0 同伴 Hub 独立枚举）。`HCCPARAMS1.AC64 == 0` 时所有 DMA 分配须位于 4 GiB 以下。关键诊断日志：`[xhci-hub] ready bdf=... root=... route=... slot=... protocol=usb2|usb3 ports=... ttt=...`、`[xhci] device bdf=... root=... route=... slot=... parent=... port=... speed=... speed_id=... iface=...`、`[xhci-msc] probing disk=...` / `[xhci-msc] disk=...` / `[xhci-msc] no-bot-interface` / `[xhci-msc] probe failed`。QEMU `hub` 用例（`usb-hub` 12 Mb/s）与 `superspeed`（直连）不能替代真机 HS TT 与 SS Hub 验收。
- **profile 覆盖**：`PROFILE=<name>` 决定固件与镜像来源；只有具备 `rootfs` 能力的
  x86 profile（默认 `x86_64-clang`）可用。`make PROFILE=aarch64-clang -n run-usb`
  必须解析期失败 `lacks capability 'rootfs'`。固件来源由
  `OVMF_FIRMWARE_SOURCE` 决定（默认 edk2-nightly HTTPS），可用绝对本地路径覆盖；
  本地覆盖仅用于诊断，不能作为默认来源的验收证据。门禁报告需记录实际启动固件的
  来源与 SHA-256（runner 将其写入 `metadata.json`）。

### `arch10-capacity`：隔离容量与工程完整性验收（ARCH-10 / Task 10）

`test-qemu SUITE=arch10-capacity` 运行在完全隔离的容量变体构建下：

- **选择器**：`CAPACITY_TEST=1`（仅支持 x86_64）。给 kernel 和 user 注入 `-DOS01_NOFILE_CAPACITY=64 -DCAPACITY_TEST=1`，使用独立的变体输出目录 `kernel/arch10-capacity/`、`user/arch10-capacity/` 与 `image/arch10-capacity/disk.img`。普通构建绝不包含测试夹具与测试二进制。
- **内核夹具**：`kernel/selftest/arch10_capacity_fixture.c` 仅在变体编译时参与链接，注册 12 个假块设备（`fakeblk0`..`fakeblk11`）与 40 个 devfs 字符节点（`arch10-node-00`..`arch10-node-39`），并挂载握手与入睡/唤醒跟踪钩子。
- **用户态套件**：`/bin/test_arch10_capacity` 由 `config/inittab.arch10-capacity` 启动，验证 32 项与 64 项 poll 真正入睡再被子进程管道写入唤醒、NOFILE 软硬限制约束与越界拒绝、高位描述符保持访问，以及读取全部 12 个块设备与 40 个 devfs 节点的唯一负载数据。

### `intr-x86`：隔离的破坏性中断入口验收（Task 5 / spec §9.3）

`test-qemu SUITE=intr-x86` 不复用普通镜像，也不复用普通 `kernel-selftest` 路径：

- **选择器**：`X86_INTR_FAULT_TEST=bp|nmi|trace-read|trace-bad-fp`（空 = 普通构建，无任何故障探针）。它经 `KERNEL_EXTRA_CFLAGS`（已在 `OS01_SUBMAKE_ALLOWED` 白名单内）把探针选择宏送进受控 sub-make 边界，再由 `kernel/Makefile` 决定是否把 `kernel/arch/x86_64/intr/fault_probe.c`（及 `fault_probe_bad_fp.S`）加进源文件列表——普通构建里既没有该目标文件，ELF 里也没有任何 `x86_intr_fault*` 符号。
- **变体**：每个用例一个自己的 kernel/image 目录（`kernel/intr-x86-fault-<case>/`、`image/intr-x86-fault-<case>/`），两个用例互不共享对象与镜像；与 selftest/systest/nettest/canary/weak/resolution/driver-fault 模式的组合在解析期即被拒绝，aarch64 profile 也直接拒绝该选择器。
- **探针位置**：`x86_64_boot_percpu()` 之后，AP bring-up、FS/TTY 与任务启动之前。此时 BSP per-CPU、IDT、串口和 APIC 已就绪；探针输出 `stage=post-percpu`，运行器要求该标记先于 armed/fatal。LAPIC 按 MADT type 4 的处理器 UID（或 `0xff` 全体）、LINT0/1 与极性/触发标志配置 NMI。此前 NMI 在 APIC 初始化后丢失的原因是内核屏蔽了 LINT1，不是 QEMU 的 early-boot 限制。
- **判定**：见 `qemutests/x86_intr_fault.py` 的 `fault_evidence()`——串口流里恰好一个 `armed`、恰好一个完整 fatal 诊断、armed 在 fatal 之前、fatal 之后 ≥1 s 观察窗口内没有新的完整心跳/继续标记/二次 fatal/panic。QMP 响应的到达时刻**不**参与排序（只有 guest 串口顺序算数）。

### `san-ubsan` / `san-kasan`：内核编译期插桩（roadmap P1）

两个独立构建开关给 x86_64 内核 C 源码加编译期 sanitizer（借鉴 ArvernOS 的 UBSan 运行时思路，影子几何与 clang 23 `kernel-address` 内联常量对齐）：

- **选择器**：`KERNEL_UBSAN=1`（`-fsanitize=undefined`，剔除 object-size 与 alignment——probe 实证 `gate.h:236` 的自由指针整型转型写会触发保守的 `__builtin_object_size` 误报，且 object-size 检查挂在 alignment 组里；Linux 对 x86 同样默认关 UBSAN_ALIGNMENT）与 `KERNEL_KASAN=1`（`-fsanitize=kernel-address -mllvm -asan-globals=0 -mllvm -asan-stack=0`）。`KERNEL_SANITIZER_TEST=ubsan|kasan` 追加注入探针 fixture（`kernel/selftest/test_kernel_sanitizers.c`，普通构建零符号）。与 selftest/systest/nettest/capacity/doom/fault-probe 等变体的组合在 `mk/project.mk` 解析期拒绝；aarch64 profile 直接拒绝两个 knob。
- **产物隔离**：kernel/image 变体目录 `san-ubsan/`、`san-kasan/`（`KERNEL_VARIANT` 与 `IMAGE_VARIANT` 双 slug）；KASan 把 kernel 编译重定向到 `x86_64-unknown-linux-gnu` 三元组（clang 驱动对 `x86_64-unknown-none` 拒收 `-fsanitize=kernel-address`），该重定向只发生在 `kernel/Makefile` 内部（`EFFECTIVE_CC` 替换），不泄漏进 user/UEFI 三元组。flags 进 `ALL_CFLAGS`，`.cflags` fingerprint stamp 自动失效。
- **KASan 影子几何**：`shadow(VA) = (VA >> 3) + 0xdffffc0000000000`（clang 23 kernel-address 内联常量，Linux 同款）。影子区在 PML4 slot 472（`0xffffec0000000000` 起）与 remap 帧缓冲影子的 slot 476。两阶段：head.S 静态零影子（1 GiB 窗口 ⇔ 物理 ≤ 8 GiB，普通构建逐字节不变）→ `kasan_shadow_install()`（`x86_64_boot_memory` 内、直映射就绪后）换成真实 2 MiB 影子帧（RAM + 早期/重映射帧缓冲 + early-MMIO 跨度），随后 `kasan_poison_arm()` 才允许 slab 毒化。运行期 MMIO 经 `x86_vmm_map_2m`/`x86_vmm_map_4k` 的映射钩子按需补影子页。插桩代码不得触碰低半区 VA（影子线性映射数学上只规范内核半区）：UEFI handoff 由去插桩的 `kasan_boot_entry` 拷贝、BSP GS 预置；`uaccess.c` 与 `pmm_arch.c`（恒等映射读 E820）整 TU 去插桩。全局与栈插桩本期关闭（内核无 `.init_array` 迭代、栈毒化从第一条指令就写影子），为 roadmap 后续项。
- **堆钩子**：`libc/stdlib/malloc.c` libk 模式 `malloc→kmalloc`、`free→kfree` 直转发，故只钩 `kernel/memory/slab.c` 即全覆盖；kmalloc unpoison `[ptr, ptr+req)` + redzone `[ptr+req, ptr+cache)`，kfree 毒化全对象 `0xfb`，slab 收缩回还 2 MiB 帧时清影子。
- **违规语义**：任何 `__asan_report_*` / `__ubsan_handle_*` = `log_err`（分类 + 源位置 + 影子帧诊断）→ `kpanic`（fatal 回溯随出）。不 recover。
- **验收**：普通构建与 master 逐字节一致；`KERNEL_SANITIZER_TEST=ubsan` 的探针在设备 bring-up 后触发 `signed integer add overflow at <file>:<line>` 并 panic；`KERNEL_SANITIZER_TEST=kasan` 触发 `WRITE of 1 byte(s) ... heap redzone (past requested size)`（shadow=0xfc、影子字节 `fc 00 00 00 fc…`）并 panic；无探针时两个插桩内核完整启动到用户态（网络 + DHCP）零违规。

systest variant 是 **compile-affecting** 的：`OS01_SYSTEST=1` 给 user CFLAGS 加 `-DOS01_SYSTEST`，所以 variant 的用户程序构建到独立对象/artifact 目录（`build/<profile>/user/systest`，`build/<profile>/artifacts/user/systest`），variant 镜像包含 systest 编译的 `init.elf`，启动 `/bin/systest` 而不是 BusyBox shell。`USER_DEBUG=1` 同样是 compile-affecting 的（`-DUSER_DEBUG=1`），用户程序构建到 `build/<profile>/user/user-debug` 与 `build/<profile>/artifacts/user/user-debug`，因此开启/关闭用户态调试日志不会互相污染对方的对象文件。另两个 variant（nettest、inittab-test）只改 inittab 文件和镜像目录，其用户二进制与普通构建共享。

也可以直接构建 variant 镜像：

```bash
make OS01_SYSTEST=1 image          # → build/x86_64-clang/image/systest/disk.img
make OS01_NETTEST=1 image          # → build/x86_64-clang/image/nettest/disk.img
make INITTAB_FILE=config/inittab.test image   # → .../image/inittab-test/disk.img
make CAPACITY_TEST=1 image  # → .../image/arch10-capacity/disk.img
```

#### 测试框架（runner、归档、退出码与观察窗口）

宿主 C 测试、x86/aarch64 QEMU 套件与静态审计共用 `qemutests/harness/` 中的进程/结果层（`ProcessSession`、`RunArchive`、`parse_v1`）。runner 直接调用时的退出码统一为：

| 退出码 | 含义 |
| --- | --- |
| `0` | PASS — 有效完整结果 |
| `1` | FAIL 或 TIMEOUT — 测试未通过（含超时） |
| `2` | ERROR — 配置/运行环境错误（缺可执行文件、不可用输入、镜像/固件缺失等） |
| `130` | Ctrl-C（SIGINT） |

Make/CI 只保证「零 / 非零」，不要求透传具体数值；`result.json` 记录 runner 与子进程的真实返回码、细分状态与中断原因（spec §6.2）。框架级不变式：`status="ERROR"` ⟺ 退出 2，`status="FAIL"`/`"TIMEOUT"` ⟺ 退出 1，`status="PASS"` ⟺ 退出 0；Ctrl-C 在 130 单独成行。

**运行归档。** 每次运行（成功、失败与超时都保留）归档到：

```text
build/<profile>/logs/tests/<suite>/<UTC-time>-<unique-id>/
  stdout.log      # 宿主命令输出或 guest 串口输出
  stderr.log      # 宿主进程 / QEMU 诊断
  result.json     # 同目录临时文件 + 原子 rename 替换
```

生命周期由用户清理 / `make clean <profile>` 管理，不自动删除。

> **已记录偏差（启动失败归档只含 `result.json`）。** 归档目录在 `ProcessSession.start()` **之前**创建，而 `stdout.log`/`stderr.log` 只有 `Popen` 成功后才打开。因此**启动失败**（可执行文件缺失等）的归档只有 `result.json`，没有 `stdout.log`/`stderr.log`。这是刻意的：启动失败时本来就没有子进程输出可采集，且修正它需要改变 `ProcessSession` 语义（方案明确禁止）。受影响的运行路径包括 `run_static_audit.py` 与 `run_hosttests.py`。

**`result.json`（schema v1）** 至少记录：schema 版本；run ID；git revision 与 dirty 状态；profile 与 suite；请求选择条件与声明/实际 ID 集合（普通 v1 套件）；实际 argv；CPU 数与内存；工具版本；镜像/固件路径与 SHA-256（有输入时，区分运行前/后哈希）；UTC 开始时间与耗时；runner / child 返回码；是否受控停止；最终状态；计数单位；各用例状态/理由；日志路径。

**计数单位（count unit）。** `count_unit` 描述报告**实际包含**的内容。仅当该次运行发布了**逐用例记录**时用 `case`：协议 v1 的内核自测（`declared_ids`/`observed_ids` 与逐用例结果齐备），以及迁移到 v1 的宿主二进制。旧格式/重复聚合套件用 `suite`：包括旧格式宿主二进制、`run_test.py` 的**全部**套件（它只把 v1 当作门控，不发布逐用例记录——`outcomes` 为空、`declared_ids` 为 null，故 `SUITE=systest` 亦为 `suite`）、内核自测的启动失败/中断 ERROR 归档（同样不发布任何逐用例记录）与 `systest-repeat` 之类的重复套件。静态审计用 `audit`。断言数不等于用例数；旧格式适配器**绝不**编造 guest 用例记录。

**`CASE` 选择。** 目前只有 systest 支持按用例选择：`make OS01_SYSTEST=1 test-qemu SUITE=systest CASE=<id>`，`<id>` 必须匹配 `[A-Za-z0-9_.-]+`（`ID` 由 `user/systest.c --list` 提供），Make 写入私有 inittab 行并经现有 `INITTAB_FILE` 传入。宿主 runner 另有 `--binary ID` 选择单个 `TEST_BINS` 二进制。其它套件（`phase-0`、`gfx`、`resolution`、`driver-model`）**不支持** `CASE`。

**1 秒观察窗口与其边界。** 套件收到完整结果后继续观察 **1 秒**（`ProcessSession.observe`，共享 `_panic_in` 检测器），以捕获结果到达后窗口内到达的尾部异常（如 panic），随后由 runner 主动停止 QEMU、排空并回收。窗口只检查「已到达 / 窗口内」的异常，**不保证**检测无限延后的崩溃。

> **已记录偏差（窗口归属）。** 1 秒窗口施加于**自身拥有该延迟**的套件（普通 x86/aarch64 套件与内核自测）。预期 fatal/故障套件不把 1 秒窗口作为验收依据：它们按各自套件拥有的证据规则判定——例如 sync-fault 要求 armed/fatal 顺序唯一、寄存器字段完整，且证据成立后由 runner 停止仍存活的 QEMU；QEMU 自行退出则失败。

**实际生效配置。** 文档区分「Make 输入」与「实际生效值」：

| 套件 | 实际 QEMU CPU 来源 | 记录字段 |
| --- | --- | --- |
| `test-qemu SUITE=...`（x86） | 单次用 `QEMU_SMP`（默认 `1`）；`phase-0` 和 `systest` 可用 `QEMU_SMP_MATRIX` 逐次设置 | 每次启动各有一个 `result.json.cpu_count` |
| `test-kernel-selftest` | `KERNEL_SELFTEST_SMP`（透传为 `--cpu`） | `cpu_count` |
| `test-aarch64 MODE=...` | 各 MODE 自带的 `--cpus` 列表 | 每个 (case, cpu) 一份报告 |
| 交互 `run` / `run-kvm` / `debug` | `SMP`（默认 `2`） | — |

例如，`make test-qemu SUITE=phase-0 QEMU_SMP_MATRIX="1 2"` 和
`make OS01_SYSTEST=1 test-syscall QEMU_SMP_MATRIX="1 2 4 8"` 都只准备一份对应镜像。
每个 CPU 数用各自的可写镜像副本，启动结束后清理副本，失败即停止；
归档仍按每次启动分开。矩阵只接受互不重复的 `1 2 4 8`，不能与 `QEMU_SMP` 同设。

本地、PR 和夜间的运行范围、保留的测试桩及计时样本，见
[测试分层与实测记录](../testing/testing-policy.md#88-运行层级与计时记录)。

### 6. 配置文件

系统行为通过 `config/` 配置（BusyBox 配置、`config/rootfs.mk` 磁盘镜像清单、inittab 模板 `config/inittab`、`config/inittab.systest`、`config/inittab.nettest`、`config/inittab.test`）。

## 第 3 章 · 构建系统 Harness

> 本章翻译自 `docs/build-system-harness.md`（英文）。本文档是 OS01 的 Makefile 构建、运行、调试、验证、测试、契约和维护入口的权威参考——`help` target、`AGENTS.md` 和 CI 脚本都以此为契约。Makefile 变更时，本文档与 `make help` 同步变更。

### 1. Capability gate（能力门控）

每个入口声明其需要的 profile 能力：`kernel`、`userland`、`rootfs`、`uefi`。门控由 `$(call require_capability,<cap>)` 在解析期强制执行，所以错误的目标调用会立即以 `PROFILE='<p>' lacks capability '<cap>'` 失败，而不是在链接时。

默认 `x86_64-clang` profile 声明 `kernel userland rootfs uefi`。
`aarch64-clang` profile 声明 `kernel uefi`。

### 2. 目标分类（target taxonomy）

| Bucket | 规范名 | 能力 | 用途 |
| --- | --- | --- | --- |
| Build artifact | `disk.img`、`kernel.bin`、`lib`、`user`、`image`、`sysroot` | profile-specific | 产生单个命名产物 |
| Run / Debug | `run`、`run-kvm`、`run-virtio`、`run-usb`、`debug` | `rootfs` | 对已有镜像启动 QEMU（`run-usb` 用 USB-only 拓扑，见下方 `usb-boot` 小节） |
| Bring-up | `aarch64-uefi`、`aarch64-uefi-kernel`、`run-aarch64-uefi` | `uefi` | AArch64 UEFI 启动链 |
| Validate | `validate`、`validate-kernel`、`validate-uefi`、`validate-profile` | `rootfs`（最后一个：`always`） | x86 产物健全性检查 + profile 检查 |
| Test | `test-qemu`、`test-qemu-usb-boot`、`test-host`、`test-static`、`test-kernel-selftest`、`test-aarch64`、`test-contract`、`test-harness`（框架回归；不带任何 profile 能力；不启动 QEMU）（8 个 bucket；见 §3） | varies | 端到端和审计套件 |
| Inspection | `print-run-paths` | `rootfs` | 为外部 QEMU 调用打印解析后的路径 |
| Maintenance | `clean`、`unlock-profile` | always | 生命周期 |

### 3. Test bucket model（测试桶模型）

**8 个测试 bucket 目标** — 4 个带 flag，4 个不带：

| Bucket | Flag | 取值 | 运行内容 |
| --- | --- | --- | --- |
| `test-qemu` | `SUITE=` | `phase-0`、`systest`、`inittab-phase`、`network`、`gfx`、`resolution`、`driver-model`、`monitor`、`terminal-present`、`gui`、`intr-x86`、`boot-lifecycle`、`boot-irq-time`、`boot-failure`、`boot-contract`、`boot-services`、`arch10-capacity`、`arch6-fsstress`、`doom` | `qemutests/run_test.py <SUITE>` 对匹配的 variant 镜像（`gfx`/`resolution`/`driver-model`/`monitor`/`terminal-present`/`gui` 用普通镜像；`resolution`/`monitor`/`terminal-present`/`gui`/`doom` 各自拉私有 QEMU 进程）；`intr-x86` 走专属目标 `test-qemu-intr-x86` → `qemutests/x86_intr_fault.py`，自建隔离 fault 变体 |
| `test-qemu-usb-boot` | `USB_BOOT_CASE=` | `readonly`（默认）/ `dual` / `ahci` / `persistence` / `superspeed` / `hub` / `firmware`（过期的预期失败断言，仅 opt-in） | 运行器经共享构造器 `qemutests/harness/qemu_cmd.py::build_qemu_x86(storage="usb")` 生成 USB-only 拓扑；不经 `run_test.py`（它总会追加 AHCI 根盘）。已登记生成的 suite catalog 与 nightly bucket |
| `test-aarch64` | `MODE=` | `smp`、`no-ack`、`gic-spi`、`sync-fault`、`serror`、`exception-trace`、`m3-probe`、`sched`、`m1-ram`、`m1-sparse`、`m1-arena-exhaust`、`m1-table-exhaust`、`m1-ap-bad-root` | `qemutests/aarch64_*.py` 之一 |
| `test-contract` | `PROFILE=` | `x86_64-clang`、`aarch64-clang` | `qemutests/build_contract.sh <PROFILE> <mode>` 按 profile mode 列表 |
| `test-host` | — | — | `os01_submake hosttests` + `pmm_boot_reservation_test.py` |
| `test-static` | — | — | 16 项静态审计（runtime-audit、stack-canary-audit、validate-kernel、runtime-link-order、kernel-runtime-link、syscall-boundary、kernel-layout、kernel-canary-contract、header-object-audit、stack-frame-audit、driver-model-boundary、gui-build-audit、intr-dispatch-audit、arch10-capacity-audit、assertion-policy、user-canary） |
| `test-kernel-selftest` | — | — | 启动 selftest 镜像 variant（`KERNEL_SELFTEST=1`）并解析 `[selftest]` 标记 |
| `test-harness` | — | — | Python `unittest` 框架回归（`TEST_HARNESS_MODULES` 列表；不启动 QEMU，含场景与契约校验） |

测试场景与分层职责规范参见 [`docs/testing/testing-policy.md`](../testing/testing-policy.md)；场景映射清单位于 `config/test-scenarios.json`（由 `python3 qemutests/scenario_catalog.py --check` 校验）。可通过 `make print-host-test-inventory` 打印宿主测试清单（由 `mk/generated/test-registry.mk` 提供 `HOST_TEST_INVENTORY`、`TEST_BINS` 与 `TEST_EXIT_STATUS_BINS`）；新增宿主测试只需在 `hosttests/cases/test_<name>.c` 头部声明 `@os01-test` 元数据并在对应 `hosttests/mk/<subsys>.mk` 中补充链接规则后运行 `make regen-test-catalog` 并提交生成物（不再手工调用 `register_host_test` 或手写 `TEST_BINS`）。端到端与架构套件清单通过 `make print-test-catalog` 导出 TSV（`TEST_QEMU_SUITE_IDS`、`TEST_QEMU_FLAVOR_*`、`TEST_QEMU_IMG_*` 与 `TEST_SUITE_CATALOG_DATA` 均由 `mk/generated/test-dispatch.mk` 生成，并在 `test-qemu` 解析期校验 `SUITE` 合法性），包含 13 个字段（`id profile capability variant cpus memory_mib timeout_s image_policy resource tier entrypoint prepare_target run_target`），由 `python3 qemutests/test_catalog.py --check` 校验其元数据完整性与 CI 调度归属（`--tier pr|nightly --format json` 导出过滤后的配置）。

**`test-harness`** 验证 runner、归档及框架契约。先跑框架退出码与 CI 日程检查，再由
`run_framework_tests.py` 运行生成的 `TEST_HARNESS_MODULES` 清单，不做目录自动发现。
直接运行的 QEMU 套件不放进该清单；需要真实 sysroot 的 `test_arch9_build_contract`
放在独立的 `test-contract` 里。

**独立的测试目标**（不归入任何 bucket，因为它们使用不同的 harness 或镜像 variant）：

| 独立目标 | 不归入 bucket 的原因 |
| --- | --- |
| `test-syscall-repeat` | 使用 `x86_64_systest_repeat.py`（与 `run_test.py` 不同）和普通镜像 variant |
| `test-user-canary` | `test-static` 的子集但可单独调用；前置条件（`USER_ARTIFACTS`、busybox、rootfs 清单）不同 |
| `test-pmm-boot-reservation` | `test-host` 的子步骤；可单独调用以便 PMM-only 调试 |

**Focused compatibility checks** 在 alias 窗口期间保留可见于 `make help`，保留其原始的窄行为：

| 目标 | 检查内容 |
| --- | --- |
| `test-kernel-layout` | 仅 x86 kernel ELF 布局 |
| `test-kernel-canary-contract` | 仅 kernel canary 编译旗标契约 |
| `test-aarch64-gic-spi` | 仅 PL011 RX 到 GIC SPI 注入 |
| `test-resolution-host` | 分辨率切换器宿主聚焦测试分发（`RES_CASE=`） |
| `test-intr-irq-x86` | x86 IRQ production-chain host subset；执行 `test_intr_irq_x86`，仍注册于 `test-host`；要求 rootfs 能力 |
| `test-monitor-host` | monitor 宿主聚焦测试分发（`CASE=`，见下） |
| `test-gui-host` | GUI 宿主聚焦测试分发（`CASE=`，见下） |
| `test-libc-host` | libc 补全计划宿主聚焦测试分发（`LIBC_CASE=`，见下） |

#### monitor 聚焦 CASE（`test-monitor-host CASE=`）

`make PROFILE=x86_64-clang test-monitor-host CASE=collect` 运行单个聚焦组。
合法 CASE 为 `file accounting pmm procfs collect model render`；省略、空值或
纯空白执行全部七组。只接受一个精确的 CASE 名称；`CASE=all` 不属于此接口，
执行完整组应省略 CASE。未知值、通配符及多个 CASE 均报错并列明合法值。
此入口要求 profile 的 `rootfs` 能力；能力不足或 CASE 非法时，在任何递归
构建前失败，包括 `-n` 和 `-j` 调用。分发回归接入 `make test-harness`。

#### GUI 聚焦 CASE（`test-gui-host CASE=`）

GUI-2 plan（2026-10-20）引入 `make PROFILE=x86_64-clang test-gui-host CASE=<name>`。合法取值由 `mk/components/run.mk` 中 `test-gui-host` 的 `host_case_group` 生成调用（`test-gui-host` 的 case 列表 + `case:hosttests-target` 映射，二者即单一事实来源）定义：

| CASE | 内容 |
| --- | --- |
| `platform` | GUI 平台后端（显示、tick、flush、生命周期） |
| `lvgl` | LVGL 移植层的宿主断言 |
| `devfs-input` | file-aware devfs read/poll 分派契约（host 编译真实的 `kernel/fs/devfs.c` + `file.c` + `poll.c`） |
| `input-frame` | PS/2 成帧（Set 1 序列、E0 前缀、帧完整性） |
| `input-route` | 内核输入路由与租约裁决 |
| `input-decode` | 扫描码 → 字符解码（修饰键、Caps 锁存） |
| `input-nonce` | 输入 nonce / 会话隔离 |
| `input-platform` | 输入与平台的挂接契约（每平台至多一个 input） |
| `input-lvgl` | LVGL indev 路径（光标、焦点、重复、取消协议） |

`CASE=all`（或不指定 `CASE`）跑全部。**只存在这一个列表**——不维护第二份并行的 CASE 清单。新增宿主二进制必须同时接入 `hosttests/Makefile::TEST_BINS`、`run_hosttests` 协议与本 focused 入口。

所有测试**只**从仓库根的 make 入口运行：`make PROFILE=x86_64-clang test-gui-host CASE=...`。**不要**用 `make -C hosttests`。

#### libc 聚焦 CASE（`test-libc-host LIBC_CASE=`）

libc 补全计划（2026-11-08）引入 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=<name>`。合法取值由 `mk/components/run.mk` 的**单一列表** `LIBC_HOST_CASES` 定义（计划固定集合）：

```
api alloc env shutdown stream append io line format integer float scan time path dir regex
```

`LIBC_CASE=all`（或不指定）跑**已交付**的组。已交付集合由 `LIBC_HOST_DELIVERED_CASES` 记录，每个补全任务把自己的 case 追加进去并新增对应的 `_test-libc-host-run-<case>` 规则：

| CASE | 内容 | 交付任务 |
| --- | --- | --- |
| `api` | 公开头契约：每个公开头自包含、跨 TU 只有一份定义、C++ 消费者拿到 C linkage；同时链接 `os01_` 前缀的生产对象 | T1 |
| `alloc` | 分配器的溢出检查与对齐分配：`calloc`/`reallocarray` 的乘法溢出、`posix_memalign`/`aligned_alloc` 的参数检查 | T2 |
| `env` | 单一环境变量表与逐项归属：`setenv`/`unsetenv`/`putenv`/`clearenv` 的可见性、借用语义与失败原子性 | T3 |
| `shutdown` | `shutdown(2)` 接真实 `SYS_shutdown`：参数转发、errno 映射、非法的 `how` 报 `EINVAL` | T4 |
| `stream` | 流状态机：`feof`/`ferror`/`clearerr`/`ungetc`（含 unlocked 版）与标准流生命周期 | T5 |
| `append` | 内核真实 `O_APPEND` 的原子追加（跨独立 fd 与共享 fd） | T6 |
| `io` | `fopen`/`fdopen`/`fclose`/`fread`/`fwrite` 与真实 `fcntl`、完整项 IO、close 错误 | T7 |
| `line` | 字符与行接口：`fgetc`/`getc`/`putc`/`fileno`/`fgets`/`getdelim`/`getline`（含 unlocked 版） | T8 |
| `format` | `dprintf`/`vdprintf`/`asprintf` 与 `fseek`/`fseeko`/`ftell`/`ftello`/`rewind`/`freopen` | T9 |
| `integer` | 整数转换：`strtol`/`strtoul`/`strtoll`/`strtoimax`/`strtoumax` 的范围、负号、endptr 与进制 | T10 |
| `float` | 浮点转换：`strtod`/`strtof`/`strtold` 按目标精度的舍入、特殊值与范围错误 | T11 |
| `scan` | `sscanf`/`vsscanf`：字段宽度、长度修饰符、`%n` 与扫描集 | T12 |
| `time` | 日历换算与 POSIX TZ：`gmtime`/`localtime` 及 `_r`、`mktime`、`strftime`、`strptime`、`ctime`/`asctime` | T13 |
| `path` | `realpath` 与 `exec` 家族：`execl`/`execlp`/`execle`/`execvp` 的参数、环境与 PATH 搜索 | T14 |
| `dir` | `opendir`/`readdir`/`closedir`/`dirfd`/`fdopendir`/`scandir`/`alphasort`：fd 归属、条目复制与排序 | T15 |
| `regex` | POSIX 正则引擎 | **未交付**（2026-11-09 用户决定本期不实现，命令按"已登记未交付"失败） |

行为约定：

- `test-libc-host` 需要 `rootfs` 能力（与 `test-monitor-host` / `test-gui-host` 相同）；
- 非法 `LIBC_CASE`（不在上面的固定集合内）在任何递归构建之前失败，退出码非零并列出合法取值；
- 已在固定集合内但**本阶段尚未交付**的 case 同样失败（非零），并打印已交付组，避免把“没有这个组”误报成通过；
- `LIBC_HOST_CASES`（合法性）与 `LIBC_HOST_DELIVERED_CASES`（已交付）分工不同：前者由计划固定，后者逐任务增长。

公开接口清单（哪些头、哪些接口、哪个任务负责）保存在 `scripts/audit_libc_api.py`，是唯一真源；宿主测试用的 C 清单由该脚本在构建时生成，测试与审计脚本不会各持一份。审计脚本同时检查：清单与头文件是否漂移、每个头是否被归类、用户 libc 的全量接口是否已导出、libk 是否仍导出既有子集。

归档侧的审计（导出符号、libk 子集未退化）有独立入口：

```
make PROFILE=x86_64-clang test-libc-api-audit   # 用户 libc + libk 归档审计
make PROFILE=aarch64-clang test-libc-api-audit  # 仅 libk（aarch64 不产出 libc.a）
```

libk 基线按 profile 区分（x86_64 与 aarch64 的 `libk.a` 导出集合不同），脚本接受 `--profile`，默认从归档路径推断。

#### libm 的归属与指令审计

基础代数数学集由独立的 `usr/lib/libm.a` 承载，来源是 libc stage（`libc/math/*.c`，vendored musl 1.2.5）：

- **归属**：`test-libc-api-audit` 断言这 16 个符号（`fabs/fabsf`、`copysign/copysignf`、`floor/floorf`、`ceil/ceilf`、`trunc/truncf`、`round/roundf`、`fmod/fmodf`、`sqrt/sqrtf`）**只**由 `libm.a` 导出，`libc.a` 与 `libk.a` 都不得定义它们——内核因此保持 FP-free。调用方需要显式链接 `-lm`（例如 `user/doom/app.mk`）。
- **指令**：`test-libm-asm` 审计**已发布**归档（`$(SYSROOT)/usr/lib/libm.a`）的反汇编，证明每个函数只用整数位运算：不得出现浮点加减乘除、FMA、平方根、比较、取整或整数↔浮点转换指令；浮点寄存器只允许被数据搬运指令触及（x86_64 的 `movsd/movss/xorps`，aarch64 的 `fmov/fcsel/movi/ldr/str`），这样 NaN 载荷与负零符号才能原样保留。profile 决定架构，外来架构的归档会被拒绝。

```
make PROFILE=x86_64-clang test-libm-asm   # x86_64 指令审计
make PROFILE=aarch64-clang test-libm-asm  # aarch64 指令审计
```

审计脚本自身的负向能力可用 `--selftest` 复现：它现场组装临时夹具，确认干净夹具通过，而 `addsd`、`cvttss2si`、`vfmadd213sd`（x86_64）与 `fadd`、`scvtf`、`frintn`（aarch64）逐一被点名拒绝，无法解析的指令行与错误架构同样被拒绝。

所有测试**只**从仓库根的 make 入口运行：`make PROFILE=x86_64-clang test-libc-host LIBC_CASE=...`。**不要**用 `make -C hosttests`。

#### GUI 头文件的发布规则（single-writer sysroot）

`libgui/Makefile::install` 只把**三个**公开头装进暂存树：

```
usr/include/gui/gui.h
usr/include/gui/gui_input.h
usr/include/gui/gui_platform.h
```

加上 `usr/lib/libgui.a` 与 `usr/share/licenses/libgui/LVGL-LICENCE.txt`。`install` 目标**拒绝**写到 `$(STAGING_DIR)/gui` 以外的任何位置，并且只产出 manifest；`mk/components/sysroot.mk` 是 sysroot generation 的**唯一写入者**，它消费 manifest 后原子发布 `build/<profile>/sysroot` 符号链接。

因此**发布出去的 GUI 树必须恰好是** `{gui.h, gui_input.h, gui_platform.h}`：

- 不得出现任何 `lv_*` 类型（LVGL 头是编译期依赖，不是发布面）。
- 不得出现私有桥接头（`libgui/internal/*.h` 只在编译期可见）。
- 不得出现测试注入接口。

`make PROFILE=x86_64-clang sysroot` 之后可以直接核对发布面：

```sh
ls build/x86_64-clang/sysroot/usr/include/gui/
```

### 4. Alias policy（别名策略）

Bucket 目标是规范名。**2026-09-26 cleanup 删除了所有转发别名**（`test`、`test-phase-0`、`test-syscall`、`test-inittab`、`test-network`、`test-aarch64-uefi-smp`、`test-aarch64-uefi-smp-no-ack`、`test-aarch64-gic-spi`、`test-build-contract-x86`、`test-build-contract-aarch64`）；CI 和所有调用方必须直接使用 bucket 目标。

**2026-10-06 例外（ARCH-9 Task 12）**：`test-syscall` 是**唯一**保留的转发别名，作为 `AGENTS.md` 行 60 + 70 的 exact-target 要求兼容入口。它的真实 recipe 是 `$(MAKE) OS01_SYSTEST=1 test-qemu SUITE=systest`，并在解析期有两个 gate：

* `OS01_SYSTEST` 必须为 `1`（systest variant 是这个 alias 唯一的 init 模式；普通 inittab 下不会以 PID 1 加载 `/bin/systest`）
* `KERNEL_SELFTEST` 必须**不**为 `1`（内核内自测在 boot 时 spawn kthreads，会干扰 systest 的 fork+exec+waitpid 测试）

新代码和 CI **不应**使用 `test-syscall`；用 `OS01_SYSTEST=1 test-qemu SUITE=systest`（bucket 目标）即可。本 alias 仅作为用户 AGENTS.md 的 exact-target 兼容垫片保留，未来若 AGENTS.md 撤回该要求，应在同一次 cleanup 中一并删除（参考 `task-12-report.md` 的撤销条件）。

新增其它转发别名**不**是替代品——它不能替代在新代码或 CI 中使用 bucket 目标。

**保留的 focused checks**：某些 `test-*` 名保留自己的原始 recipe（独立的针对性检查，不转发到 bucket）。为调试永久保留：

- `test-runtime` — runtime audit 子集（5 个 python 调用 + validate-kernel）
- `test-kernel-layout` — kernel.elf 在 `_end` 之后的保留布局审计
- `test-kernel-canary-contract` — kernel canary 编译旗标契约
- `test-user-canary` — 6 步 SSP / 用户栈 canary 审计
- `test-pmm-boot-reservation` — PMM 启动期内存保留守卫
- `test-resolution-host` — 分辨率切换器宿主聚焦测试分发入口

### 5. 添加新 target

1. 决定 bucket（build / run / debug / bring-up / validate / test / inspection / maintenance）。
2. 选择 flag 值（如果有），确认 recipe 不是已有 recipe 的简单变体——如果是，合并到 bucket 并加别名。
3. 添加 gate（`require_capability`），选择已有 Make 变量用于路径和工具，写 recipe。
4. 如果 recipe 含 `$(MAKE)` 调用（variant 构建、sub-make），放独立 recipe 行，使 `make -n` 遵守 dry-run 契约——见 `run.mk` 行 263-266 的现有注释。
5. 在 `mk/components/run.mk` 的 `help` recipe 追加一行，匹配 bucket 的 printf 格式。
6. 如果 bucket 获得新值，更新本文档和 `AGENTS.md`。

### 5.1 生成式测试目录与派发表（2026-11-14 闭环）

> 规范 [2026-11-14-test-framework-design.md](../superpowers/specs/2026-11-14-test-framework-design.md)，
> 计划 [2026-11-14-test-framework-redesign.md](../superpowers/plans/2026-11-14-test-framework-redesign.md)。
> 治理细则见 [docs/testing/testing-policy.md](../testing/testing-policy.md) §8。

**测试清单不再手写**。每个测试源在 `hosttests/cases/test_<name>.c` 里带 `@os01-test`
元数据块；`tools/tests/scan_tests.py` 扫描后生成并入库：

| 产物 | 用途 |
|------|------|
| `config/test-catalog.json` | 全量测试条目（`host_test` / `suite` / legacy 桥 / `ignored`） |
| `mk/generated/test-dispatch.mk` | SUITE ID 表 + 每个 SUITE 的 flavor 与镜像映射 |
| `mk/generated/test-harness-modules.mk` | harness 模块清单 |
| `mk/generated/test-link-rules.mk` | 宿主测试链接规则（按元数据驱动） |
| `mk/generated/test-registry.mk` | 宿主测试注册表 |

新增宿主测试的流程：`hosttests/mk/<subsys>.mk` 落链接规则 → `make regen-test-catalog`
→ 提交生成物。手写 `register_host_test` / `TEST_BINS` **已退役**。

```bash
make regen-test-catalog    # 重新生成并写回
make check-test-catalog    # 漂移门禁：生成物与源不一致即失败
```

CI 的 `harness` job 独立运行 `make check-test-catalog`；日程漂移检查由
`make test-harness` 的 `check-ci-schedule` 先决目标执行。

栈帧框架单测使用固定样本，不读取工作区里已有的内核 ELF。实际栈帧上限由
x86_64 的 `make test-static` 和 AArch64 的
`make PROFILE=aarch64-clang test-aarch64-audit` 构建并审计对应产物。

**六处并行维护已收敛为单一源**：SUITE 白名单原先同时维护在 `run.mk` 的 `case` 语句、
`TEST_QEMU_FLAVOR` 表、`TEST_QEMU_IMG` 表、`catalog.mk` TSV、`ci.yml`、
`run_test.py` 的 if-elif 链（另有一行手工 append 的 `TEST_HARNESS_MODULES`），
现在全部由 `mk/generated/test-dispatch.mk` 供给；`run.mk` 改为对
`TEST_QEMU_SUITE_IDS` 做 **parse-time 校验**，生成片段缺失即硬错误。

**表驱动的目标生成**：聚焦宿主用例分组由 `host_case_group` 宏生成
（`mk/components/run.mk:803`），五个 x86 `boot-*` QEMU 故障矩阵由 `boot_matrix_suite`
宏生成（`:1094`）；RED / canary 管线脚本化提取，独立静态审计表驱动。

### 6. 变量参考

| 变量 | 定义于 | 用于 |
| --- | --- | --- |
| `RUN_QEMU_BASE` | `mk/components/run.mk` | `run`、`run-kvm`、`run-virtio`、`debug` |
| `RUN_QEMU_DISK` | `mk/components/run.mk` | NIC 选择之后的公共 disk、RNG、memory、display 和 serial 参数 |
| `RUN_QEMU_FLAGS_run-kvm` / `RUN_QEMU_FLAGS_debug` | `mk/components/run.mk` | 插入共享 network/disk 参数之前的旗标 |
| `TEST_QEMU_FLAVOR_<suite>` / `TEST_QEMU_IMG_<suite>` | `mk/components/run.mk` | `test-qemu` per-SUITE 查找（Make 变量，不是 shell 变量） |
| `X86_CONTRACT_MODES` / `AARCH64_CONTRACT_MODES` | `mk/components/run.mk` | `test-contract` |
| `_test-contract-prep-x86` / `_test-contract-prep-aarch64` (private) | `mk/components/run.mk` | per-PROFILE pre-build 步骤（拆开使 `+env` 位于 recipe 行位置） |
| `TEST_AARCH64_EXTRA_<mode>` | `mk/components/run.mk` | `test-aarch64` per-MODE DTB 旗标 |
| `_test-aarch64-prep-<mode>` / `_test-aarch64-run-<mode>` (private) | `mk/components/run.mk` | per-MODE pre-build + python 调用（拆开使 `$(MAKE)` 和 python 位于独立 recipe 行） |

## 附录 A · 命令/Flag 速查

### 常用命令

```bash
# 构建
make                                # 默认 profile 完整构建
make kernel.bin                     # 默认 profile 内核
make lib user                       # 默认 profile 库与用户程序
make image                          # 当前 profile 磁盘镜像
make clean                          # 清理

# 运行与调试
make run                            # 默认 profile QEMU
make run-kvm                        # KVM 加速
make run-virtio                     # virtio-net
make debug                          # QEMU 暂停 + GDB :1234
make PROFILE=aarch64-clang run-aarch64-uefi  # aarch64 UEFI 启动

# 测试
make test-host                      # 宿主测试
make test-qemu SUITE=phase-0        # QEMU E2E（普通镜像）
make test-qemu SUITE=systest        # syscall E2E
make test-qemu SUITE=inittab-phase  # inittab 阶段派发
make test-qemu SUITE=network        # 网络回归
make test-qemu SUITE=resolution     # 分辨率切换 surface/session 验收
make test-qemu SUITE=monitor        # 终端 monitor 1/2/8 核矩阵 + 终端生命周期
make test-qemu SUITE=terminal-present  # GFX_PRESENT_RECT 像素正确性 + 30s 稳态字节/ns 性能门控（spec §8）；baseline 通过 TERMINAL_PRESENT_BASELINE_LOG 覆盖
make test-qemu SUITE=gui             # GUI-1 LVGL 平台后端 + GUI-2 输入与焦点验收（SMP 1/2 矩阵）
make PROFILE=x86_64-clang test-gui-host CASE=input-decode  # GUI 宿主聚焦 CASE（host_case_group 映射）
make PROFILE=x86_64-clang test-libc-host LIBC_CASE=api     # libc 补全宿主聚焦 CASE（LIBC_HOST_CASES / LIBC_HOST_DELIVERED_CASES）
make PROFILE=x86_64-clang test-libc-api-audit              # libc 归档导出审计（用户 libc + libk）
make PROFILE=x86_64-clang test-libm-asm                   # libm 归档反汇编审计（禁止浮点算术指令）
make FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution  # 隔离故障子集
make CAPACITY_TEST=1 test-qemu SUITE=arch10-capacity # ARCH-10 隔离容量与工程完整性验收
make test-static                    # 16 项静态审计
make test-kernel-selftest           # 内核 selftest
make test-aarch64 MODE=smp          # aarch64 PSCI/SMP
make PROFILE=aarch64-clang test-aarch64 MODE=sched  # EL1 调度器验收（正例矩阵 + 四个故障注入变体）
make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap  # AP 协作调度验收（7 case 生产矩阵）
make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap AP_SCHED_CASE=fatal-owner  # fatal 单 owner 故障 boot
make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick  # 每核 tick 记账验收（1/2/4/8 核各两次）
make PROFILE=aarch64-clang test-aarch64 MODE=exception-trace  # AArch64 异常回溯与帧读取修复验收（read/bad-fp/task，可设 EXCEPTION_TRACE_CASE=<case>）
make test-contract                  # CI 契约检查
make test-harness                   # Python unittest 框架回归（TEST_HARNESS_MODULES 列表；不启动 QEMU）
make test-doctor                    # 环境体检：比对本地 clang/ld.lld/qemu 与 ci/Dockerfile 钉值
make ci-local SUITE=phase-0         # 在本地 ghcr.io/aagu/os01-ci:latest 容器中运行套件（不自动 pull）

# 检查
make PROFILE=x86_64-clang print-run-paths  # 打印 firmware= / image= 路径
make validate                       # 内核 ELF / EFI 验证

# aarch64
make PROFILE=aarch64-clang aarch64-uefi         # aarch64 镜像 + 固件
make PROFILE=aarch64-clang aarch64-uefi-kernel  # aarch64 内核 ELF
```

### 常用 Flag/环境变量

| Flag/变量 | 用途 |
| --- | --- |
| `PROFILE=<name>` | 选择 profile（默认 `x86_64-clang`） |
| `SUITE=<name>` | `test-qemu` 的 variant（`phase-0`、`systest`、`inittab-phase`、`network`、`gfx`、`resolution`、`driver-model`、`monitor`、`terminal-present`、`gui`、`doom`） |
| `USB_BOOT_CASE=<name>` | `test-qemu-usb-boot` 的 case（`readonly` / `dual` / `ahci` / `persistence` / `superspeed` / `hub` / `firmware`，默认 `readonly`；`firmware` 是过期的预期失败断言，仅可显式 opt-in） |
| `MODE=<name>` | `test-aarch64` 的 variant（`smp`、`no-ack`、`gic-spi`、`sync-fault`、`serror`、`exception-trace`、`m3-probe`、`sched`、`sched-ap`、`sched-wake`、`sched-tick`、`m1-*`） |
| `AP_SCHED_CASE=<name>` | `MODE=sched-ap` 的用例选择（`positive`、`fatal-owner`，默认 `positive`） |
| `SCHED_CASE=<name>` | `MODE=sched` 的用例选择（`positive`、`sp-top`、`sp-low`、`pc-low`、`pc-data`、`all`，默认 `all`） |
| `OS01_SYSTEST=1` | 启用 systest variant（影响编译旗标） |
| `OS01_NETTEST=1` | 启用 nettest variant |
| `INITTAB_FILE=<path>` | 自定义 inittab 模板 |
| `DEBUG_CHANNELS=<list>` | 启用调试通道（`sched,vfs,mm` 等） |
| `USER_DEBUG=1` | 启用用户态调试日志（`user/common/user_log.h` 的 `log_debug` 门面）：terminal 的每秒 `[term-present]` / `[term-probe]` 记录才编译进去；默认关闭，串口不再持续刷这两条记录 |
| `TERM_PRESENT_TEST=1` | terminal-present 套件的内部旗：等价于 `USER_DEBUG=1` + 隔离的 `terminal-present` 镜像变体（由 `make test-qemu SUITE=terminal-present` 自动传入） |
| `KERNEL_SELFTEST=1` | 启用内核内自测 |
| `LOG_TARGET=<target>` | 日志输出目标（`serial`、`fb`、`both`） |
| `NDEBUG=1` | 编译期消除 `log_debug` |
| `OVMF_FIRMWARE_SOURCE=<url\|abs path>` | 自定义 OVMF 固件源 |
| `UEFI_RUNTIME_SOURCE=<path>` | aarch64 UEFI 运行时源 |
| `CLANG=<name>`、 `LLVM_NM=<path>`、 `UEFI_CLANG=<path>` | 工具链覆盖（详见 [`toolchain.md`](toolchain.md)） |
| `SMP=<n>` | QEMU SMP CPU 数（默认 2） |

## 来源映射

本文档由以下三份文档合并而成（2026-10-01）：

- `docs/build.md` → 第 1 章（编译过程）
- `docs/build-run-debug.md` → 第 2 章（构建、运行与调试操作）
- `docs/build-system-harness.md` → 第 3 章（构建系统 Harness）+ 第 2 章 §2-3（部分）

`docs/build/toolchain.md` 保持独立，覆盖 x86_64 toolchain override 契约。

## 用户态程序与 app.mk

用户程序由 `user/apps.mk` 统一发现，`make user` 编译并发布 ELF，
`make disk.img` 将程序安装为 `/bin/<程序名>`。新增程序无需修改顶层
Makefile 或 `config/rootfs.mk`。删除源码或 `app.mk` 后，下一次镜像构建
也会移除对应的安装条目。

简单程序直接放在 `user/<程序名>.c`。多文件程序放在
`user/<程序名>/`，添加 `app.mk`，例如：

```make
# user/example/app.mk；源文件路径相对于此目录
APP_SOURCES := main.c parser.c entry.S
APP_CFLAGS := -DENABLE_FEATURE
APP_LDLIBS := -lgfx
```

每个子目录对应一个同名 ELF；只发现一级子目录中的 `app.mk`，
不会把目录中的辅助 `.c` 当作独立程序。`APP_SOURCES` 必须非空，
支持 `.c` 和 `.S`，也可列出目录内更深层的源文件。顶层单文件与
子目录程序重名时立即报错；`busybox` 名称保留给 BusyBox 构建适配器。

可选变量：

| 变量 | 用途 |
|---|---|
| `APP_CFLAGS` | 追加到本程序目录内源文件的编译选项 |
| `APP_LDFLAGS` | 追加到本程序的链接选项 |
| `APP_LDLIBS` | 额外静态库，放在应用对象之后、默认 `-lc` 之前 |
| `APP_SHARED_SOURCES` | 相对于 `user/` 的共享源文件，以全局选项编译一次 |
| `APP_EXTRA_OBJS` | 资源生成规则提供的额外对象 |

共享辅助代码放在 `user/common/`；此目录不放 `app.mk`。
启动对象 `crt0.o` 由 libc 安装进 sysroot，信号返回 trampoline 是 libc.a
成员，链接脚本在 `libc/arch/<arch>/user.ld`；均由公共规则处理。
对象与 `.d` 依赖文件保留在 profile/variant 的构建目录内，修改头文件
或 `app.mk` 会触发相应重建。

`app.mk` 同时被顶层发现逻辑与用户组件读取。自定义资源规则及
`APP_EXTRA_OBJS` 应放在 `ifeq ($(USER_BUILD_CONTEXT),1)` 条件内，
使用 `$(OBJ_DIR)` 存放生成文件；可参考 `user/terminal/app.mk`。
资源生成规则建议写在变量声明之后；默认目标固定为 `all`。

`test_resolution.c` 仍仅在 `FB_RESOLUTION_TEST=1` 时发现和安装。
`sh.c` 会编译及发布，但 `/bin/sh` 保留为 BusyBox 链接。其他同名
BusyBox applet 的链接会让位于发现的原生程序。

构建回归检查包含在 `make test-host` 中，也可单独运行：

```bash
python3 tools/tests/test_user_build.py
```

### aarch64 每核调度 tick 记账验收

`make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick` 构建独立
`sched-tick` kernel/image/firmware，运行 1/2/4/8 核各两次。选择器
`AARCH64_SCHED_TICK_TEST=1` 要求 `KERNEL_SELFTEST=1`，由 mode 自动设置，
并拒绝与旧 sched/AP sched/M1/no-ack/sync-fault/canary/weak/systest 的组合；
x86_64 profile 直接拒绝该 aarch64 选择器。

镜像内运行产品 selftest：每个已录用 CPU 发布七个冻结 case（tick-ready、
tick-idle、tick-busy、tick-yield、tick-deadline、tick-global、tick-remote）
各一行，整批证据在对应的 `selftest_record_late()` 记录之前发表，
`[sched-tick] END failed=0` 排在 selftest END 之后。idle 窗口在目标自己的
idle 上下文测量，busy/yield/deadline/global 窗口在目标核的 pinned worker
上测量，remote 窗口让每个 CPU 都成为一次真实 SGI4 的目标。

严格 runner（`qemutests/aarch64_sched_tick.py`）按冻结字段与跨字段关系解析
（`idle==samples`、`busy==samples`、`runtime==samples`、`vruntime==samples`、
`local_samples==busy+idle`、AP 的 `global_ticks==0`、BSP 的
`jiffies_delta==global_ticks`），要求每个 case 每 CPU 恰一行、
`accepted_mask==(1<<N)-1`、证据行早于对应 BEGIN、END 恰一次；产物按内容审计
（AArch64 ELF64、专用符号、同一 variant 目录、专用符号不得泄漏进普通产品
ELF）。每次 QEMU 有 120 s 绝对期限（tick 不延长），完成后 1 s 排空窗口。

### aarch64 跨核唤醒验收（P2）

`make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake` 构建独立
`sched-wake` kernel/image，运行1/2/4/8核各两次。选择器
`AARCH64_AP_SCHED_TEST=wake`要求KERNEL_SELFTEST=1，由mode自动设置，
并拒绝与旧sched/M1/sync-fault/no-ack/canary/systest组合。

wake包络继承P1全部七项，加remote-wake、wake-switch、idle-race、sgi、
work-sleep、scan-fallback六项。每AP分别给出100轮显式唤醒、100轮切出窗口
唤醒、三种idle时序各100轮、1次真实ready交接及200次work唤醒、100轮scan
fallback。UP只对跨核项给出全零NA行，仍运行本地scan和硬件self-SGI。
严格runner验证phase、每CPU完整字段、恰一次START/END及全在线mask；准备阶段
审计真实ELF的wake hook隔离和`dsb sy; wfi; ret`指令。归档含镜像、固件、
ELF摘要、revision/dirty、命令、日志与退出状态。WFI计数证明经过等待路径，
不表示QEMU物理省电；SGI不抢占不合作的线程。

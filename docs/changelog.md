# 已完成工作汇总（Changelog）

> OS01 各阶段已完成工作的按时间汇总。最新在前（截至 2026-10-07）。
> 本表为历史完成记录，规划项见 `docs/roadmap.md`。

---

## 2026-10-07

- cleanup(aarch64): **M2/M3 杂项 cosmetic 遗留 7 项全部闭环**（分支 `worktree-m23-cosmetic-cleanup`，7 commits）：
  - **死枚举**：删 `ap_work.h` 的 `WORK_PT_STRESS`/`WORK_PT_ALLOC`（无任何用户；`WORK_PT_MAP` 由 kernel selftest 的 `ap_work_ext_run` override 消费，保留），同步修正 `ap_work.c` 与 selftest 头注释里过时的 WORK_PT_STRESS 叙述
  - **head.S 过时注释**：`head.S` TPIDR_EL1 注释里 "152 (0x98)" 改为陈述真实约束（16-bit mov immediate），不再硬编码可漂移的尺寸数字（实际 `PERCPU_DATA_SIZE` 自 M3 Task 12 起为 144）
  - **mock percpu 陷阱**：`hosttests/mock/test_platform.h` 删 legacy `tlb_wanted/tlb_ack`（生产端 M3 Task 12 已移除）、`apic_id` 改名 `arch_processor_id` 对齐生产命名，注释明确「compile-only stub，布局契约以 `kernel/include/percpu/percpu.h` 为准（`test_percpu_layout.c` 运行时钉）」
  - **serial_printk %d 字面输出**：确认根因（aarch64 `printk_stub` 用 `kputs(fmt)` 打字面格式串）已在 `7ebc9d0a`（share color_printk + serial_printk via kernel-core）移除；本次以 `test-aarch64 MODE=smp` 9/9 boot 日志 0 处字面格式串 + `[selftest] 6 total: 6 passed` 闭环，无代码改动
  - **errno 归一化（TDD）**：`vmm_backend.c` 新增 `pt_err_to_linux()`，替换原 Task 19 两点式 EEXIST/ENOENT 归一化，覆盖全部 `arch_vmm_*` 出口——此前 `AARCH64_PT_EINVAL=-1`（撞 Linux `-EPERM`）、`ENOMEM=-4`（撞 `-EINTR`）、`EPERM=-6`、`EAGAIN=-7`、`ECONFLICT=-5` 裸穿透（可达路径：split 已发布 root 的 `-EPERM` 与 alloc-fail 的 `-ENOMEM`）。`AARCH64_PT_EPROT_NONE` 三态 sentinel 保留；未知码原样穿透 fail-loud。RED→GREEN：`test_aarch64_backend_4k.c` 新增 errno-contract 4 断言（先 3 失败后全绿），`shootdown_probe.c`/`vmm_backend.h` 注释同步
  - **read_l2_desc test-only 守卫**：声明与定义均加 `#ifdef OS01_SELFTEST`（`kernel/random/random.c` test-hook 同款模式）——生产误用直接编译错；hosttests 的 `AARCH64_PT_SW_HOST_CFLAGS` 加 `-DOS01_SELFTEST=1` 保持 host harness 可用；生产 aarch64 内核 `nm` 0 引用验证
  - **§5.2b TLBI TODO**：`walk_to_l2` 的 `TODO(Task 18)` 过时任务编号改为条件式描述（首个「已发布 root + create」caller 出现时落地 per-level TLBI 分支；M4 phantom-ENOENT 钉死前不实现）
  - **验证**：`make clean` 后 `test-host` 108 套件 0 失败（避开增量假绿）；生产 aarch64 构建 rc=0；`make PROFILE=aarch64-clang test-aarch64 MODE=smp` 9/9 PASS（cpus 1/2/4 × 3）且 selftest `6 total: 6 passed`

## 2026-10-06

- refactor(driver-model): **ARCH-9 驱动模型 / 总线抽象** —— commits `14119cf7..8b29b25`（分支 `arch9-driver-model` 已合并并删除，21 commit + 1 followup fix；84 文件，+16,695/-1,875）：
  - **A 阶段（块设备 ops + GPT 包装）**：`struct block_device_ops {read, write, flush}` + `struct block_device_desc {name, sector_count, sector_size, ops, private_data, parent, kind}`；移除 `port_num` 直访问，驱动注册自己的 ops；GPT 经父入口 dispatch；fs/boot 只从 BLOCK_DISK 中选第一盘
  - **B 阶段（device core + PCI core + AHCI + boot coordinator）**：typed `pci_driver`（id_table 匹配 + probe/remove） + `struct pci_device`（dev, domain/bdf, vendor/device/class, bars[6] {kind, address, valid, index}, driver, driver_data） + `struct pci_backend {roots, root_count, read32/write32/route_gsi}` + x86 backend 隔离在 `kernel/arch/x86_64/bus/pci.c`；AHCI 私有 `ahci_instance` + 安全撤销/DMA 隔离 + boot coordinator（`device_core_init` → `net_device_init` → `pci_enumerate` → `pci_bind_all` → `net_lwip_start`）
  - **C 阶段（net_device 抽象 + lwIP + e1000 + virtio-net + ENETDOWN 守卫）**：`struct net_device {name, mac, mtu, link_up, parent, ops, priv, adapter}` + ops `{xmit, poll_rx, get_link, stop}`；`net_service_ready()` / `net_default_ipv4()` 公共 readiness 查询；e1000 与 virtio-net 每实例驱动，3 种 IRQ 模式（INTX/POLL/MSIX）；socket `do_socket` 在 NIC 未 ONLINE 时返回 `-ENETDOWN`（spec §Review Focus #4）；过渡期 `os01_netif` 在 Task 9/10 一起移除，`kernel/net/net.c` 收敛为 39 行两向薄 shim
  - **D 阶段（QEMU 多卡矩阵 + 故障 fixture + per-BDF 观察）**：`make test-qemu SUITE=driver-model` 跑 16 case × 2 SMP；`ARCH9_FAULT=none|observe|bad-nic-bar|adapter-fail|ahci-empty|irq-conflict` 通过 root 传 kernel flag；build contract 路径固定 `build/x86_64-clang/{kernel,artifacts,image}/driver-model-<fault>/`；每个 BDF 注册观察计数器（probe/bar/adapter）让 unsupported/modern-only 能精确断言未匹配设备 zero probe；boundary audit + test-syscall alias 恢复（用户 AGENTS 唯一受允许例外）；`kernel/include/ipc/mbox.h` 从 `kernel/net/sys_arch.c` 抽出满足 source/header symmetry
  - **最终 wave fix**：AHCI id-table 严式 `01/06/01` 单一匹配（移除 `0xFFFF00` prog_if 掩码，避免非 AHCI SATA 控制器被程序化为 AHCI）；新增 `arch9-irq-mode: ... nic=ethN mode=POLL|INTX` log，使 irq-conflict 矩阵 case 真正验证第二卡 POLL 回落而非仅凭 `iface=` 标记放行
  - **全量测试验证**：`test-arch9-host` 10 套件绿、`test_driver_model_matrix.py` 21/21（10 RED 注入 + 11 含 IRQ mode/per-card evidence/SMP 过滤/边界审计/IRQ mode 解析）、`test_arch9_build_contract.py` 12/12、`make test-qemu SUITE=driver-model` 31/31（16 case × 2 SMP，含 net-block-smp@SMP=2 only、`irq-conflict` 第二卡 POLL marker）、e1000 + virtio 单卡网络回归、`make PROFILE=x86_64-clang image` 全链路；`test-driver_model_boundary_audit.py` 7 套件验证 `test-syscall` alias gate 与 5 条边界规则
  - **已知遗留**：`make OS01_SYSTEST=1 test-systest` 暴露 pre-existing 回归（`config/inittab.systest` 不带起网卡，Task 9/10 的 `net_service_ready()` 正确返回 -ENETDOWN；不在本分支范围，单独 follow-up）；多项 minor cleanup（test-only externs、`copy_to_user_ft_res` utility split、文档准确性）通过 task 评审 parked 进 ledger
  - **测试质量复盘**：完成 12 task × 1 task-reviewer + 4 fix rounds（Task 11 的 silent-pass 隐藏 bug 已修复：`nic1=None` 与 `ok1 >= 3` 互斥 + SMP=1 误跑 + `unsupported` 观察断言反向）+ 最终 wave（AHCI prog_if mask + irq-conflict POLL marker）；交付 12 commits + 7 fix commits
  - **设计/计划文档**：`docs/superpowers/specs/2026-10-05-arch9-driver-model-design.md`（已确认设计）+ `docs/superpowers/plans/2026-10-05-arch9-driver-model.md`（实现计划，12 任务 4 阶段）；执行评审记录见 `.superpowers/sdd/2026-10-05-arch9-driver-model/progress.md`（gitignored）

## 2026-10-05

- refactor(syscall): **ARCH-2 Linux ABI 兼容层独立**：
  - **保持 OS01 原生 ABI 为唯一标准**：Linux x86_64 系统调用仅作为兼容层映射转调原生分发器，杜绝兼容逻辑反向主导系统调用号与语义
  - **兼容模块子目录化与防溢出保护**：拆分至独立子目录 `kernel/syscall/compat/{dispatch.c, table_x86_64.c, proc.c, fs.c}`，表项类型升级为 `int16_t`（`compat_syscall_nr_t`），附带编译期 `_Static_assert` 杜绝溢出隐患
  - **未映射/不支持严格哨兵拦截**：未映射、越界或显式不支持（`COMPAT_UNSUPPORTED`）编号严格返回 `-ENOSYS` (-38)，不再透传至原生分发器
  - **领域语义适配**：在 `compat/proc.c` 中承接 `rt_sigaction`（Linux 13，4 参数 `sigsetsize == 8` 校验）与 `wait4`（Linux 61，参数转发至 `SYS_waitpid`）
  - **ELF 探测与 Linux ABI 激活解耦**：`elf_detect_abi()` 通过 `PT_NOTE` 段（`NT_GNU_ABI_TAG`，OS=Linux）检测外部静态 Linux 二进制并赋予 `PF_LINUX_ABI`；**严格不碰 `PT_INTERP`**，彻底解耦 Linux ABI 激活与未来动态链接机制
  - **全量测试验证**：`test-static` 静态审计（含递归兼容目录扫描）、`test-host` 57/57 单元测试套件、`systest` 341/341 项端到端测试（新增 `54_linux_abi_compat`）、QEMU `phase-0`、`inittab-phase` 与 `network` 套件全量绿灯通过

## 2026-10-04

- refactor(syscall): **ARCH-1 syscall 层脱离 arch** —— commits `2765103..74e3cfeb`：74 个已实现处理器迁至 `kernel/syscall/`，x86_64 入口保留寄存器解码、Linux ABI 预翻译和用户态信号返回；`SYS_getpeername`（62）保留 trace 名称但无 handler、返回 `-EINVAL`。新增分发表 hosttest 与静态边界审计；host 56/56、syscall systest 340/340、网络 QEMU、普通启动通过。独立内核自测在 `test_tty_vintr` 有通过和超时两种结果，本地 master 合并验收亦复现超时；根因未确认并列入 roadmap Parked。
- refactor(arch): **ARCH-3 头文件定义全局 + 彻底移除 `-z muldefs`**：
  - 彻底移除 `kernel/arch/x86_64/make.config` 与 `kernel/arch/aarch64/make.config` 中的 `-z muldefs` / `-Wl,-z,muldefs` 链接参数，消除链接期对符号冲突的静默吞噬
  - 全局对象定义迁入 `.c`：`kernel/include/sched/task.h` 中的 `init_task_union`、`init_task[]`、`init_mm`、`init_thread` 迁入 `kernel/sched/task.c`；`kernel/include/intr/interrupt.h` 中的 `irq_table` 迁入 `kernel/intr/irq.c`
  - x86_64 TSS 硬件结构解耦：新建 `kernel/include/arch/x86_64/tss.h`，`init_tss[]` 实体定义迁入 `kernel/arch/x86_64/cpu/task_arch.c`，通用调度器头文件 `task.h` 不再包含体系结构特定的 TSS 结构体与硬编码 IST 字段
  - 清理暴露的重复符号：lwIP `sys_msleep` 宏覆盖与 `arch/x86_64/string.h` 中的 `memset` `static inline` 修饰
  - 静态测试门禁闭环：`qemutests/runtime_audit.py` 增加禁止 `-z muldefs` 参数校验；新增 `qemutests/header_object_audit.py` 静态审计 149 个公开头文件，保障零强/弱对象定义
  - 完善后台自测执行：`test-kernel-selftest` QEMU 增加 `< /dev/null` 重定向避免非交互终端挂起；放宽 `test_mutex.c` SMP 自测自旋上限
- feat(terminal): **terminal 迁移至 libgfx + 刷新与 I/O 优化** —— commits `52a77005` / `920b48a6` / `c9105090`：
  - terminal 渲染路径切至 `libgfx.a` 静态库，支持离屏双缓冲与字形绘制加速（`gfx_draw_glyph`）
  - 优化刷新算法：利用像素级区域滚动替代整屏重绘，批量合并串口 I/O 读写
  - 增加 eager PTY 排空与 8 像素宽字体快速展开路径，显著降低高负载输出卡顿
- feat(mm): **用户空间信封扩展至 512 MiB + 匿名 mmap 缓冲** —— commits `c53d7cde` / `8ad4d1a8`：
  - `USER_PAGE_SIZE` 重命名为 `USER_ENVELOPE_SIZE`，上限由 16 MiB 提升至 512 MiB（`0x400000` 到 `0x20400000`），为大分辨率像素缓冲与后续应用预留充足空间
  - `libgfx` 像素缓冲分配由堆（`malloc`/`brk`）迁移为匿名 `mmap`（`MAP_ANON`），彻底解耦图形缓冲与 brk 用户堆
- docs(roadmap): **规划 P2 架构治理任务 ARCH-1..10 并清理过时项** —— commit `dd05877b` 及后续收尾：
  - 确认 OS01 自有 syscall ABI 为唯一标准，Linux ABI 仅作兼容层
  - 规划 ARCH-1（syscall 脱离 arch）、ARCH-2（Linux ABI 兼容层独立）、ARCH-3（消除 muldefs 与头定义全局变量）、ARCH-4（拆分 sched/task.c）等 10 项治理任务
  - 清理 roadmap 中已在 master 闭环的规划项（用户堆与 ELF 映射隔离、2D 图形 API、PS/2 鼠标驱动、aarch64 M0/M1、增量重编等）

## 2026-10-03

- feat(mm): **aarch64 M1 运行期直映严式页表树合入** —— merge `e531167b`（commits `10826fef..f1df9444`）：
  - 共同 `arch/boot_memory.h` 启动直映契约：PMM 之后、AP 启动前单点调用 `arch_boot_direct_map_init()`
  - x86_64 适配器保留现有 2 MiB 启动直映，传播中间页表分配失败；`ZONE_UNMAPPED_INDEX` 覆盖截止语义保持
  - aarch64 从低窗口 arena 建立独立 TTBR1，覆盖高 RAM 与 holes，收紧设备与保留 block；严格限制非 RAM 映射
  - BSP/AP root 与 probe 全面验证，覆盖 16 组 RAM/CPU/镜像矩阵（256 MiB..4 GiB，1..4 核），通过稀疏、容量耗尽、AP 坏 root 注入测试
- docs(spec,plan): **aarch64 M2 Slab + M3 内核 VMM 详细设计闭环** —— commits `93f8ae7f..9edc1cab`：
  - spec v8 + plan v4：明确 frame 级记账、锁契约（BBM、inactive root 拆分）、SGI/SMP shootdown 协议与初始化顺序契约，就绪待实装

## 2026-10-02

- feat(mm,elf): **用户堆与 ELF 映射隔离全面闭环** —— merge `277b6315`（8-task plan，16 commits `fb6635a3..085f50e1`）：
  - **Task 1**（`5cc047e6`）：纯 ELF64 加载布局校验器（`elf_layout_validate`）
  - **Task 2**（`01d95d6c`）：4 KiB 分页 ELF 加载器与单回滚所有者（`elf.c` 重写，杜绝大页溢出）
  - **Task 3**（`fa3fa45d`）：暂存 spawn/exec 镜像生命周期 + 零长度初始 `VM_HEAP` VMA + `mm_init_user_heap`
  - **Task 4**（`76076b9d`）：`mm_set_brk` 拥有已提交 4 KiB 堆页，`SYS_brk` 委托处理；扩展按页分配，收缩时释放物理页并解除映射；缺页保护防止越界预写
  - **Task 5**（`a6b9507d`）：`mm_user_range_protected` 保护 ELF envelope、堆保留区、guard 保护页与用户栈免受 `mmap`/`munmap`/`mprotect` 侵入
  - **Task 6**（`95129e93`）：暂存两阶段 `fork_mm_copy` + 回滚修复（VMIO huge、fork-of-fork COW）
  - **Task 7**（`81dc54a0`）：`prepare_user_write_range(_locked)` 用户写前 COW 私有化准备与 11+ 审计调用点接入
  - **Task 8**（`284d846a`）：全屏 1440×900 + `brk(0)` headroom + Tetris smoke + E2E 验证；host 43/43 suites，systest 334/334 全绿
- feat(aarch64): **aarch64 M0 启动直映契约合入** —— merge `2523a311`（commits `e829c377..8a9fa7e6`）：
  - `head.S` 在 MMU 打开前建立 0..2 GiB boot map，内核 block 保持 EL1 可执行，其余 RAM PXN/UXN；普通镜像与自测镜像契约完全对齐，消除 C 代码补图差异

## 2026-10-01

- feat(aarch64): **aarch64 EL1 sync 致命异常诊断** —— merge `d6fbb22d`（commits `8734f1e5..cba5f578`）：
  - 实现 ESR_EL1 / FAR_EL1 / ELR_EL1 致命诊断与寄存器解析
  - 建立 sync-fault 独立探针镜像构建机制，配套 QEMU 自动化测试 harness
- docs: **文档目录树状结构重构** —— commit `931393b9`：
  - 将 `docs/` 下 50+ 扁平文件重组为与 `kernel/<subsys>/` 对称的树状子目录
  - 历史 closure 报告与调试日志归档至 `docs/archive/`；合并构建相关手册为 `docs/build/build.md`

## 2026-09-30

- feat(gfx): **P3 2D 图形 API 闭环（libgfx + /dev/gfx0）** —— merge `8a0f4e7b`（commits `f81f092f..614e181c`，6-task plan）：
  - `libgfx.a` 用户态静态库：点/线/矩形/填充矩形/位图 blit 与局部视口裁剪
  - `/dev/gfx0` 受限 present 设备：per-file ioctl 视图生命周期、受控矩形呈现、坐标校验
  - Tetris 游戏迁移：将直接 `/dev/fb` mmap 替换为受限视图呈現，游戏逻辑零改动平滑迁移
  - QEMU ring-3 E2E 测试套件（`test-qemu SUITE=gfx`）与 host 单元测试全覆盖

## 2026-09-28

- refactor(kernel_main): **x86 引导阶段分步拆分** —— merge `9f911824`（commits `f5004588..8a43cea3`）：
  - 拆分 BSP per-cpu 初始化、启动文件系统挂载、x86 设备节点注册、控制台 TTY 引导连接与 AP 引导辅助
  - 提升引导模块内聚性，为 x86/aarch64 统一单 `kernel_main` 入口铺平路径

## 2026-09-27

- feat(driver): **PS/2 鼠标驱动落地（P3 首项闭环）** —— commits `116e57ec..0a68ae97`（10-task plan）：
  - `i8042` 控制器共用层：互斥锁、键盘/鼠标来源分流、command byte 事务机制与 pump drain 上限保护
  - `mouse_event_t` 8 字节事件 ABI（`kernel/include/uapi/mouse.h`）
  - `/dev/mouse` 设备节点：event ring 环形缓冲、非阻塞/阻塞读取、`poll` 就绪通知
  - 500 ms 有界硬件探测与 F5 safe cleanup，QEMU 与 host 单元测试全通过

## 2026-09-26

- refactor(aarch64,build): **最后两个 Parked 项闭环（idle 正名 + sysroot 头文件级增量重编）** —— worktree `parked-two-closures`：
  - **idle 正名**：`kernel/arch/aarch64/cpu/idle_resume_stub.c` → `cpu/idle.c`。`subsys_stub.c` 此前已由 AAGU-3 闭环删除（`serial_printk`/`strcmp`/`num_cpus` 均有真实实现）；wfi 永循环对无调度器的 aarch64 是正确的 idle 行为而非占位符，per-CPU need_resched 再入归 §P2 上下文切换统一
  - **增量重编**：genid→`-B` 全量重编移除。kernel 编译/链接（`-isystem`、`-include stdint.h`、`ALL_LDFLAGS -L`、`KERNEL_RAW_LIBDIR`）全部切稳定 `$(SYSROOT)` symlink；publisher `cp -p` 保 mtime + publish 等 lease（防中途换 header）；`mk/components/kernel.mk` 删 `.sysroot-generation` stamp
  - **顺带修复三个预先存在的缺陷**：① `-MMD` 跳过 system header → kernel `.d` 从未记录 `-isystem` 引入的 libc 头，改 `-MD`（`.cflags` fingerprint 加 `deps=system-headers` 前缀使现存 build 目录一次性自动重建）；② runtime grouped rule receipt 无条件重写 + archive 永不更新 → 每次 kernel 构建都 relink（stage1+kallsyms 两遍），receipt 写入改 cmp 内容门控；③ aarch64 下 `KERNEL_RAW_LIBDIR` 空 `-L/usr/lib`（宿主路径）风险随 symlink 切换消除
  - 回归：`test-contract` 新 mode `sysroot-headers`（单头编辑只重编依赖者 / `.cflags` stamp 不动 / 还原一致 / 零污染）。验证：x86 contract 9 modes `>>> ALL TESTS PASSED <<<`；aarch64 构建 + SMP 9/9 PASS

- fix(build): **CFLAGS-only build cache 失效闭环（Parked 项）** —— `kernel/Makefile` 新增 `$(BUILD_DIR)/.cflags` fingerprint stamp（编译 pattern rule 前置依赖；`ALL_CFLAGS`+`ALL_C_ONLY_FLAGS` 内容变化才改写 → 全量重编；recipe-time 展开规避 `make -n` 破坏性，本地 `FLAGS_STAMP_FORCE` phony 替代 root-only 的 `FORCE`）。新增 `KERNEL_EXTRA_CFLAGS` 旋钮并入 `OS01_SUBMAKE_ALLOWED`。`test-contract` 新增 `flags-cache` mode（TDD：先 RED「stamp missing」后 GREEN，4 门禁：stamp 存在 / identical 重建零重编 / flag 变化 stamp+重编 / flag 还原重编；排除 `kernel/runtime/`——runtime builtins 用独立 `RUNTIME_CFLAGS_kernel`，不参与 kernel CFLAGS）。根因核查：原始 `KERNEL_SELFTEST=1` 症状已被 AAGU-5.6 variant 目录顺带修复（复现实测 169 个 .o 全落 `kernel/selftest/`，normal 命名空间仅共享 runtime builtins 被动重建）；本次闭合通用面——实测注入 `-D` 探针后 0 个 `.o` 重编。`make test-contract`（x86 8 modes + aarch64）PASS；`aarch64-uefi-kernel` 构建正常。**预先存在的独立问题**（未修）：aarch64 contract `targets` mode 依赖 x86 build 目录存在——`make -n PROFILE=x86_64-clang kernel.bin` 的 `+` 前缀 artifact recipe 在 `-n` 下也执行，host-test mode 末尾的 `make clean` 删掉 x86 目录后 aarch64 contract 即失败（clean workspace 上跑 aarch64 contract 同样触发）

- refactor: **AAGU-4 残留清理 — close §3.3 ❌ + 🟡** —— branch `docs/roadmap-slim-v30`（commit 链路 `099b060`（spec）→ `4e1d33b`（plan）→ 4 task commits）：
  - **Task 1**（`b994d8f`）：softirq 原子操作实现从 `kernel/arch/<arch>/cpu/atomic.c` 外部函数迁移到 `kernel/include/arch/<arch>/atomic_bitops.h` static inline + `__attribute__((always_inline))`；`kernel/intr/softirq.c` 删除 2 个 `#if defined(__x86_64__)/#elif defined(__aarch64__)/#else #error` 块
  - **Task 2**（`3ababa8`）：`kernel/time/tick.c` poll-timeout scan + PIT/LAPIC handoff 拆分：weak 默认 `poll_timeout_tick()` 在 `kernel/time/tick.c`（aarch64 phase 1 路径），strong 实现在 `kernel/fs/poll.c`（x86_64）；PIT/LAPIC handoff 合并到 `kernel/arch/x86_64/platform/time.c::arch_tick_start()`；`tick.c` 删除 2 个 `#if defined(__x86_64__)` 块
  - **Task 3**（`29996e9`）：`kernel/include/time/clocksource.h` 2 个 `#if defined(__x86_64__)` 块（percpu include + `clocksource_read_ns()` inline）迁至新建 `kernel/include/arch/x86_64/clocksource.h`；`kernel/time/clocksource.c` + `kernel/time/timer.c` SUBSYS_INITCALL ifdef 删除；`timer.c` spin hint 接 `arch_cpu_pause()`
  - **Task 4**（`e91d0b9`）：`kernel/driver/{ahci,keyboard,pit,serial}.c` + `kernel/net/net.c` 共 5 个 TU 删除冗余 `#ifdef __x86_64__` SUBSYS_INITCALL 守护（TU 已在 x86-only 路径，Makefile 已 gate）
  - 文档同步：`docs/arch/cross-boundary-symbols.md` §3.3 ❌/🟡 状态全改 ✅，§6 验收清单增条目
- fix(aarch64): **IPI cpus≥2 FAIL 根因修复（TPIDR_EL1 误读）** —— commit `d695020`（worktree `fix/aarch64-ipi-fail`，当前 HEAD）：`kernel/arch/aarch64/intr/ipi_test.c::ipi_cpu_id()` 从 `TPIDR_EL1` 取指针后误解释为 `aarch64_boot_percpu_t *`（实际 `percpu_t`），跨字段偏移导致 cpu≠self 时 `id` 计算错位（`cpu=1 received=3`、`cpu=2/3 received=0`）。两个「暖机」SGI 掩盖真问题：`cpu=0` 暖机时 IPI handler 调 `gic_send_sgi(self,2,0,FILTER_SELF)` 写 `0x02000002`，触发 SGI 2 self-trigger，handler 重入将 `received[cpu]` 加 1 —— 看似通过；删暖机后 `cpu=1..N-1 received == 0`。修：去掉类型转换，改用运行期 `cpu_id()`（`mrs x0, TPIDR_EL1` → `percpu_current()`），删除两个暖机 SGI；`SGI_TEST_ACK_COUNT == N-1`（`cpu=0` 不应收到自给 SGI）+ `GICC_IAR == 0x401`（sgi_int_id=2，CPU targets=1）严格断言。QEMU `make PROFILE=aarch64-clang test-aarch64-uefi-smp` 1/2/4 ×3 共 9/9 PASS（0 TIMEOUT / 0 FAIL）。完整根因 + 修复记录见 `docs/archive/aarch64/aarch64-ipi-fail-handoff-2026-09-26.md`
- ci: **bucket test targets 迁移** —— commit `36e6230`：CI workflow 从旧的 forwarding `test-*` aliases 切到 6 个 bucket 目标（`test-qemu` / `test-host` / `test-static` / `test-aarch64` / `test-contract` / `test-kernel-selftest`）
- refactor(harness): **删 forwarding `test-*` aliases** —— commit `ce258a1`（worktree `feat/build-system-harness-consolidation` 收尾）：删 09-25 引入的临时 forwarding `test-*` aliases，仅保留 6 个 bucket 目标作为权威入口。follow-up 见 `9f13465`（track followup）

## 2026-09-25

- refactor: **build system harness consolidation**（worktree `feat/build-system-harness-consolidation`，12 commits `70daaea..ce258a1`）：统一 6 个 bucket test 目标（`test-qemu` / `test-host` / `test-static` / `test-aarch64` / `test-contract` / `test-kernel-selftest`），target taxonomy / capability gate / alias policy 收敛到 `docs/build/build.md` 作为权威 reference：
  - `70daaea` plan (v3, 12 tasks) → `64dd4ac` gate x86 validation + 公开隐藏 test target → `5c43897` drop unused `all` alias → `452bbf1` DRY QEMU command lines（`RUN_QEMU_BASE` / `_FLAGS_<target>`）
  - `f2652a7` consolidate 4 QEMU E2E 目标 → `test-qemu SUITE=<name>`（保留旧名为 aliases）→ `58a6ef9` canonical `test-host` + 拆分 pmm helper → `64e3207` `test-static` umbrella + 4 subset aliases
  - `c28e67a` consolidate 3 aarch64 tests → `test-aarch64 MODE=<smp|no-ack|gic-spi>` → `7350c61` consolidate 2 contract tests → `test-contract PROFILE=<name>`
  - `782e964` `docs/build/build.md` 新建（target taxonomy 权威 reference）→ `30f3508` update Quick start / 用户入口 / test recipes
- refactor: **arch source groups**（commit `c3412da` + `18a4cff`，merge `79ccffb`，spec `2026-09-25-arch-source-groups-design.md`）：kernel 源按 `kernel/arch/<arch>/<topic>/` 职责分组（如 `kernel/arch/x86_64/intr/{8259A,lapic,lapic_timer}.c`、`kernel/arch/x86_64/sched/{task,switch}.c`、`kernel/arch/aarch64/intr/{gic_driver,gic,irq_probe,ipi_test,entry.S}` 等），更新所有架构引用路径 + 文档架构图
- refactor: **compiler_rt 目录裁撤**（commit `1c5816b`，merge `ca72145`）：`kernel/compiler_rt/` 整个目录裁撤，符号落点各归其位——`__stack_chk_*` 迁至 `kernel/core/stack_chk.c`；`__udivti3` 走 `runtime/builtins/`；elf-loader 保留 `runtime/builtins/`。消除「目录名与实际职责不符」的违例
- fix(run): commit `1fbca71` aarch64 QEMU harness 改用 selftest 变体镜像
- feat: **AAGU-5.8 三档环境测试 + 验收清单** —— commit `a05d975`（PR #28，worktree `feat/aagu-5-entropy-facade`）：QEMU 默认 / `+rdrand,+rdseed` / 真硬件 三档测试 + 验收清单。spec `docs/arch/entropy-source-facade.md`
- feat: **AAGU-5.7 STRONG-only pool + selftest NONE-mode + spawn/exec 策略** —— commit `587a894`（PR #27，6 commits `c97a8f5` / `3721e93` / `001528e` / `e9ae356` / `21f0bc7` / `587a894`）：STRONG-only pool + selftest NONE-mode 修正 + AT_RANDOM STRONG-only 控制流 + B3 WEAK-pool e2e 测试 + spawn/exec 决策

## 2026-09-24

- feat(aarch64): **AAGU-5.6 arch entropy facade + strong overrides + kernel/random refactor** —— merge #25（commits `4ab3370` / `f41e4e6` / `1c5bf17`）：spec `docs/arch/entropy-source-facade.md` + `arch_random_get_entropy()` + `arch_random_get_strong()` 双接口 + `kernel/random` refactor。x86_64 strong override：`RDSEED` = STRONG / `RDRAND` = WEAK；aarch64 strong override：`RNDRRS` = STRONG / `RNDR` = WEAK
- feat(aarch64): **AAGU-29 aarch64 libk.a link**（commit `bee8da5`，merge #26 `bec4a4c`，worktree `fix/aagu-29-libk-aarch64`）：aarch64 kernel 现在 link `libk.a`（`kernel/arch/aarch64/make.config`: `ARCH_LIBS = -nostdlib -lk`）→ `memcpy/memset/memmove/calloc/free/malloc/strlen/strcmp` 走 libc 单一来源。删 `kernel/compiler_rt/memset.c` weak fallback + `kernel/arch/aarch64/libc_stub.c` calloc/free shim。完整闭项见 `docs/archive/aarch64/aarch64-libk-aarch64-closure-2026-09-24.md`
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
  - **Phase 2 #4** `__udivti3` hoist 至 `runtime/builtins/`（merge `5edf79a`，commit 详见 `docs/archive/aarch64/aarch64-udivti3-hoist-closure-2026-09-18.md`）
  - **Phase 2 #5** `-I libc/include` 清理：6 mirror 头（`list/rbtree/string/stdlib/sys-cdefs/sys-types`）改走 `kernel/include/compat/`，由 libc 单一来源提供（merge `a110ab9`，详见 `docs/archive/aarch64/aarch64-libc-include-policy-closure-2026-09-18.md`）
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
| **文档学习层**：`docs/README.md` 索引（按 0/1/2/3/4 层组织，全局认知→架构骨架→核心机制→I/O 子系统→构建调试）+ 4 份源码导读（调度器 / trap.c / vfs+memory / tty+intr），让新人按指定顺序读源码即可走通关键路径。`docs/README.md` + `docs/sched/scheduler-reading-guide.md` + `docs/syscall/trap-reading-guide.md` + `docs/memory/vfs-memory-reading-guide.md` + `docs/interrupt/tty-intr-reading-guide.md` | 1 天 | 09-13 |

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
| **自托管 compiler runtime**（多个 commits）：udivti3 实现 + provider-keyed selfhosted archive + provider 构建不变量硬化 + 内核链接 compiler runtime + kernel link publication 加固 + compiler-rt eligibility 验证 + kernel runtime validation targets + syscall/selftest suite 隔离 + variant link paths + root `make sysroot` 入口。详见 `runtime/` + `docs/build/build.md` | 3 天 | 09-04~09-05 |
| **aarch64 UEFI bootloader 统一**（commits `af166bc`..`06e6127`，merge `06e6127`）：x86_64 + aarch64 共享 `boot/uefi/main.c` + arch 分发 + `boot_context` handoff ABI + boot_context 头部偏移断言 | 1 天 | 09-03 |
| **aarch64 UEFI 固件修复**：firmware 截断 64MiB 适配 QEMU pflash（`11aa6ed`）+ aarch64 UEFI 默认 URL 下载（`bad8825`）+ aarch64 也显式传 clang+lld 到 posix-uefi（`25872d1`） | 半天 | 09-03 |
| **profile-only UEFI overlay 简化**：x86 UEFI 固件 per-profile（不再用运行时 overlay patch）+ host test 按 profile 隔离 + 所有组件强制声明 profile + profile-only UEFI cleanup contract | 半天 | 09-03 |
| **GNU Make 构建系统重构**（profiles + 受控递归 Make + 单 writer sysroot 原子 generation 发布 + 锁/租约协议 + kernel/libc/user/busybox/mbedtls/posix-uefi 组件适配器 + rootfs manifest + mkdisk 重写 + aarch64 bring-up 图分离 + 变体隔离镜像 + 全部公开 target 恢复。QEMU 验证：x86 disk.img 启动、aarch64 handoff/phase1 双签名、systest 228/228、nettest 6/6。契约测试 5 模式全绿）。**Plan deviation**：rootfs applet 项以 busybox 副本替代 symlink（内核 vfs 无软链接跟随 → exec symlink ENOEXEC，历史构建即副本规避）；kernel exec-symlink gap 列入 roadmap（P1/P5） | 1 天 | 09-02 |

## 2026-08-24 ~ 08-26

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **syscall 边界审计**（commits `a1ad1b9`..`80eab1a`，11 commits）：逐 syscall 检查 user-pointer 边界 + 可睡眠路径 + copy_{to,from}_user 失败处理 + ASLR/canary 未来兼容；详见 `docs/syscall/syscall.md` 末尾审计触达清单 | 半天 | 08-24~08-26 |

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

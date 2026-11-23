# OS01 优化路线图

> **基准**：`a92938b8`（master，2026-11-22）　**日期**：2026-11-22

本文件**只列未完成 / 进行中**的规划项。已完成工作见 [`changelog.md`](changelog.md)；**最近 3 天的闭环快速索引见文末「近期闭环索引」**；闭环细节见各专题文档：
[`arch.md`](arch.md) · [`memory/memory.md`](memory/memory.md) · [`memory/cow-mmap.md`](memory/cow-mmap.md) ·
[`gui/gui.md`](gui/gui.md) · [`driver/driver.md`](driver/driver.md) · [`testing/testing-policy.md`](testing/testing-policy.md) ·
[`libc/libc.md`](libc/libc.md) · [`signal/signal.md`](signal/signal.md) · [`decisions.md`](decisions.md)。

## 待办总览

| 优先级 | 主题 | 未完成项 | 备注 |
|-------|------|---------|------|
| **P0** | 工程与测试基础设施 | 0 | flake 账本入库已于 2026-11-21 闭环（见 [`changelog.md`](changelog.md)） |
| **P1** | 安全加固 | 2 | UBSan/KASan 后续项（v1 已于 2026-11-22 闭环）+ ASLR 剩余范围（栈基址随机化 / aarch64） |
| **P2** | aarch64 适配 | 10 | 内存与执行体 0 · 能力齐平 6 · 架构统一 0 · 真机阻断 4 |
| **P2** | 架构治理 / 扩展性 | 0 | 新增治理项沿 AAGU 编号追加，先记入 [`decisions.md`](decisions.md) 与 [`changelog.md`](changelog.md)，再回填该节 |
| **P3** | GUI | 7 | GUI-4/5/6 + 优化与验证 4 |
| **P4** | 硬件适配 | 5 | USB 2 · NVMe · HPET · ACPI S3 |
| **P5** | ABI 扩展 / 兼容性 | 14 | syscall 与 libc 缺口 |

推进顺序：**P0 → P1 → P2 → P3 / P4 → P5**；P3 GUI 与 P4 硬件可并行。

---

## 🧰 P0 工程与测试基础设施

当前无剩余项。本节最近闭环：**flake 账本入库**（2026-11-21，账本文件 `config/flake-ledger.json`
入库 + CI 夜间调用报告器，见 [`changelog.md`](changelog.md) 与
[设计 spec](superpowers/specs/2026-11-21-flake-ledger-design.md)）。

## 🔒 P1 安全加固

前置链（已完成，详见 `docs/changelog.md`）：`getrandom` → 统一用户态启动方式 → 用户栈 canary + `AT_RANDOM` →
AAGU-5 arch entropy facade（STRONG-only 控制流）→ 用户堆与 ELF 映射隔离 → **ASLR-A：x86_64 `mmap_base` 随机化** → **ASLR-B：x86_64 静态 PIE 与镜像基址随机化**（均于 2026-11-22 闭环，见 [`cow-mmap.md`](memory/cow-mmap.md) 与 [`changelog.md`](changelog.md)）。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| UBSan + KASan 后续项 | x86_64 内核插桩 **v1 已闭环（2026-11-22）**（`KERNEL_UBSAN=1` / `KERNEL_KASAN=1` 独立 knob、两阶段影子、slab 毒化钩子、UBSan 运行时、探针端到端验证，见 [`build.md`](build/build.md) 与 [`changelog.md`](changelog.md)）。**剩余四项**：① 栈插桩（需 Linux 式早期文件排除清单）；② 全局插桩 / 全局红 zone（需 `.init_array.1` 扫描）；③ aarch64 接入（UBSan 运行时本身架构中立，但两个 knob 当前都门禁在 x86_64 profile，`mk/project.mk:470-471`、`:523-526`）；④ 偶发 LAPIC 影子 `0xff` 待诊断（报告已带影子帧 PA） | v1 ✅ | ArvernOS |
| ASLR 剩余范围 | ASLR-A（`mmap_base` 随机化）与 ASLR-B（静态 PIE 与镜像基址随机化）均于 2026-11-22 闭环，**完整用户态 ASLR 尚缺两项**：① **用户栈基址随机化**——用户栈仍固定在 `X86_USER_STACK_BASE = 0x20400000`（`kernel/include/arch/x86_64/user_layout.h:20`），A/B 完成后尚不构成完整用户态 ASLR；② **aarch64 尚未纳入 ASLR**——aarch64 已有 EL0 用户态，但本项暂只覆盖 x86_64 | ASLR-A ✅、ASLR-B ✅ | |

## 🏗 P2 aarch64 适配

### 内存管理与执行体后续任务

当前无剩余项。`inode 级 MAP_SHARED` 的剩余五项（磁盘回写、fd I/O 一致性、truncate 撤页、共享内存 ABI、`x86_64` 用户 VM 接入）已于 2026-11-22 全部闭环，见 [`changelog.md`](changelog.md#2026-11-22) 与 [`memory/cow-mmap.md`](memory/cow-mmap.md#3-跨进程-inode-级-map_shared2026-11-22aarch64)。

### 能力齐平：对标 x86_64 的剩余缺口（2026-11-21 扫描，2026-11-22 复核）

范围：能力门（`kernel/include/arch/features.h`）+ 共享 syscall 表（`kernel/syscall/dispatch.c`）+ 驱动/设备注册面 + userland 应用与 E2E 套件的全量对比。差距呈「外设与生态 > syscall > 执行体细节」梯度：执行体核心（fork/exec/信号/SMP 用户调度）已收口，本节只记**此前未立任务的缺口**。头部事实：`OS01_HAS_NET` / `OS01_HAS_PCI` / `OS01_HAS_USB` 在 aarch64 全为 0；共享 syscall 表 x86_64 服务 **79** 项、aarch64 产品态 **57** 项（15 共享原生 + 33 fs 组 + 5 misc/msync + 4 time 组），缺 22 项 = 网络 13 · 会话 5（`umask`/`setpgid`/`getpgid`/`setsid`/`getsid`）· rlimit 2 · `futex` 1 · `putchar` 1。

| 任务 | 内容与完成条件 | 前置 |
|------|----------------|------|
| 会话/资源 syscall 组 | 两档：`getrlimit`/`setrlimit` 的 `sys_resource_dispatch` 架构中立（`kernel/syscall/sys_resource.c`），解除隐藏即得；`umask` 与进程组四项的 `sys_proc_dispatch` 现为 x86 专属实现（`kernel/arch/x86_64/syscall/sys_proc.c:36`），须先下沉到 `kernel/syscall/` 再解除 aarch64 隐藏；`putchar` 需在 aarch64 自有 misc handler 补桩——**现 `kernel/arch/aarch64/syscall/sys_misc.c` 只服务 getrandom/sync/reboot/uname 四项（dispatch 入口 `:25`），尚无 `putchar` 分支**。验收 = aarch64 guest 内 `setsid`/进程组行为与 x86 一致 | 独立（SIGTSTP/SIGCONT 停止状态机是 P5「作业控制」既有缺口，两架构共用，不在本项） |
| futex(2) 移植 | `kernel/arch/aarch64/sync/futex.c` 已建 64 桶等待队列但无 syscall 到达（头注释自述任务说明）；把 x86 `do_futex_wait`/`wake` 的用户地址物理页解析（x86 走 4KB/2MB 叶 helper）迁到 aarch64 leaf walker（`kernel/include/arch/aarch64/user_vm_leaf.h`），解除 `SYS_futex` 隐藏。P5 clone/pthread 链在 aarch64 侧的硬前置 | EL0 用户页表叶游走 ✅（user_vm_leaf） |
| 网络栈接入（`OS01_HAS_NET` 0→1） | 三段：① lwIP 纳入 aarch64 profile 构建（现 aarch64 不编译 lwIP，`kernel/arch/aarch64/make.config:46` `ARCH_ENABLE_LWIP = 0`，x86 同文件 `:46` = 1）；② virtio-net 传输层——现驱动是 x86 PCI legacy IO-port 传输（`kernel/driver/virtio-net.c:1-4`，内联 `inl/outl`），aarch64 需参照 virtio-blk-mmio（`kernel/driver/virtio_blk.c` + `kernel/include/arch/virtio_mmio.h`）实现 MMIO transport，或先抽公共 virtio 设备核心再两架构分接；③ 翻门解锁 `kernel/fs/file.c` 的 4 处 FD_SOCKET 块（`#if OS01_HAS_NET`：`:30-43` / `:181` / `:942-1012` / `:1280-1328`）并解除 13 项 socket syscall 隐藏（`kernel/syscall/dispatch.c:123-135`）；`kernel/fs/poll.c:328-350` 的 FD_SOCKET 分支**无编译门**，仅因 socket syscall 隐藏而运行时不可达，无需翻门。验收 = aarch64 上 `SUITE=network` 等价 E2E 跑通 | 独立；E2E 依赖本节「E2E 套件 aarch64 化」 |
| 输入设备（键盘/鼠标） | aarch64 唯一输入是 PL011 RX → console TTY（`kernel/arch/aarch64/platform/console.c:37-42`），无 i8042/PS2、无 xHCI HID（`OS01_HAS_PCI`/`OS01_HAS_USB`=0）→ 无键盘/鼠标。QEMU virt 路线：新增 virtio-input MMIO 驱动（全树无 virtio_input 代码）作生产者，接入 `input_route` 路由与 /dev/keyboard、/dev/mouse 消费面（现字节消费链挂在 i8042 demux 上，`kernel/driver/keyboard.c:246-251`，需先分层解耦）。真机 USB HID 路线远期（aarch64 无 PCI/USB 计划）。P3 GUI「aarch64 GUI 等待显示设备基础就绪」中输入是缺口的一半 | 独立 |
| 图形/GUI 栈 sysroot 与应用解禁 | aarch64 userland 排除 14 应用（`mk/profiles/aarch64-clang.mk:362` 的 `USER_EXCLUDED_PROGRAMS`：desktop/doom/doom_testctl/tetris/test_lvgl 与 GUI 测试组/test_gfx/usb_hid_probe/test_display_owner/netmodeltest/systest/test_monitor/test_monitor_test），根因 sysroot 无 libgfx/libgui/lvgl/mbedTLS（`user/apps.mk:19-25`）。内核侧基础已在：fb/gfx0 devfs 节点已注册（`kernel/arch/aarch64/boot/main.c:884-889`）。任务 = sysroot 图形库移植 + 逐应用解禁；ramfb 现非字形控制台（`kernel/arch/aarch64/platform/console.c:5`），fb 文本控制台按需另列 | 交互类应用依赖「输入设备」；doom 等纯显示应用仅依赖 sysroot |
| E2E 套件 aarch64 化 | `test-qemu` 的 **25 个 x86-only E2E 套件**（systest/network/gfx/terminal-present/gui/usb-boot 等；`mk/generated/test-dispatch.mk:2` 的 `TEST_QEMU_SUITE_IDS` 共 41 项，其余 16 项为 `aarch64-*`）全部 x86-only；aarch64 侧只有 aarch64 验收族：`make test-aarch64 MODE=<smp\|no-ack\|gic-spi\|sync-fault\|serror\|exception-trace\|m3-probe\|sched\|sched-ap\|sched-wake\|sched-tick\|user-runtime\|m1-ram\|m1-sparse\|m1-arena-exhaust\|m1-table-exhaust\|m1-ap-bad-root\|boot-contract\|lma-layout>`（`mk/components/run.mk:663`，**共 19 种 MODE**——`serror` 与 `exception-trace` 为 2026-11-21 新增）+ `SUITE=aarch64-*` 16 项 + `test-contract`（`mk/components/run.mk:2036`，PROFILE=aarch64-clang）。优先把 systest（fork/exec/waitpid 进程语义 E2E）与 terminal 冒烟扩到 aarch64 profile，其后随 syscall 组解禁逐套件跟进 | 无硬前置；个别用例依赖上面 syscall 组先行解禁 |

### 架构统一推进链（迈向单一 kernel_main）

剩余待统一项：

当前无剩余项。**GICv2 Phase 2 已于 2026-11-21 完成软件实现与 QEMU 验收**（正式 EL1 VBAR `+0x180`/`+0x380`/`+0x580` SError 入口→`aarch64_serror_fatal` 首错诊断→`aarch64_serror_park`、`AARCH64_USER_TASK_PSTATE=0x240`、GICv2 `SecurityExtn` Non-secure Group 1 交接契约与 PMR 校验、1020 INTID 边界与 1020–1023 跳过、`gic_cpu_target_mask` 折叠与每核原子 claim），闭环记录见 [`changelog.md`](changelog.md) 与 [`arch.md`](arch.md) §AArch64 SError 与 GICv2 Phase 2。**真机取证**（实机 IRQ 覆盖与物理 SError 注入）并入下方「真机适配审计」的 GICv2 交接条件阻断行统一跟踪。

**距离「用户态就绪的统一内核」还差多远（粗估，一个人全职）**：剩余差距集中在 x86_64 signal 帧 FPU 区（见 P5）；aarch64 的 EL0 FP/SIMD 已于 2026-11-21 闭环、跨进程 inode 级 `MAP_SHARED` 已于 2026-11-22 闭环（aarch64 侧）。详见 `docs/arch.md` §aarch64 用户态现状与末段、统一 kernel_main spec/plan。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；
中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。靠 arch 抽象层封装，统一接口、不统一实现。

#### 真机适配审计：QEMU virt hardcode 清单（2026-11-17 扫描，2026-11-22 复核）

范围：`kernel/arch/aarch64/` + `kernel/include/arch/aarch64/` + `boot/uefi/arch/aarch64/` 全量通读。审计结论是 hardcode
呈三层固化——① DRAM 基址 / 内核加载地址；② 早期 MMIO 窗口；③ DTB 解析器实为「QEMU virt 形状校验器」（解析出的设备基址先与
QEMU 常量对账、相等才通过，而不是用起来）。加载地址层、DTB 地址层与早期 MMIO Device 窗口 / 控制台 handoff 运行时化已闭环
（见 [`arch.md`](arch.md) §aarch64 平台值解绑），下表只保留**仍开放的真机阻断项**。

**硬阻断（任何非 QEMU virt 平台直接无法启动）**：

| 问题 | 位置 | 真机影响 | 状态 / 剩余解法 |
|------|------|----------|----------|
| DTB 形状校验（PL011 interrupts）与 GICv3 缺失 | `kernel/arch/aarch64/platform/dtb_parse.c:152-155`（PL011 interrupts 仍要求恰 3 cell + GIC_SPI=0 + IRQ_TYPE=LEVEL_HIGH）；GIC compatible 白名单只收 `arm,cortex-a15-gic` / `arm,gic-400`（`:133-134`）；全树无 `arm,gic-v3` | DTB 形状不同的板被拒；GICv3 平台无法启动 | ❌ **仍开放**：放宽 PL011 interrupts 形状校验 + GICv3 另列驱动任务 |

**条件阻断（能到早期启动，特定环节卡死/静默失效）**：

| 问题 | 位置 | 真机影响 | 状态 / 剩余解法 |
|------|------|----------|----------|
| GICv2 Group 0 / Non-secure Group 1 交接 | `kernel/arch/aarch64/intr/gic_driver.c` | 无 Security Extensions 时配置 Group 0；有 `SecurityExtn` 时假定固件已为 OS-owned SPI 及各核 SGI/PPI 配置 Group 1 与 NS-accessible PMR（跳过 IGROUPR 并校验 PMR 读回非零） | ✅ **软件侧已就绪**（2026-11-21）；**真机待验证**：TF-A Group 1 实测 + 实机 IRQ 覆盖（CNTP/定向 SGI/PL011 RX→TTY）与物理 SError 注入，按 [`arch.md`](arch.md#aarch64-serror-与-gicv2-phase-22026-11-21) 的板级证据模板取证 |
| 非 BSP CPU 强制 enable-method=psci | `kernel/arch/aarch64/platform/dtb_parse.c:108`（`exact(n->enable,"psci")`）、`:361-362`（非 BSP 无 psci → -3 FATAL） | spin-table 平台（老 RPi 固件、`cpu-release-addr`）全部 AP 拒启（-3 FATAL） | ❌ **仍开放**：若目标板需要，加 spin-table 等待路径 |
| timer 中断形状校验偏脆（要求 interrupts 第二项 = PPI 14） | `kernel/arch/aarch64/platform/dtb_parse.c:200-207`（`len < 24 \|\| len % 12 \|\| data+12 != 1 \|\| data+16 != 14`；`cntp_ppi` 写死 30 = 16+14，GICv2 架构定值本身正确） | 真机 DT 省略 secure timer 项时条目位移被 -4 拒 | ❌ **仍开放**：改为按"non-secure physical timer 的 PPI 30"匹配而非固定槽位 |

**平台能力上限（非阻断，选型需知）**：PA 上限 40-bit/1 TiB（`kernel/arch/aarch64/head.S:252-268` TCR IPS=0b010、`kernel/arch/aarch64/memory/page_table.c:51`；**IPS 仍为编译期硬编码，全树未实现 `ID_AA64MMFR0_EL1.PARange` 动态探测，>40-bit PA 平台不支持**）；早期 identity map 为 [0x00000000, 0x80000000)（`kernel/arch/aarch64/head.S:168-212`，`aarch64_build_boot_page_tables` 调用在 `:199-212`，范围常量 `AARCH64_BOOT_MAP_END=0x80000000` 见 `kernel/include/arch/aarch64/boot_layout.h:41`）；**Device 窗口优先取 boot_context handoff 运行时派生值**（`kernel/arch/aarch64/head.S:199-211`，含 `:168-197` 的 fail-closed 校验），无 handoff 时才回落 QEMU 常量 [0x08000000,0x0a000000)（`kernel/arch/aarch64/head.S:205-206`）；aarch64 已接 RNDR/RNDRRS（`kernel/arch/aarch64/cpu/random.c:30-60,110-122`，
经 `ID_AA64ISAR0_EL1.RNDR` 探测，输出 STRONG/WEAK/NONE），STRONG 池可用，无 RNDR 的板只剩 EFI_RNG_PROTOCOL
（无 RngDxe 的板 random_init fail-closed）；`AARCH64_M1_SPARSE` 假设 0x4fe00000..0x50400000 是 RAM（仅自测宏）。

**剩余解绑顺序**：按目标板补 Group-1 / spin-table → 放宽 PL011 / timer 形状校验 → GICv3 驱动 → 实机启动验证。

### 🧱 P2 架构治理 / 扩展性

#### 剩余架构治理任务

当前无剩余项。新增治理项沿 AAGU 编号追加，先记入 [`decisions.md`](decisions.md) 与 [`changelog.md`](changelog.md)，再回填本节。

## 🖥 P3 GUI

### 后续功能任务（按依赖推进）

| 项 | 内容 / 完成条件 | 依赖 |
|----|-----------------|------|
| GUI-4 字体与资源 | 先使用裁剪的抗锯齿位图字体与编译内置图标，覆盖桌面所需字号；单列中文字符集、字库容量与输入方案。按需再引入动态字体或文件资源加载，并审计依赖、许可证及缓存开销 | GUI-1 ✅、GUI-2 ✅、GUI-3 ✅、GUI-2b ✅；**GUI/LVGL 栈当前字体仍为内置 `lv_font_montserrat_{12,14,16,20,24}`**（`config/lv_conf.h:659-665`，CJK 源全关 `:682-683`，默认 `lv_font_montserrat_14` `:701`；字形集为 ASCII + U+00B0/U+2022 + FontAwesome 图标），无中文字形、无自建裁剪位图字体；**构建产物实为 6 个字体对象**——`lv_font_montserrat_14_aligned` 也被编入（其自有宏默认 1，`thirdpart/lvgl/src/font/lv_font_montserrat_14_aligned.c:25-27`，bpp 8/stride 16，字形集相同）；**唯一自建位图资源是图标而非字体**（`libgui/assets/cursor-arrow.png` → `libgui/src/cursor_icon.c`，16×24 ARGB8888，`tools/gen_cursor_icon.py`），即「编译内置图标」已部分落地；**terminal 侧另有一份内置 PSF2 位图字体 `user/terminal/terminal_font.psf`（objcopy 内嵌），与桌面字库无关**，不可据此认为 GUI-4 已部分落地 |
| GUI-5 Window Server + compositor | 单独设计 surface、IPC、缓冲共享、损伤区域、Z-order、移动/缩放、遮挡、焦点/输入抓取与进程退出回收；可信窗口服务器统一呈现，收紧 `/dev/fb` 与重叠 gfx view 的旁路访问。IPC/共享内存能力先评估，缺项独立补齐；客户端允许使用 LVGL 或其他绘制库 | GUI-1 ✅、GUI-2 ✅、GUI-2b ✅；IPC/共享缓冲与图形权限设计；借鉴 opuntiaOS/HackOS |
| GUI-6 terminal 窗口化 | 保留 PTY/VT100 核心，将渲染输出接入窗口 surface；键盘只投递到焦点终端，验证 Ctrl-C、alt-screen、窗口关闭和 shell 存活；缩放时更新 PTY winsize 并接通 SIGWINCH | GUI-5；terminal/PTY ✅；SIGWINCH ✅（PTY master `TIOCSWINSZ` 尺寸变化时派发到前台进程组，`kernel/tty/pty.c:109,136-138`；`qemutests/test_resolution_switcher.py` 的 `pty-watch` 场景已验证前台进程收到信号并读到四个 winsize 字段） |

顺序：**GUI-4** → GUI-5 → GUI-6；GUI-5/6 是独立系统工程，不与首版单进程桌面捆绑；aarch64 GUI 等待显示设备基础就绪。

### 优化与验证项

| 项 | 当前边界 / 后续工作 | 验收依据 |
|----|---------------------|----------|
| 帧耗时与呈现能力 | 分别测量绘制、复制、内核提交和等待开销，覆盖多分辨率及实际阴影/滚动/动画场景；按瓶颈优化。vsync、page flip、write-combining 或 GPU 后端另行设计；当前私有缓冲不保证无撕裂 | 记录帧耗时分布、CPU 占用与上传量；优化前后使用同一场景比较。**仍缺**：跨分辨率的帧耗时分布统计与统一帧时序子系统——现有计时是诊断级（内核 present 统计 `kernel/driver/gfx.c:355`、present A/B 诊断 ioctl `GFX_TEST_PRESENT_AB`、Tetris 的 `elapsed_ns`、terminal `[term-present]` 的 `begin_ns/end_ns/success_ns` 上报），全树无 frame-timing 符号；`user/terminal/terminal.c:33` 的 `FRAME_INTERVAL_MS 33` 是**节流**常量而非计时测量，`config/lv_conf.h:1117` 的 LVGL FPS 监视器关闭 |
| 内存池与资源生命周期 | 评估 4 MiB TLSF 池是否适合桌面；大型绘制缓冲使用 mmap，预算包含字体、图片、图层与多窗口 surface。测试反复创建/销毁、低内存和池耗尽，明确失败处理 | 内存占用有上限；长期运行无持续增长；耗尽不崩溃或破坏状态。**当前边界**：4 MiB 池定义在 `config/lv_conf.h:72`（`LV_MEM_SIZE`），但实体是**每进程静态 BSS 数组**（`thirdpart/lvgl/src/stdlib/builtin/lv_mem_core_builtin.c:80-83` 的 `work_mem_int[]` → `lv_tlsf_create_with_pool`），GUI 进程数增加即是实打实的预算问题；大型绘制缓冲已走 mmap（`libgui/src/platform.c:349-350` draw buffer、`:358-359` 离屏 shadow canvas，释放 `:436-438,515-517`）；已有内存**统计**接口 `gui_platform_stats.pool_total/pool_used/pool_max_used`（`libgui/include/gui/gui_platform.h:132-135`，取自 `libgui/src/platform.c:613-618` 的 `lv_mem_monitor`），但**资源生命周期管理完全缺失**（无字体/图片加载器、无 layer/surface 对象、无引用计数；LVGL 全部图片解码器关闭：`config/lv_conf.h:986,989,992,1000,1003,1006`），反复创建/销毁、低内存、池耗尽测试均未做 |
| libc / 可选 LVGL 能力 | **剩余**：① `lseek` 归位到 `libc/include/unistd.h`（现仅声明在 `libc/include/sys/stat.h:145`，实现 `libc/unistd/lseek.c:13`）；② pthread 后端（现仅 `libc/pthread/mutex.c` 的非递归 mutex 与 `libc/include/pthread.h:10-14` 的五个 mutex 函数，缺线程、条件变量、信号量与递归 mutex）；③ FreeType/ThorVG/图片解码库逐项审计（LVGL 自带副本全在 `config/lv_conf.h` 关闭：`:1028` FREETYPE、`:1039` TINY_TTF、`:1078` SVG、`:986` LODEPNG、`:989` LIBPNG、`:992` BMP）。**libm 基础代数集已落地**：`usr/lib/libm.a` 独立发布，导出 16 个符号（`fabs/fabsf`、`copysign/copysignf`、`floor/floorf`、`ceil/ceilf`、`trunc/truncf`、`round/roundf`、`fmod/fmodf`、`sqrt/sqrtf`），来源为 musl 1.2.5（见 `thirdpart/musl-libc-algorithms.manifest`）；全部用整数位运算实现，无浮点算术指令。**剩余范围**：三角/对数/幂等超越函数（`libc/include/math.h:183-198` 只声明那 16 个函数，连 `sin`/`cos`/`tan`/`log`/`exp`/`pow` 的声明都没有）、`long double` 变体、复数与 `fenv`（异常标志，`libc/include/` 下无 `fenv.h`/`complex.h`/`tgmath.h`，`math.h:19-23` 明示"the libc has no fenv"）均未实现；内核不链接 libm，保持 FP-free | 每次配置扩展重新验证构建、符号和运行时；不让可选依赖阻塞基础桌面。**libm 验收依据**：`make PROFILE=x86_64-clang test-libc-api-audit` 断言 16 个符号只由 libm.a 提供（libc.a/libk.a 均不定义）；`make PROFILE=<p> test-libm-asm` 审计已发布归档的反汇编，无浮点算术/FMA/取整/转换指令；宿主 `test_libc_math` 与 guest `64_libm` 分别对拍宿主 glibc 与实跑 guest |
| 构建与版本维护 | 持续验证无修改构建、配置/头文件变更重编、staging 孤立文件清除及已发布 sysroot 编译；审计实际 archive 与当前对象清单一致，删除源码时重建归档。保留固定版本，升级单独回归 | 增量结果与干净构建一致；无陈旧归档成员；符号审计与 GUI 回归通过。**现有覆盖**：`make test-gui-incremental`（`mk/components/run.mk:1828-1900`）已覆盖前四个子项（无修改构建不重编、touch 公共头重编依赖者、install 清孤儿头文件、用**已发布 sysroot** 编译链接一个真实 target ELF），配 `qemutests/test_build_incremental.py`；符号审计 `test-libc-api-audit` / `test-libm-asm`；版本固定走 `thirdpart/*.manifest`（busybox / doomgeneric / lvgl / musl-libc-algorithms / posix-uefi——**mbedtls 无 manifest**）。**缺口**：「删除源码时重建归档」只对 `libm.a` 成立（`libc/Makefile:139-146` 先 `rm -f $@` 再 `ar rcs`），而 `libc.a` / `libk.a` 直接 `ar rcs` 不删包（`libc/Makefile:131-137`）——删源码后旧成员会残留；且**没有任何目标断言「归档成员 == 当前对象清单」**（仅 mbedtls 有对应回归 `qemutests/test_build_incremental.py:88`） |

## 🔧 P4 硬件适配

| 项 | 内容 / 完成条件 | 依赖 | 借鉴 |
|----|-----------------|------|------|
| 真机启动 (USB) —— 物理实机验收 | **代码路径与 QEMU 验收已闭环**（`SUITE=usb-boot` 6/6 子用例：readonly / dual / ahci / persistence / superspeed / hub；另有 `SUITE=usb-hid` 覆盖 direct/hub × SMP=1/2）。**剩余**：制作实机可引导 USB 盘，验证 UEFI + bootloader + xHCI 实机硬件自举路径，并采集真实控制器 BDF、固件版本、外设 `VID:PID`、协商速度、物理拓扑与热拔插日志 | USB 存储 / FAT32 ✅；Hub ✅；HID ✅ | Tilck |
| USB 大容量存储的协议与介质覆盖 | 现仅支持 **BOT**（协议 0x50）、**512 字节逻辑块**、**32 位 LBA**（`kernel/driver/usb_msc.c:18,137-144`）；**无 UAS**、**无 4Kn**、**无 >2 TiB**、**无热插拔重探测**（`xhci_msc_probe_controller()` 仅在 init 调用一次，`kernel/arch/x86_64/driver/xhci_msc.c:157`），也**无用户态挂载入口**（无 `mount` syscall，挂载仅由引导期 ESP GUID 选择决定） | MSC/BOT ✅ | Linux usb-storage |
| NVMe 驱动 | PCIe NVMe 1.4+ 控制器驱动，Admin/IO 队列对与 Doorbell 机制，替代 AHCI 成为高性能块设备（当前 `kernel/` 下无任何 nvme 代码，仅 `user/lspci.c` 有类名字符串） | ARCH-9 PCI/块设备 ✅ | Linux nvme / HelenOS |
| HPET clocksource | 真实硬件跨平台高精度定时器源（当前 `kernel/`、`user/`、`libc/` 全树无 hpet 实现） | 独立 | |
| ACPI 电源管理（休眠部分） | **FADT 仅取 offset 64 的 PM1a 控制块**（`kernel/arch/x86_64/intr/apic/acpi.c:350-359`），**MADT entry 全量解析**（`kernel/arch/x86_64/intr/apic/acpi.c:150` `parse_madt()`：LAPIC/IOAPIC/ISO/NMI/x2APIC），但 **DSDT/AML 全树未解析**；**S5 关机已闭环**（`kernel/arch/x86_64/intr/syscall_frame.c:50-57` 在 `RB_POWER_OFF` 下写 `SLP_EN=0x2000`，其 SLP_TYP 位域为 0）。**剩余 S3 睡眠/唤醒**（SLP_TYP 选择、进入与唤醒路径、真机验证） | S5 ✅ | |

## 📐 P5 ABI 扩展/兼容性

依赖链：`ELF loader ✅ → 动态链接器 → 共享 libc → Alpine apk/musl`；`futex ✅ → clone → pthread`；
`socket ✅ → AF_UNIX`；`mbedTLS ✅（仅入库为 libmbedtls.a，尚未接入 BusyBox；BusyBox 走自带 TLS）→ HTTPS`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 动态链接器 | PT_INTERP + ld.so + 共享 libc | ELF ✅（loader **显式不检查** PT_INTERP，`kernel/arch/x86_64/fs/elf.c:410`「Strictly avoid inspecting PT_INTERP」；`kernel/include/fs/elf.h:81` 定义了 `PT_INTERP` 常量但两架构均不消费——x86_64 ET_EXEC 路径忽略非 `PT_LOAD` 段，**静态 PIE 路径直接拒绝**（`kernel/arch/x86_64/fs/elf_pie.c:119` → `-ENOEXEC`）；aarch64 侧 `kernel/arch/aarch64/memory/user_elf.c:45-46` 以 `return false` **拒绝**而非跳过）；仓库内无 `ld.so` / 共享 libc / 任何 `.so`（`dlopen`/`dlsym` 全树零命中） | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构；**现状**：Linux 兼容层的 `rt_sigaction` 只是转发到旧 `SYS_signal` 的 ABI shim（`kernel/syscall/compat/proc.c:11-19`），无 SA_RESTART / 实时信号 | |
| x86_64 signal 帧 FPU 区 | x86_64 `struct sigframe` 同样无 FPU 区（`kernel/include/uapi/time.h:95-106`，仅 200 B 通用寄存器 + blocked 掩码），signal handler 的 FP 使用会毁掉被打断上下文的 x87/SSE 状态。aarch64 侧已闭环（816 B 帧含 `fp[528]@288`，2026-11-21，见 [`arch.md`](arch.md) 与 [`changelog.md`](changelog.md)），本项为 x86_64 对齐任务 | aarch64 FP 帧实现 ✅（2026-11-21） | |
| clone/pthread | 线程模型 + pthread_create | futex ✅；**现状**：`kernel/include/uapi/syscall.h` 无 `SYS_clone`（Linux nr 56 在兼容表里直接映射到 `SYS_fork`，`kernel/syscall/compat/table_x86_64.c:43`），`libc/include/pthread.h:10-14` 只声明 mutex 一族，无 `pthread_create` | |
| readv/writev | scatter-gather I/O | 独立；**现状**：未进 syscall 表，也无 libc 包装 | |
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立；ARCH-5 挂载树与 dcache 已就绪 ✅；**现状**：`SYS_fstatat` 已有（at 族部分落地，`kernel/include/uapi/syscall.h:101`），但 `openat`/`dup3`/`pipe2` 均未进 syscall 表 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立；ARCH-5 已就绪 ✅（当前仅有 pipe 的合成 stat `S_IFIFO`，`kernel/syscall/sys_fs.c:563-564`，**无 mkfifo**） | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立；**现状**：`libc/unistd/alarm.c:2` 是静默空桩（无 syscall），`setitimer`/`getitimer` 完全未实现 | |
| 作业控制（剩余部分） | **剩余**：SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs，当前仅作 ignore 处理，无 stop/continue 状态机——`TASK_STOPPED` 定义于 `kernel/include/sched/task.h:70` 但内核无任何置位点，`kernel/tty/tty.c:169-170` 明确 "SIGTSTP not implemented: just drop the char."）、控制终端与前台进程组的完整语义。**已具备**：setpgid/setsid/getpgid/getsid ✅、tcgetpgrp/tcsetpgrp ✅、tty ISIG + VINTR/VQUIT ✅、kill pid=0/-pid/-1 ✅、SIGWINCH 派发 ✅（PTY master `TIOCSWINSZ` 尺寸变化时 `signal_pgrp(pgrp, SIGWINCH)` 派发到前台进程组，`kernel/tty/pty.c:109,136-138`；`user/test_resolution.c` 的 `pty-watch` 观察者与 `qemutests/test_resolution_switcher.py` 场景给出运行期证据）。注意**未收到 SIGWINCH 时默认动作仍是忽略**（`SIG_DFL` ignore，见 `docs/signal/signal.md:25,71`；内核侧 `kernel/arch/x86_64/intr/trap.c:866-867` 把 `SIGWINCH` 列入非致命 `SIG_DFL` 集，`:906-908` 的默认忽略分支与 `:1030-1032` 的致命判定同样跳过它） | tty termios ✅；PTY ✅ | |
| /proc 完善 | **全局 `/proc/stat`** ✅ 已就绪（自有 key-value 格式，非 Linux 定长列 ABI，`kernel/fs/procfs_fmt.c:112-146`）；**`/proc/<pid>/stat` 不存在**（pid 目录 lookup 只解析 `status`/`maps`/`fd`，`kernel/fs/procfs.c:596-608`）；`status` 已有 Version/Name/Pid/PPid/State/CPU/Priority/BirthId/StartNs/SampleNs/RuntimeTicks（`kernel/fs/procfs_fmt.c:73-106`），**仍缺 signal mask 与 utime/stime**；**`cmdline` 完全缺失**（`kernel/fs/` 全目录零命中） | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget —— **HTTPS 能力本身已具备**：`CONFIG_TLS=y`（`config/busybox.config.in:980`）启用 BusyBox 1.36 **自带**的 TLS 客户端（`networking/tls.c` + `tls_{aes,rsa,fe,pstm}*.c`，非 OpenSSL/mbedTLS 后端），`CONFIG_FEATURE_WGET_HTTPS=y`（`:994`）时 `wget` 直接走 `tls_handshake()`；`libmbedtls.a` 已入库到 sysroot 但 **wget 并不链接它**（BusyBox 由原生 Makefile 构建、仅覆盖 `CC`/`LD`，全树无 `-lmbedtls`；ASLR-B 只把该归档的构建 flag 改为 `-fPIE -mcmodel=small -mno-red-zone`（`mk/components/sysroot.mk:145`），它仍是静态 `.a` 且**依旧未被链接**）。**剩余**：二选一 —— 把 mbedTLS 真正接进 BusyBox，或明确改用 BusyBox 内建 TLS 并放弃 mbedTLS 路线；并补 HTTPS 端到端验证（**当前无 guest 端 `wget https://` 实测证据**：guest 网络测试走的是 `http://`，`user/nettest.c:224`；`qemutests/build_contract.sh` 的 HTTPS 分支是宿主侧假 wget 夹具，只断言下载 URL 前缀，不构成 guest 端到端证据） | mbedTLS ✅ 已入库为 `libmbedtls.a` | |
| AF_UNIX/socketpair | 本地 socket IPC | socket ✅；**现状**：`socketpair()` 是 `ENOSYS` 桩（`libc/unistd/net_stubs.c:45-48`）；内核 `socket_alloc()` **不校验 `domain`**（`kernel/net/socket.c:64` 的 `socket_alloc(domain, type, protocol)`，`:67-71` 的 switch 只按 `SOCK_STREAM`/`SOCK_DGRAM` 映射 TCP/UDP，`domain` 仅在 `:81` 被原样存入 `s->domain`），请求 `AF_UNIX` 会拿到 TCP/UDP socket 而不是报错——这是正确性缺口，不只是缺功能 | |
| BusyBox applet 悬空软链 | ⚠️ **`config/busybox.config.in` 在 grep/sed/awk/xargs/tar/dmesg 这六个中只启用了 find**（`CONFIG_FIND=y`，`:451`；其余六个均为 `# CONFIG_* is not set`：grep `:483`/sed `:426`/awk `:417`/xargs `:487`/tar `:186`/dmesg `:609`）——生成的 `applet_tables.h` 里也没有这些 applet。**连带缺陷**：`config/rootfs.mk:12` 仍创建 `/bin/grep`、`/bin/sed`、`/bin/awk`、`/bin/xargs`、`/bin/tar`、`/bin/dmesg` 等软链，执行时得到 "applet not found"（悬空软链）→ **待决：启用这些 applet 还是撤掉对应软链**。libc 侧 POSIX 正则引擎未实现（`libc/include/regex.h` 为桩头，`regcomp`/`regexec` 全空转），`glob` 为返回 `GLOB_NOMATCH` 的桩（`libc/stdlib/glob.c:5-12`；libc-completion T16 正则/glob：2026-11-09 用户决定推迟到后续周期）；`fnmatch` 已实现（`*`/`?`/`[]`），**仅 `FNM_NOESCAPE` 被消费，其余 flags 全忽略**（`libc/string/fnmatch.c:12` 的 `(void)flags;` 注释，消费点在 `:55` 的 `*p == '\\' && !(flags & FNM_NOESCAPE)`） | libc | |
| Alpine apk/musl | musl 二进制包兼容路线 | 动态链接器 | cavOS |

## Parked（未闭环 follow-on，随时可拾起）

当前仍挂起项（均不阻塞主线）：

- **中断/异常 dispatch 第一期遗留（2026-10-24）**：
  - **host 测试补强（非阻断）**：decoder 表行名用于失败诊断；增加 x86 CPL0 + U/S=1 仍判 KERNEL、ARM OTHER EC 的无效 FAR 断言；ARM backend 有效 FAR 用例补齐地址/有效位；dispatcher 的 x86 host 现场假设与 `memcmp(pt_regs_t)` 布局假设显式化，注明 longjmp 用例无法观察跳转后的清理；x86 backend 删除 reset 后才断言零计数的空检查；ARM backend RED 证据可补有意破坏断言的 mutation。
  - **调用链审计 fixture 补强（非阻断）**：继续梳理重复边诊断。
  - **维护性收尾（非阻断）**：x86 四个向量常量在 decoder/adapter/backend 三处重复；sync/IRQ backend 的 canary 属性不对称可说明；ARM 私有头统一 `pt_regs_t *` 写法；`test_uaccess.c` 分配注释应与实际 order-1（8 KiB）一致。`hosttests/Makefile` 的 "EVERY production header" 注释应限定为内核头与 ARM shim：libc/framework 的既有手写依赖缺口尚未覆盖，后续单独治理。

- **GUI QEMU suite 中 guest 侧 `test_lvgl` 的用户态 SIGSEGV（2026-10-22 偶发）**：
  - 第二轮修复期间观察到一次（SMP 1 `smoke` 报 `rc=139`，`0x49884D` memset），后续运行与最终干净门禁均未复现。未修、未声称已修。后续需在能复现时抓寄存器/栈再定位。详见 [GUI-2 验证报告](superpowers/reports/2026-10-22-gui-2-validation.md) §7.5。

- **测试配置 QEMU 中 ash 用户态 SIGSEGV（2026-10-24 偶发）**：
  - 验证 terminal ARM 时观察到一次，后续放慢命令发送后通过。尚未定位，详见 [terminal ARM 调查](debug/2026-10-24-fbtest-terminal-arm-validation.md)。

- **monitor 计划遗留 parked 项（2026-10-11）**：
  - **正确性防护**：② `file_free` 仅在 `f->node` 非空时调 `vfs_file_release`，`fs_private` 有值而无 node 的理论状态会泄漏快照（open 契约保证不可达）。
  - **验证强度缺口**：⑥ monitor QEMU 双核绑核验证弱（亚 0.2s 迁移漏检可被 epoch 探测错过；需补受限内核 spin 自测）。
  - **语义/契约选择备案**：⑪ `monitor_collect` 在 `opendir("/proc")` 失败时返回 0 + 空进程列表，与"无进程"不可区分；⑫ thermal `stale` 标记与进程 `stale` 字段等待独立 thermal 计划激活。

- **统一 SMP 启动评审 deferred minors（2026-11-20，待用户决定是否拾起）**：
  - `arch_smp_last_status()` 校验失败路径的 requested/online/online_mask 快照为零值（纯诊断面，无读取方——x86 `x86_smp_fail()` 只写 failed_cpu/failed_stage/arch_rc，`kernel/arch/x86_64/smp/smp.c:113-120`）。
  - 退役名陈旧注释：原记录的三处中，**aarch64 侧两处仍在**——`kernel/arch/aarch64/runtime/early_print.c:1` 把自身路径写成 `kernel/arch/aarch64/early_print.c`、`:8` 引用已删除的 `kernel/arch/aarch64/main.c`；x86 侧原 `kernel/arch/x86_64/smp/boot.c:67` 一行随文件变动移位，`boot.c:19` 的 `arch_boot_aps()` 引用经核对属实（该函数仍在 `kernel/core/smp.c:84`）、非退役名。

- **x86 PTE 权限评审 deferred minors（2026-11-13，4 项，均 Minor）**：
  - ① `kernel/selftest/{test_uaccess,test_deep_copy_argv}.c` 中间表 PA 仍用 `PAGE_4K_MASK`（如 `kernel/selftest/test_uaccess.c:257,268,303,321`）；② `kernel/include/arch/x86_64/pte.h:22` 遗留含糊 `PAGE_PAT` 别名（零用户——`pte_bits.h:30-32` 已用 `PAGE_PAT_LARGE` 取代含糊名，但旧别名仍在）；③ `hosttests/cases/test_x86_vmm_backend.c` 缺文件尾换行。
  - ~~原第 ③ 项「`trap.c` 无 VMA COW resolve 未走 `x86_pte_4k_make`」已闭环~~：VMA COW resolve 的 privatize 与 in-place upgrade 两个分支现均走 `x86_pte_4k_make`（`kernel/arch/x86_64/intr/trap.c:622,628`）。

- **内存管理架构边界评审 deferred minors（2026-11-14，M-1..M-5）**：
  - ~~① `memory/user_vm.h` 首行 guard 外 include ARM `vmm_backend.h`~~ **已闭环**：该头已归位到 `kernel/include/arch/aarch64/user_vm.h`，且不再 include ARM `vmm_backend.h`；② ~9 个夹具 + `printk_fb.c` 注释级旧路径（`printk_fb.c` 现按架构落在 `kernel/arch/x86_64/runtime/` 与 `kernel/arch/aarch64/runtime/`，自身头部路径已更新）；③ syscall 审计豁免是仓库级非按文件；④ `mm_header_boundary.py` flag 传递依赖全 dash 前缀（`qemutests/mm_header_boundary.py:98-103` 的 argparse 全为 `--` 前缀）；⑤ ARM fixture shadow mmu.h 用 bool 无 stdbool.h（`hosttests/mock/page_table_aarch64_include/arch/aarch64/mmu.h`，`bool` 有 2 处、`stdbool.h` 未 include）。
  - **中立目录归位第二轮**：`driver/serial.c`、`keyboard.c` 等不引用架构头的 x86-only 文件仍在原目录，整体迁移可另开一轮。

- **Craton 改名暂缓（2026-11-17 用户决定）**：
  - README + boot logo 已改（`13c379be`）并验收；**GitHub repo 名与其余 OS01 字符串（2026-11-17 实测 5159 处 / 926 文件：docs 317、kernel 207、hosttests 154、qemutests 99、user 42、libc 23）明确暂缓**，含 `OS01_*` 构建标识符族（`OS01_PROFILE_FILE` / `OS01_SYSTEST` / `OS01_SELFTEST` 等）、include 路径 `<os01/compiler_rt.h>`、LICENSE/README/AGENTS.md 标题行。机械但广面的替换，宜与 repo 重命名同批做。
  - aarch64 boot logo 字标区缝隙透出灰色 (152,152,152) 底层小字（2026-11-08 起既有，疑为固件/早期控制台文字残留，logo 不做整带清除）：可作日后清理项（画字标前清带）。

---

## 近期闭环索引（最近 3 天：2026-11-20 ~ 2026-11-22）

只列条目名与文档指针；完整「做了什么 / 关键结论 / 验证」见 `docs/changelog.md` 对应日期条目。更早的闭环记录同样在 changelog，不在本索引重复。

| 闭环项 | 日期 | 详情 |
|--------|------|------|
| **aarch64 handoff 动态分配（解除 128 KiB 窗口固化）**（`boot/uefi/arch/aarch64/boot.c` 改为在 `[0, AARCH64_BOOT_MAP_END)` 内且避开 boot block 动态 `AllocatePages(AllocateMaxAddress, EfiLoaderData, ...)` 分别分配 `boot_context`、DTB 与 UEFI 内存图；`boot_layout.h` 移除 `AARCH64_HANDOFF_BASE`/`END`，内核上界放宽至 `AARCH64_TRAMPOLINE_BASE`；2 核 QEMU 实测 1 MiB DTB 引导与 selftest 全绿） | 2026-11-23 | [changelog](changelog.md#2026-11-23) |
| **aarch64 时间 syscall 组解禁**（`kernel/syscall/dispatch.c` 在 `OS01_HAS_USERSPACE=1` 下对 aarch64 开放 `sys_time_dispatch` 四项 `SYS_time`/`gettimeofday`/`nanosleep`/`times`，`kernel/Makefile` 纳入 `syscall/sys_time.c`；2 核 QEMU 实测 `/bin/timeit /bin/busybox sleep 1` 真睡 `real 1.021863`） | 2026-11-22 | [changelog](changelog.md#2026-11-22) · [syscall](syscall/syscall.md) |
| **inode 级 `MAP_SHARED` 剩余五项闭环**（磁盘回写 `msync`/`fsync`/`sync`/拆卸落盘、fd I/O 读写与共享页双向覆盖对齐、`vfs_truncate` 撤页与尾页清零回收、POSIX `shm_open`/`shm_unlink` + `SYS_msync`/`SYS_fsync` ABI、`x86_64` 用户 VM 全链接入共享对象注册表） | 2026-11-22 | [changelog](changelog.md#2026-11-22) · [cow-mmap](memory/cow-mmap.md#3-跨进程-inode-级-map_shared2026-11-22aarch64) · [syscall](syscall/syscall.md) |
| **USB boot M0 固件检查点退役**（`USB_BOOT_CASE=firmware` 子用例删除：过期的 `rc=-19` 预期失败断言、case 表与 make 白名单一并收敛；其正面证明——xHCI 枚举 + MSC 发布 + GUID 选盘 usba 根 + 两个分区标记——并入 `readonly` 的成功证据链，并同步 `mk/components/run.mk` 的帮助文本与三处过期注释） | 2026-11-22 | [changelog](changelog.md#2026-11-22) · [build](build/build.md) · [driver](driver/driver.md) |
| **跨进程 inode 级 `MAP_SHARED`**（架构中立共享对象注册表 `kernel/memory/shared_backing.c`，键 `(挂载实例, inode)`、匿名对象 fork 继承；aarch64 mmap/fork/uaccess/拆卸全链接入；产品 E2E `USER_RUNTIME_CASE=map-shared` 实跑 `/bin/test_map_shared` 覆盖会合/偏移隔离/rename/unlink/权限上限/两种拆卸入口） | 2026-11-22 | [changelog](changelog.md#2026-11-22) · [cow-mmap](memory/cow-mmap.md#3-跨进程-inode-级-map_shared2026-11-22aarch64) · [arch.md](arch.md) |
| **x86_64 静态 PIE 与镜像基址随机化（ASLR-B）**（`x86_elf_pie_prepare()` 校验零链接 `ET_DYN`，逐页重定位跨页 `R_X86_64_RELATIVE`，PIE 偏移 `[0x00400000,0x10400000)` 抽取；PIE 偏移/`mmap_base`/`AT_RANDOM` 三段独立强熵、任一失败 fail-closed `-EAGAIN`；用户态与 BusyBox 全迁 `-fPIE` + `user-pie.ld`，`test-user-pie` 审计 47 产物） | 2026-11-22 | [changelog](changelog.md#2026-11-22) · [spec](superpowers/specs/2026-11-22-aslr-b-static-pie-design.md) · [cow-mmap](memory/cow-mmap.md) |
| **x86_64 用户态 `mmap_base` 随机化（ASLR-A）**（28-bit 4 KiB 页粒度窗口 `[0x0000400000000000,0x0000410000000000)` + 规范用户上界；`spawn_user_task()`/`sys_exec()` 初始化、fork 继承；无回绕 first-fit 间隙搜索封堵 `MAP_FIXED(0)`） | 2026-11-22 | [changelog](changelog.md#2026-11-22) · [spec](superpowers/specs/2026-11-21-aslr-a-mmap-base-design.md) · [cow-mmap](memory/cow-mmap.md) |
| **x86_64 内核 UBSan + KASan 插桩 v1**（`KERNEL_UBSAN=1`/`KERNEL_KASAN=1` 独立 knob + `san-ubsan`/`san-kasan` 变体，普通构建逐字节不变；KASan 两阶段影子 `shadow(VA)=(VA>>3)+0xdffffc0000000000` + slab 毒化钩子；UBSan 运行时 log+kpanic；`KERNEL_SANITIZER_TEST=ubsan\|kasan` 探针端到端验证） | 2026-11-22 | [changelog](changelog.md#2026-11-22) · [plan](superpowers/plans/2026-11-22-ubsan-kasan.md) · [build](build/build.md) |
| **GICv2 Phase 2 与正式 SError 入口**（EL1 VBAR `+0x180`/`+0x380`/`+0x580` SError 入口 → `aarch64_serror_fatal` 首错诊断 → `aarch64_serror_park`；`SecurityExtn` Non-secure Group 1 交接契约与 PMR 校验；1020 INTID 边界与 1020–1023 跳过；`gic_cpu_target_mask` 折叠与每核原子 claim；真机取证模板已备） | 2026-11-21 | [changelog](changelog.md#2026-11-21) · [arch.md](arch.md#aarch64-serror-与-gicv2-phase-22026-11-21) · [spec](superpowers/specs/2026-11-21-aarch64-gic-phase2-design.md) |
| **双架构统一异常诊断与单段内核栈回溯**（`kernel/core/trace.c` 统一 `TRACE_FULL`/`TRACE_PC_ONLY` 输出；`trace_symbols.c` 经链接器边界校验 PC 归属的 kallsyms 查找；`arch_unwind_read_frame()` 两字受保护读取 + 缺页修复例程；x86_64 用 TSS `rsp0` 定栈、aarch64 用 `aarch64_unwind_publish_task_stack()`） | 2026-11-21 | [changelog](changelog.md#2026-11-21) · [arch.md](arch.md#双架构统一异常诊断与单段栈回溯2026-11-21) · [spec](superpowers/specs/2026-11-21-multiarch-exception-trace-design.md) |
| **aarch64 运行期跨核用户任务迁移**（`pull_aarch64_user_task()` 拉取式负载均衡 + 每核 8-tick 空闲平衡；`aarch64_vm_edit_begin()` quiesce 静默校验（可回滚错误恢复原 mm/TLS、`-EBUSY` fail-stop）；地址序双 rq 锁内只做队列事务；`MODE=user-runtime USER_RUNTIME_CASE=migration` 验证 3 次往返） | 2026-11-21 | [changelog](changelog.md#2026-11-21) · [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [plan](superpowers/plans/2026-11-21-aarch64-runtime-task-migration.md) |
| **CPU 特性探测**（`arch/cpu_features.h`：`arch_cpu_features()` 返回统一位图 `ARCH_CPU_FEAT_{FPU,VIRT,CACHE_COHERENCY}`；x86 CPUID 与 aarch64 `ID_AA64PFR0_EL1` 双实现 + 宿主/内核自测） | 2026-11-21 | [changelog](changelog.md#2026-11-21) · [arch.md](arch.md#facade--override-矩阵) |
| **libm 基础代数集落地**（`usr/lib/libm.a` 独立发布，导出 16 个符号：`fabs/fabsf`、`copysign/copysignf`、`floor/floorf`、`ceil/ceilf`、`trunc/truncf`、`round/roundf`、`fmod/fmodf`、`sqrt/sqrtf`；来源 musl 1.2.5 vendored，全部整数位运算实现、无浮点算术指令；三层验证 = 宿主对拍 glibc、guest 实跑 `64_libm`、已发布归档反汇编审计） | 2026-11-21 | [changelog](changelog.md#2026-11-21) · [spec](superpowers/specs/2026-11-21-libm-design.md) · [libc](libc/libc.md#4-libm-基础代数集) |
| **aarch64 产品镜像迁 ext2 根文件系统**（产品变体从整盘 FAT32 superfloppy 迁到与 x86 相同的 mkdisk GPT 双分区布局：ESP→/boot、ext2→/ 承载产品用户态；/bin/sh 改真软链，同日跟进后 /bin 装全部解锁用户程序 29 项（x86 同款过滤器）；内核零改动，`fs_boot_mounts` GUID 路径直挂。整盘 FAT32 回退挂载在 aarch64 失去产品级覆盖，FAT32 读覆盖经 ESP→/boot 保留） | 2026-11-21 | [changelog](changelog.md#2026-11-21) · [plan](superpowers/plans/2026-11-21-aarch64-ext2-product-image.md) |
| **flake 账本入库**（账本文件 `config/flake-ledger.json` 入库：`runs` 机器写 + `decisions` 人写；报告器支持 `--merge`/`--update-ledger`/`--emit-fragment`；CI 夜间首次调用报告器并发布 `flake-ledger` artifact；契约门禁防回退。推翻 D8「无写回」，保留「不加权限」「不动 job 骨架」） | 2026-11-21 | [changelog](changelog.md#2026-11-21) · [spec](superpowers/specs/2026-11-21-flake-ledger-design.md) · [plan](superpowers/plans/2026-11-21-flake-ledger.md) · [testing-policy](testing/testing-policy.md#86-flake-账本) |
| **aarch64 运行期跨核任务迁移**（多核 `allowed_cpus`、`pull_aarch64_user_task` 拉取式负载均衡、8-tick 空闲平衡、`aarch64_vm_edit_begin` 静默校验、`USER_RUNTIME_CASE=migration`） | 2026-11-21 | [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [scheduler.md](sched/scheduler.md) · [plan](superpowers/plans/2026-11-21-aarch64-runtime-task-migration.md) |
| **用户态进入与返回门面**（`arch/context.h`：`arch_task_prepare_user_entry`、共享返回编排 `context_run_return_work`、`arch_context_restore_frame` 同址恢复） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [arch.md](arch.md#facade--override-矩阵) · [spec](superpowers/specs/2026-11-20-user-context-facade-design.md) · [plan](superpowers/plans/2026-11-20-user-context-facade.md) |
| **aarch64 EL3 分支标注仅 QEMU 支持**（`head.S` EL3→EL1 盲写 `SCR_EL3`/`SPSR_EL3` 边界化：仅 QEMU `-M virt,secure=on` 场景，真机 TF-A/UEFI 从 EL2/EL1 交接走 2a 分支，不受影响） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [arch.md](arch.md#aarch64-平台值解绑2026-11-18--2026-11-19) |
| **libc 统一 libk.a 内核子集与审计基线**（x86_64 / aarch64 同一份 32 对象 / 77 符号，审计基线架构中立） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [libc.md](libc/libc.md) |
| **libc aarch64 / x86_64 齐平**（`libc_arch` facade + per-arch override；setjmp 汇编 override、floatscan/strtod/vsscanf 经 float_abi 移植） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [libc.md](libc/libc.md) |
| **ELF 分段 NX 细化**（W^X 叶策略：`.data`/`.bss` RW+NX、`.rodata` RO+NX、仅 `.text` 可执行） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [cow-mmap.md](memory/cow-mmap.md#elf-分段叶子-nx-细化2026-11-20) |
| **aarch64 正式 EL0 IRQ 抢占、8-bit ASID 与 SMP 用户调度**（固定每任务单核亲和、`user-runtime` irq/asid/smp/product 1/2/4/8 核） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [plan](superpowers/plans/2026-11-20-intr-dispatch-remaining.md) |
| **aarch64 硬件 FP/SIMD 放行**（CPACR_EL1.FPEN + per-task 保存恢复；当时的剩余项 PSTATE F 位与 signal 帧 FP 区已于 2026-11-21 闭环，见 [changelog](changelog.md#2026-11-21)） | 2026-11-20 | [changelog](changelog.md#2026-11-20) |
| **损伤区域与批量 present**（`GFX_PRESENT_RECTS` + terminal / LVGL 双消费端） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [gui.md](gui/gui.md) · [spec](superpowers/specs/2026-11-19-gfx-damage-batch-present-design.md) |

---

## 相关文档

| 文档 | 内容 |
|------|------|
| `docs/README.md` | 文档索引（按 0-4 层组织，按阅读顺序排列） |
| `docs/architecture.md` | 启动链、内存布局、初始化序列（全局骨架图） |
| `docs/boot/boot.md` | UEFI 引导 + `boot_context` ABI + E820 拆分 + 启动编排治理（BOOT-1~5） |
| `docs/arch.md` | Multi-arch weak-default + strong-override 总览（facade/override 矩阵、页表层级、bootinfo ABI、驱动 initcall、IRQ hook 3 段、RTC 拆分、启动阶段契约、aarch64 用户态现状、距离单一 `kernel_main`） |
| `docs/arch/cross-boundary-symbols.md` | 跨边界符号 / ABI 边界规范（AAGU-4）—— builtin、UAPI、arch-value、libc 镜像 4 类规则 + 现状对照表 |
| `docs/arch/entropy-source-facade.md` | arch-neutral entropy facade spec（AAGU-5.3 / .5）—— STRONG/WEAK/NONE 三档质量标签 + 控制流决策 |
| `docs/memory/memory.md` + `docs/memory/cow-mmap.md` | 物理/虚拟内存管理 + COW fork/mmap + PMM arch-neutral + 页表层级统一 + aarch64 COW 与匿名 VMA + 内存子系统架构边界 + x86 NX 按 VMA 策略强制执行 |
| `docs/interrupt/interrupt.md` | 中断处理（do_IRQ、register_irq、IDT、共同事件分派） |
| `docs/smp/smp.md` | SMP 架构 + 负载均衡实施总结 |
| `docs/sched/scheduler.md` + `docs/sched/scheduler-complexity.md` | EEVDF 调度器设计、复杂度评估与上下文门面抽象 |
| `docs/syscall/syscall.md` | syscall 表 + 用户指针边界语义 + syscall 边界审计触达清单 + 跨架构共享 syscall 表 |
| `docs/signal/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/time/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结 |
| `docs/gui/gui.md` | Tetris、Material 桌面、libgui 平台后端与原生控件统一焦点体系 |
| `docs/fs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/net/network.md` + `docs/archive/lwip-debugging-experience.md` | lwIP 网络栈 + 正确性加固 + init PID 1 延迟启动 |
| `docs/fs/filesystem.md` | VFS 注册表、ops 分离、元数据、挂载树、dcache、FAT32、ext2（含双/三级间接块映射）、devfs、procfs、tmpfs、GPT、block device |
| `docs/driver/driver.md` | 驱动子系统（keyboard, serial, ahci, pci, e1000, virtio-net, fb, mouse, gfx0, logo, xhci, usb_devfs）+ 帧缓冲 cache policy 与映射事务 + USB 大容量存储 MSC/BOT + USB HID 引导输入 + xHCI Hub/Scratchpad/PSIC |
| `docs/subsys/subsys.md` | 子系统注册框架（`SUBSYS_INITCALL()` + 阶段顺序 + BOOT-3 失败策略 / BOOT-4 阶段契约与依赖拓扑） |
| `docs/libc/libc.md` | 用户态 libc：接口覆盖 / 语义要点 / 已知限制（含 T1~T17 落地状态表） |
| `docs/games/doom.md` | Doom 移植：默认构建行为 / IWAD 打包 / 运行参数 / 按键 / 诊断构建 / 引擎许可与来源材料 |
| [启动顺序与阶段抽象分析](superpowers/reports/2026-11-07-boot-sequence-analysis.md) | 静态证据、CPU/IRQ/失败传播风险及 BOOT-1～5 spec 拆分依据 |
| `docs/log/log.md` | 日志级别、DEBUG_CHANNELS、LOG_TARGET、NDEBUG |
| `docs/build/build.md` + `docs/build/toolchain.md` | 构建系统手册（profile 构建体系、端到端运行调试、构建/测试 harness 权威 taxonomy 与 6 bucket 目标）+ 工具链指南 |
| `docs/testing/testing-policy.md` + `docs/testing/audit-dispositions.md` | 测试分层规范、场景归属与覆盖策略 + 生成式测试目录与派发表（§8）+ 静态审计处置台账 |
| `docs/debug/debug.md` | 调试通道与历史调查闭环索引 |
| `docs/archive/applet-verification.md` | busybox applet 验证清单 |
| `docs/decisions.md` | 关键设计决策总账（含开源 OS 借鉴对照表） |
| `docs/structure.md` | 目录结构逐目录说明 |
| `docs/changelog.md` | **已完成工作历史**（按时间倒序）——roadmap 中已闭环项的完整记录 |
| `docs/superpowers/specs/` + `docs/superpowers/plans/` | 历史设计 spec + 实施 plan（过程档案） |
| `docs/superpowers/reports/` | 历史里程碑验收与闭环验证证据报告 |
| `docs/archive/aarch64/` | aarch64 专题 closure 与 handoff 报告 |

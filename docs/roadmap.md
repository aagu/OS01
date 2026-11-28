# OS01 优化路线图

> **基准**：`b44e1825`（master，2026-11-27）　**日期**：2026-11-27

本文件**只列未完成 / 进行中**的规划项。已完成工作见 [`changelog.md`](changelog.md)；**最近 2 天的闭环快速索引见文末「近期闭环索引」**；闭环细节见各专题文档：
[`arch.md`](arch.md) · [`memory/memory.md`](memory/memory.md) · [`memory/cow-mmap.md`](memory/cow-mmap.md) ·
[`gui/gui.md`](gui/gui.md) · [`driver/driver.md`](driver/driver.md) · [`testing/testing-policy.md`](testing/testing-policy.md) ·
[`libc/libc.md`](libc/libc.md) · [`signal/signal.md`](signal/signal.md) · [`decisions.md`](decisions.md)。

## 待办总览

| 优先级 | 主题 | 未完成项 | 备注 |
|-------|------|---------|------|
| **P0** | 工程与测试基础设施 | 0 | flake 账本入库已于 2026-11-21 闭环（见 [`changelog.md`](changelog.md)） |
| **P1** | 安全加固 | 1 | UBSan/KASan 后续项（v1 已于 2026-11-22 闭环）；ASLR 全覆盖（x86_64 与 aarch64 的 `mmap_base`、栈基址与静态 PIE 均已于 2026-11-26 闭环） |
| **P2** | aarch64 适配 | 8 | 内存与执行体 0 · 能力齐平 4（PCIe ECAM/INTx/GICv2m/AHCI 与 GUI sysroot 已于 2026-11-27 闭环，剩网络/输入/E2E/PCIe 演进） · 架构统一 0 · 真机阻断 3（硬阻断 1 + 条件阻断 2；GICv3 已于 2026-11-27 在 Dragon Q8B 真机验证通过）· 真机推进待办 1（RPi5 与 Radxa Dragon Q8B 真机均已跑到 boot-3 fs/root，剩根文件系统） |
| **P2** | 架构治理 / 扩展性 | 0 | 新增治理项沿 AAGU 编号追加，先记入 [`decisions.md`](decisions.md) 与 [`changelog.md`](changelog.md)，再回填该节 |
| **P3** | GUI | 7 | GUI-4/5/6 + 优化与验证 4 |
| **P4** | 硬件适配 | 5 | USB 2 · NVMe · HPET · ACPI S3 |
| **P5** | ABI 扩展 / 兼容性 | 12 | syscall 与 libc 缺口（`readv/writev`、`openat/dup3/pipe2` 已于 2026-11-26 闭环） |
| **横跨 P0–P5** | [其他 OS 设计借鉴](#其他-os-设计借鉴) | 5 | 独立计数，优先试做 ext2 宿主集成测试与 init 重启限流 |

推进顺序：**P0 → P1 → P2 → P3 / P4 → P5**；P3 GUI 与 P4 硬件可并行。

---

## 🔒 P1 安全加固

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| UBSan + KASan 后续项 | x86_64 内核插桩 v1 已闭环（[`build.md`](build/build.md)）。**剩余四项**：① 栈插桩（需 Linux 式早期文件排除清单）；② 全局插桩 / 全局红 zone（需 `.init_array.1` 扫描）；③ aarch64 接入（UBSan 运行时本身架构中立，但两个 knob 当前都门禁在 x86_64 profile，`mk/project.mk:472-473` 的定义与 `:523-525` 的 profile 门禁）；④ 偶发 LAPIC 影子 `0xff` 待诊断（报告已带影子帧 PA） | v1 ✅ | ArvernOS |

## 🏗 P2 aarch64 适配

### 能力齐平：对标 x86_64 的剩余缺口（2026-11-21 扫描，2026-11-27 复核）

范围：能力门（`kernel/include/arch/features.h`）+ 共享 syscall 表（`kernel/syscall/dispatch.c`）+ 驱动/设备注册面 + userland 应用与 E2E 套件的全量对比。差距呈「外设与生态 > syscall > 执行体细节」梯度：执行体核心（fork/exec/信号/SMP 用户调度）已收口，本节只记**此前未立任务的缺口**。头部事实：`OS01_HAS_PCI` 在 aarch64 已为 `1`（QEMU `virt` ECAM + `interrupt-map` INTx + `GICv2m` MSI-X + `pci_iomap_range()` + `ahci` 已于 2026-11-27 闭环），`OS01_HAS_NET` / `OS01_HAS_USB` 在 aarch64 仍为 `0`；共享 syscall 表 x86_64 服务 **80** 项 entry（其中 `SYS_getpeername` handler 为 0、实际不可达 **79** 项）、aarch64 产品态 **66** 项（15 共享原生 + 33 fs 组 + 6 misc/msync + 4 time 组 + 5 proc 组 + 2 rlimit + `futex` 1），缺 **14** 项 = 网络 13 · `arch_prctl` 1（会话/资源/putchar 8 项与 `futex(2)` 已于 2026-11-26 闭环）。`arch_prctl` 是 2026-11-24 x86_64 静态 TLS 落地时新增的缺口（`SYS_arch_prctl` = 79，compat 表 158→79）；aarch64 用 TPIDR_EL0、不需要 `ARCH_SET_FS`，故优先级最低，仅登记不单独立项。

| 任务 | 内容与完成条件 | 前置 |
|------|----------------|------|
| 网络栈接入（`OS01_HAS_NET` 0→1） | 三段：① lwIP 纳入 aarch64 profile 构建（现 aarch64 不编译 lwIP，`kernel/arch/aarch64/make.config:46` `ARCH_ENABLE_LWIP = 0`，x86 同文件 `:46` = 1）；② virtio-net 传输层——现驱动是 x86 PCI legacy IO-port 传输（`kernel/driver/virtio-net.c:1-4`，内联 `inl/outl`），aarch64 需参照 virtio-blk-mmio（`kernel/driver/virtio_blk.c` + `kernel/include/arch/virtio_mmio.h`）实现 MMIO transport，或先抽公共 virtio 设备核心再两架构分接；③ 翻门解锁 `kernel/fs/file.c` 的 4 处 FD_SOCKET 块（`#if OS01_HAS_NET`：`:31-43` / `:183` / `:947` / `:1295`）并解除 13 项 socket syscall 隐藏（`kernel/syscall/dispatch.c:129-141`）；`kernel/fs/poll.c:328-350` 的 FD_SOCKET 分支**无编译门**，仅因 socket syscall 隐藏而运行时不可达，无需翻门。验收 = aarch64 上 `SUITE=network` 等价 E2E 跑通 | 独立；E2E 依赖本节「E2E 套件 aarch64 化」 |
| 输入设备（键盘/鼠标） | aarch64 唯一输入是 PL011 RX → console TTY（`kernel/arch/aarch64/platform/console.c:37-42`），无 i8042/PS2、无 xHCI HID（`OS01_HAS_USB`=0）→ 无键盘/鼠标。**消费面分层解耦前置已完成（2026-11-22，`376c00d2`）**：`kernel/driver/input_route.c` 成为物理输入的独占租约与唯一权威（物理键态、Caps 锁存、NORMAL/ACTIVE 双路路由），消费端 `kernel/driver/keyboard.c` 与 `kernel/driver/mouse.c` 只读 route 快照，i8042 demux 与 PS/2 生产者下沉到 `kernel/arch/x86_64/driver/{i8042.c,ps2_kbd.c,ps2_mouse.c}`，x86 多生产者经 `SUBSYS_INITCALL` 仲裁。**仍缺**：QEMU virt 路线需新增 virtio-input MMIO 驱动（全树无 virtio_input 代码）作生产者接入 route 与 /dev/keyboard、/dev/mouse；真机/PCI USB HID 需先推进 xHCI 跨架构与 `OS01_HAS_USB=1`。P3 GUI「aarch64 GUI 等待显示设备基础就绪」中输入是缺口的一半 | 独立 |
| PCIe 总线二期与设备生态 | 一期（QEMU `virt` ECAM + `interrupt-map` INTx + `GICv2m` MSI-X + `pci_iomap_range()` + `ahci`）已于 2026-11-27 闭环。**剩余**：① Type 1 PCIe 桥（`pcie-root-port`）下游设备 INTx swizzle 路由；② RPi5 `brcm,bcm2712-pcie` RC 间接配置访问与 Outbound ATU（接管 RP1）；③ Dragon Q8B ACPI `MCFG` 发现与 ECAM 接入；④ `xhci` / `virtio-pci` 等更多 PCIe 设备驱动跨架构接入与 `DMA`/`dma-ranges` 翻译 | PCIe 一期 ✅ |
| E2E 套件 aarch64 化 | `test-qemu` 的 **25 个 x86-only E2E 套件**（systest/network/gfx/terminal-present/gui/usb-boot 等；`mk/generated/test-dispatch.mk:2` 的 `TEST_QEMU_SUITE_IDS` 共 41 项，其余 16 项为 `aarch64-*`）全部 x86-only；aarch64 侧有 `make test-aarch64 MODE=<...>`（**共 20 种 MODE**）+ `test-aarch64-pcie`（`ecam-virt` / `ahci-virt` / `no-ecam`）+ `SUITE=aarch64-*` 16 项 + `test-contract`（`PROFILE=aarch64-clang`）。**`systest` 已于 2026-11-27 接入 aarch64**（UP `--cpus 1` 下 `total=122 passed=121 failed=0 skipped=1`；SMP `--cpus 2` 下 `poll` 阻塞唤醒偶发挂起待后续排查）；其余套件（terminal-present 等）随 syscall/驱动组解禁逐套件跟进 | 无硬前置；个别用例依赖上面 syscall 组先行解禁 |

### 真机适配：树莓派 5 & Radxa Dragon Q8B 剩余阻断

两块真机均已打通早期内核路径（串口、HDMI logo、M0/M1、DTB/ACPI、GICv2/GICv3、Timer、CSPRNG，详见 [`changelog.md`](changelog.md#2026-11-27) 与 [RPi5 handoff](aarch64-rpi5-bringup-handoff-2026-11-25.md)），当前共同停在 `boot-3` `FATAL stage=fs component=root rc=-19`（`-ENODEV`）。

**硬阻断（无法进入根文件系统 / 用户态）**：

| 问题 | 位置 | 真机影响 | 状态 / 剩余解法 |
|------|------|----------|----------|
| **根文件系统无块设备**（Dragon Q8B 当前唯一阻断；RPi5 已打通） | `kernel/fs/boot.c:92` `mount_disk_by_guid()` 在 `block_device_count() <= 0` 时返回 `-ENODEV`；Dragon Q8B 无 NVMe/USB 块驱动。RPi5 RP1 xHCI + USB MSC 已于 2026-11-28 在真机实测通过（`rp1-usb0`/`rp1-usb1`、`0951:1665` `usba`、`root=usba2 boot=usba1`、`SHOOTDOWN-PROBE: OK` 并拉起 `pid=1`，[aarch64-usb-boot-qualification.md](driver/aarch64-usb-boot-qualification.md)） | Dragon Q8B 停在 boot-3 `stage=fs component=root rc=-19`；RPi5 已跨过根盘挂载并进入用户态 `init` | ❌ **Dragon Q8B 仍开放**（待存储或内存根方案）；✅ **RPi5 USB 根盘挂载已实测打通** |
| **Cortex-A76（RPi5）ARMv8.1-PAN 用户态写权限异常** | `kernel/arch/aarch64/syscall/sys_misc.c:53-75`（`SYS_getrandom` 原在 `uaccess_user_write_begin()` 后直接把用户地址传给 `get_random_bytes()` → `memcpy()`；已改为 `syscall_prepare_user_write()` + 内核栈 256B 缓冲分块生成 + `copy_to_user_ft()`/`STTRB` 回填） | RPi5（Cortex-A76）拉起 `init`（`pid=1`）后首个 `getrandom` 曾触发 Data Abort 停机：`ec=0x25 esr=0x9600004f`（level-3 Permission fault）、`spsr=0x204003c5`（bit 22 `PAN=1`）、`far=0x07af57c0`；QEMU `cortex-a53`（ARMv8.0-A）无 PAN 硬件特性故未复现 | ✅ **已闭环（2026-11-28 RPi5 真机实测通过）**（`SYS_getrandom` 已改为经内核缓冲 + `copy_to_user_ft()` 回填；QEMU `cortex-a53` systest `121/122` 全过、`cortex-a76` `getrandom` 与 `43_ssp_guard_seeded` 通过；RPi5 真机成功拉起 `init`（`pid=1`）→ `/bin/terminal`（`pid=2`）→ `BusyBox v1.36.1 ash`（`pid=3`，提示符 `# `），日志见 [`serial-run4.log`](../test-results/rpi5-usb-boot/serial-run4.log)） |

（注：`GICv3 驱动缺失` 已于 2026-11-27 通过 ACPI + `gic_driver.c`/`gic.c`/`ipi.c` 落地并在 Dragon Q8B 实机跑通 `intids=992`；DTB 路径的 `arm,gic-v3` 节点解析与 GICv3 多核 SMP IPI 实测待后续补齐。）

**条件阻断（能到早期启动，特定环节卡死/静默失效）**：

| 问题 | 位置 | 真机影响 | 状态 / 剩余解法 |
|------|------|----------|----------|
| GICv2 Group 0 / Non-secure Group 1 交接 | `kernel/arch/aarch64/intr/gic_driver.c` | 无 Security Extensions 时配置 Group 0；有 `SecurityExtn` 时假定固件已为 OS-owned SPI 及各核 SGI/PPI 配置 Group 1 与 NS-accessible PMR（跳过 IGROUPR 并校验 PMR 读回非零） | ✅ **软件侧已就绪**（2026-11-21）；**RPi5 真机部分取证已过**（GIC-400 上 `[gic] GICv2 driver: intids=320` 初始化成功，clocksource/timer/timekeeping 子系统就绪，`CSPRNG: seeded STRONG from UEFI GetRNG`）；**仍待**：TF-A Group 1 实测 + PMR 读回审计 + 实机 IRQ 覆盖（CNTP / 定向 SGI / PL011 RX→TTY）与物理 SError 注入，按 [`arch.md`](arch.md#aarch64-serror-与-gicv2-phase-22026-11-21) 的板级证据模板取证 |
| 非 BSP CPU 强制 enable-method=psci | `kernel/arch/aarch64/platform/dtb_parse.c:161`（`exact(n->enable,"psci")`）、`:467`（非 BSP 无 psci → -3 FATAL） | spin-table 平台（老 RPi 固件、`cpu-release-addr`）全部 AP 拒启（-3 FATAL） | ❌ **仍开放**（RPi5 走 PSCI，不阻断本板）：若目标板需要，加 spin-table 等待路径 |

（注：`timer` 中断形状校验偏脆已于 2026-11-26 闭环，`dtb_parse.c` 改为扫描所有 3-cell 项匹配 `GIC_PPI(1)` + `14`（INTID 30），不再要求固定在第二槽位。）

**真机推进待办（不阻断 rootfs 之前的板级能力，但真机验收必须补齐）**：

| 项 | 内容 | 状态 |
|----|------|------|
| 4 核 SMP / AP 启动实测 | RPi5 4 核 PSCI SMP 与 `SHOOTDOWN-PROBE: OK`（经 `config.txt` `platform rpi5` 传递 `0x2000000000` 探测 PA）已于 2026-11-28 在真机实测通过：`requested=4 online=4 status=PASS`、`spinlock`、`clocksource`、`cntp`、`IRQ` 均通过 | ✅ **RPi5 4 核 SMP 已实测通过** |
| RPi5 显示分辨率与串口 RX | 真机 UEFI GOP 仅由 TF 卡 VideoCore `config.txt` 控制（实测 `[logo] boot logo drawn (aarch64, 1920x1080)`）；RP1 MSI-X + BCM2712 MIP 控制台 RX 中断通路已落地并通过宿主/QEMU 回归（`docs/testing/aarch64-rp1-irq.md`），真机板测待执行 | ⚠️ **已知边界 / 真机待测**（分辨率需改 TF 卡 VideoCore `config.txt`；RP1 串口 RX 中断待上板跑 `tools/tests/aarch64_rp1_irq_board.py` 实测） |

**平台能力上限（非阻断，选型需知）**：PA 上限 40-bit/1 TiB（`kernel/arch/aarch64/head.S:252-268` TCR IPS=0b010、`kernel/arch/aarch64/memory/page_table.c:51`，未实现 `ID_AA64MMFR0_EL1.PARange` 动态探测）；早期 identity map 默认 [0x0, 0x80000000)，`AARCH64_BOOT_BLOCK_BASE >= 0x80000000` 时扩展为 [0x0, 0xC0000000)。

**剩余解绑顺序**：**RPi5 / Dragon Q8B** → 根文件系统（RAM 根 / 板载存储 / UEFI Block I/O 三选一）→ 多核 SMP（4 核 / 8 核）→ 存储 / PCIe / 网络 →
补真机测试；**其他 DTB 平台** → 补 Group-1 / spin-table / DTB `arm,gic-v3` 解析 → 实机启动验证。

## 🖥 P3 GUI

### 后续功能任务（按依赖推进）

| 项 | 内容 / 完成条件 | 依赖 |
|----|-----------------|------|
| GUI-4 字体与资源 | 先使用裁剪的抗锯齿位图字体与编译内置图标，覆盖桌面所需字号；单列中文字符集、字库容量与输入方案。按需再引入动态字体或文件资源加载，并审计依赖、许可证及缓存开销 | GUI-1 ✅、GUI-2 ✅、GUI-3 ✅、GUI-2b ✅；**GUI/LVGL 栈当前字体仍为内置 `lv_font_montserrat_{12,14,16,20,24}`**（`config/lv_conf.h:659-665`，CJK 源全关 `:682-683`，默认 `lv_font_montserrat_14` `:701`；字形集为 ASCII + U+00B0/U+2022 + FontAwesome 图标），无中文字形、无自建裁剪位图字体；**构建产物实为 6 个字体对象**——`lv_font_montserrat_14_aligned` 也被编入（其自有宏默认 1，`thirdpart/lvgl/src/font/lv_font_montserrat_14_aligned.c:25-27`，bpp 8/stride 16，字形集相同）；**唯一自建位图资源是图标而非字体**（`libgui/assets/cursor-arrow.png` → `libgui/src/cursor_icon.c`，16×24 ARGB8888，`tools/gen_cursor_icon.py`），即「编译内置图标」已部分落地；**terminal 侧另有一份内置 PSF2 位图字体 `user/terminal/terminal_font.psf`（objcopy 内嵌），与桌面字库无关**，不可据此认为 GUI-4 已部分落地 |
| GUI-5 Window Server + compositor | 单独设计 surface、IPC、缓冲共享、损伤区域、Z-order、移动/缩放、遮挡、焦点/输入抓取与进程退出回收；可信窗口服务器统一呈现，收紧 `/dev/fb` 与重叠 gfx view 的旁路访问。IPC/共享内存能力先评估，缺项独立补齐；客户端允许使用 LVGL 或其他绘制库 | GUI-1 ✅、GUI-2 ✅、GUI-2b ✅；IPC/共享缓冲与图形权限设计；借鉴 opuntiaOS/HackOS |
| GUI-6 terminal 窗口化 | 保留 PTY/VT100 核心，将渲染输出接入窗口 surface；键盘只投递到焦点终端，验证 Ctrl-C、alt-screen、窗口关闭和 shell 存活；缩放时更新 PTY winsize 并接通 SIGWINCH | GUI-5；terminal/PTY ✅；SIGWINCH ✅（PTY master `TIOCSWINSZ` 尺寸变化时派发到前台进程组，`kernel/tty/pty.c:149`；`qemutests/test_resolution_switcher.py` 的 `pty-watch` 场景已验证前台进程收到信号并读到四个 winsize 字段） |

顺序：**GUI-4** → GUI-5 → GUI-6；GUI-5/6 是独立系统工程，不与首版单进程桌面捆绑；aarch64 GUI 等待显示设备基础就绪。

### 优化与验证项

| 项 | 当前边界 / 后续工作 | 验收依据 |
|----|---------------------|----------|
| 帧耗时与呈现能力 | 分别测量绘制、复制、内核提交和等待开销，覆盖多分辨率及实际阴影/滚动/动画场景；按瓶颈优化。vsync、page flip、write-combining 或 GPU 后端另行设计；当前私有缓冲不保证无撕裂 | 记录帧耗时分布、CPU 占用与上传量；优化前后使用同一场景比较。**仍缺**：跨分辨率的帧耗时分布统计与统一帧时序子系统——现有计时是诊断级（内核 present 统计 `kernel/driver/gfx.c:355`、present A/B 诊断 ioctl `GFX_TEST_PRESENT_AB`、Tetris 的 `elapsed_ns`、terminal `[term-present]` 的 `begin_ns/end_ns/success_ns` 上报），全树无 frame-timing 符号；`user/terminal/terminal.c:33` 的 `FRAME_INTERVAL_MS 33` 是**节流**常量而非计时测量，`config/lv_conf.h:1117` 的 LVGL FPS 监视器关闭 |
| 内存池与资源生命周期 | 评估 4 MiB TLSF 池是否适合桌面；大型绘制缓冲使用 mmap，预算包含字体、图片、图层与多窗口 surface。测试反复创建/销毁、低内存和池耗尽，明确失败处理 | 内存占用有上限；长期运行无持续增长；耗尽不崩溃或破坏状态。**当前边界**：4 MiB 池定义在 `config/lv_conf.h:72`（`LV_MEM_SIZE`），但实体是**每进程静态 BSS 数组**（`thirdpart/lvgl/src/stdlib/builtin/lv_mem_core_builtin.c:80-83` 的 `work_mem_int[]` → `lv_tlsf_create_with_pool`），GUI 进程数增加即是实打实的预算问题；大型绘制缓冲已走 mmap（`libgui/src/platform.c:349-350` draw buffer、`:358-359` 离屏 shadow canvas，释放 `:436-438,515-517`）；已有内存**统计**接口 `gui_platform_stats.pool_total/pool_used/pool_max_used`（`libgui/include/gui/gui_platform.h:134-136`，取自 `libgui/src/platform.c:613-618` 的 `lv_mem_monitor`），但**资源生命周期管理完全缺失**（无字体/图片加载器、无 layer/surface 对象、无引用计数；LVGL 全部图片解码器关闭：`config/lv_conf.h:986,989,992,1000,1003,1006`），反复创建/销毁、低内存、池耗尽测试均未做 |
| libc / 可选 LVGL 能力 | **剩余**：① `lseek` 归位到 `libc/include/unistd.h`（现仅声明在 `libc/include/sys/stat.h:145`，实现 `libc/unistd/lseek.c:13`）；② pthread 后端（现仅 `libc/pthread/mutex.c` 的非递归 mutex 与 `libc/include/pthread.h:10-14` 的五个 mutex 函数，缺线程、条件变量、信号量与递归 mutex）；③ FreeType/ThorVG/图片解码库逐项审计（LVGL 自带副本全在 `config/lv_conf.h` 关闭：`:1028` FREETYPE、`:1039` TINY_TTF、`:1078` SVG、`:986` LODEPNG、`:989` LIBPNG、`:992` BMP）。**libm 基础代数集已落地**：`usr/lib/libm.a` 独立发布，导出 16 个符号（`fabs/fabsf`、`copysign/copysignf`、`floor/floorf`、`ceil/ceilf`、`trunc/truncf`、`round/roundf`、`fmod/fmodf`、`sqrt/sqrtf`），来源为 musl 1.2.5（见 `thirdpart/musl-libc-algorithms.manifest`）；全部用整数位运算实现，无浮点算术指令。**剩余范围**：三角/对数/幂等超越函数（`libc/include/math.h:183-198` 只声明那 16 个函数，连 `sin`/`cos`/`tan`/`log`/`exp`/`pow` 的声明都没有）、`long double` 变体、复数与 `fenv`（异常标志，`libc/include/` 下无 `fenv.h`/`complex.h`/`tgmath.h`，`math.h:19-23` 明示"the libc has no fenv"）均未实现；内核不链接 libm，保持 FP-free | 每次配置扩展重新验证构建、符号和运行时；不让可选依赖阻塞基础桌面。**libm 验收依据**：`make PROFILE=x86_64-clang test-libc-api-audit` 断言 16 个符号只由 libm.a 提供（libc.a/libk.a 均不定义）；`make PROFILE=<p> test-libm-asm` 审计已发布归档的反汇编，无浮点算术/FMA/取整/转换指令；宿主 `test_libc_math` 与 guest `64_libm` 分别对拍宿主 glibc 与实跑 guest |
| 构建与版本维护 | 持续验证无修改构建、配置/头文件变更重编、staging 孤立文件清除及已发布 sysroot 编译；审计实际 archive 与当前对象清单一致，删除源码时重建归档。保留固定版本，升级单独回归 | 增量结果与干净构建一致；无陈旧归档成员；符号审计与 GUI 回归通过。**现有覆盖**：`make test-gui-incremental`（`mk/components/run.mk:1811-1904`）已覆盖**五**个子项（无修改构建不重编、touch 公共头重编依赖者、install 清孤儿头文件、用**已发布 sysroot** 编译链接一个真实 target ELF、发布态 PS2 头改动触发 generation rekey 并重编消费者），配 `qemutests/test_build_incremental.py`；符号审计 `test-libc-api-audit` / `test-libm-asm`；版本固定走 `thirdpart/*.manifest`（busybox / doomgeneric / lvgl / musl-libc-algorithms / posix-uefi——**mbedtls 无 manifest**）。**缺口**：「删除源码时重建归档」只对 `libm.a` 成立（`libc/Makefile:139-146` 先 `rm -f $@` 再 `ar rcs`），而 `libc.a` / `libk.a` 直接 `ar rcs` 不删包（`libc/Makefile:131-137`）——删源码后旧成员会残留；且**没有任何目标断言「归档成员 == 当前对象清单」**（仅 mbedtls 有对应回归 `qemutests/test_build_incremental.py:88`） |

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
| 动态链接器 | PT_INTERP + ld.so + 共享 libc。**T1 exec 通路已闭环（2026-11-28，仅 x86_64）**：ET_DYN 主程序带 `PT_INTERP` 时 exec 读路径（≤256 B、NUL 结尾、`/` 开头）、经 VFS 打开、按既有静态 PIE 形约束校验并以独立 ASLR-B bias 装载为第二镜像（占页冲突 `-EEXIST` 重采样 ≤16 次，耗尽 `-ENOMEM`）；auxv `AT_BASE`=interp bias（静态 PIE 仍 0，auxv 字节不变），`AT_ENTRY`/`AT_PHDR` 仍指主程序，初始 RIP 归 interp、骨架经 `AT_ENTRY` 跳回主程序；ET_EXEC 带 PT_INTERP 由静默忽略改为显式拒绝（`kernel/arch/x86_64/fs/elf.c`、`elf_pie.c` 放行单 PT_INTERP）。解释器专用校验：被装载为解释器的镜像必须 ET_DYN，自身 PT_INTERP（嵌套）与 PT_TLS 一律拒绝（`elf_load_at` interp_mode）。运行证据：systest `exec_interp`（fork/exec `/bin/dyn_exec` → 串口 `OS01_LDSO` → `OS01_DYNMAIN` → waitpid rc=0；主程序另校验解释器在 auxv `AT_NULL` 后写入的握手对，RIP 被直送主入口时用例必败），宿主 `test_elf_load_4k` 新增 9 个 PT_INTERP 用例、`test_brk_pages` 新增堆增长拒绝外页用例（brk 增长遇装载器外页整体 `-ENOMEM` 拒绝，边界与外页保持原值），形状门 `tools/tests/check_interp_fixture.py`（PT_INTERP 串、DT 白名单逐条严格解析、DF_1_PIE、RELATIVE-only）。**T2 ld.so 符号重定位已闭环（2026-11-28，仅 x86_64，spec/plan [`docs/superpowers/specs/2026-11-28-ldso-reloc-x86-design.md`](superpowers/specs/2026-11-28-ldso-reloc-x86-design.md)**）：内核 DT 白名单对 interp 主镜像放宽 5 tag、RELA 容忍 GLOB_DAT/JUMP_SLOT（addend==0、校验不应用）、RELACOUNT 前缀语义、RELATIVE 写守卫（禁写区=新增动态元数据+GOT[0..2]）、SYMTAB/HASH 位置规则（带 NEEDED/符号型/非空 JMPREL 须 DT_HASH 且两表落非可写 LOAD）、exec 层 ABI 门槛（interp 主镜像仅 native ABI，GNU-note `-ENOEXEC`）；`user/ldso/` 真 ld.so（替换 T1 骨架、marker/握手对/exit 91 保留）：DT_NEEDED 装载（anon mmap+read 灌段+用户态预触 PTE+逐段 mprotect）、SYSV hash 符号解析（SHN_ABS 不加 bias）、GLOB_DAT/JUMP_SLOT 写 S；夹具 dyn_link 四变体（rc=0/95/126/98）+ 形状门 8 参数 + 串口窗口门（protocol BEGIN/terminal 定界）；干净构建 systest 138 用例全绿。**已知债务**：段间空洞页 anon RW（T3 file-backed 消失）、SHN_ABS 公式无自动化覆盖、gnuabi+compat 任务的解释器 ABI 选择未裁决。**剩余**：T3 共享 libc（libc.so 进 sysroot + 动态链接构建开关 + 上述三项债务清偿）；T4 aarch64 对齐（`kernel/arch/aarch64/memory/user_elf.c` 仍拒绝 `PT_INTERP`/`PT_DYNAMIC`） | ELF ✅（`PT_TLS` 静态 TLS 已由 x86 私有校验器接管，2026-11-24） | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构；**现状**：Linux 兼容层的 `rt_sigaction` 只是转发到旧 `SYS_signal` 的 ABI shim（`kernel/syscall/compat/proc.c:11-19`），无 SA_RESTART / 实时信号 | |
| x86_64 signal 帧 FPU 区 | x86_64 `struct sigframe` 同样无 FPU 区（`kernel/include/uapi/time.h:95-106`，仅 200 B 通用寄存器 + blocked 掩码），signal handler 的 FP 使用会毁掉被打断上下文的 x87/SSE 状态。aarch64 侧已闭环（816 B 帧含 `fp[528]@288`，2026-11-21，见 [`arch.md`](arch.md) 与 [`changelog.md`](changelog.md)），本项为 x86_64 对齐任务 | aarch64 FP 帧实现 ✅（2026-11-21） | |
| clone/pthread | 线程模型 + pthread_create | futex ✅；**x86_64 线程上下文前置已闭环（2026-11-24）**：ELF `PT_TLS`（Variant II）静态 TLS 在 spawn / exec 初始化、`%fs:0` 发布 TP、`arch_prctl`(79) 原生实现 + Linux 158→79 兼容映射、`IA32_FS_BASE` 在上下文切换中保存/恢复、fork 快照调用者 selector + base（`kernel/arch/x86_64/fs/elf.c`、`kernel/arch/x86_64/cpu/switch.c`、`kernel/arch/x86_64/syscall/sys_misc.c`、`kernel/arch/x86_64/sched/fork.c`）。**仍缺**：`kernel/include/uapi/syscall.h` 无 `SYS_clone`（Linux nr 56 在兼容表里直接映射到 `SYS_fork`，`kernel/syscall/compat/table_x86_64.c:43`），`libc/include/pthread.h:10-14` 只声明 mutex 一族，无 `pthread_create` / 线程 / 条件变量 / 信号量 / 递归 mutex；aarch64 侧 `futex` 已于 2026-11-26 闭环 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立；ARCH-5 已就绪 ✅（当前仅有 pipe 的合成 stat `S_IFIFO`，`kernel/syscall/sys_fs.c:565`，**无 mkfifo**） | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立；**现状**：`libc/unistd/alarm.c:2` 是静默空桩（无 syscall），`setitimer`/`getitimer` 完全未实现 | |
| 作业控制（剩余部分） | **剩余**：SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs，当前仅作 ignore 处理，无 stop/continue 状态机——`TASK_STOPPED` 定义于 `kernel/include/sched/task.h:70` 但内核无任何置位点，`kernel/tty/tty.c:169-170` 明确 "SIGTSTP not implemented: just drop the char."）、控制终端与前台进程组的完整语义。**已具备**：setpgid/setsid/getpgid/getsid ✅、tcgetpgrp/tcsetpgrp ✅、tty ISIG + VINTR/VQUIT ✅、kill pid=0/-pid/-1 ✅、SIGWINCH 派发 ✅（PTY master `TIOCSWINSZ` 尺寸变化时 `signal_pgrp(pgrp, SIGWINCH)` 派发到前台进程组，`kernel/tty/pty.c:149`；`user/test_resolution.c` 的 `pty-watch` 观察者与 `qemutests/test_resolution_switcher.py` 场景给出运行期证据）。注意**未收到 SIGWINCH 时默认动作仍是忽略**（`SIG_DFL` ignore，见 `docs/signal/signal.md:25,71`；内核侧 `kernel/arch/x86_64/intr/trap.c:888` 把 `SIGWINCH` 列入非致命 `SIG_DFL` 集，`:928` 的默认忽略分支与 `:1052` 的致命判定同样跳过它） | tty termios ✅；PTY ✅ | |
| /proc 完善 | **全局 `/proc/stat`** ✅ 已就绪（自有 key-value 格式，非 Linux 定长列 ABI，`kernel/fs/procfs_fmt.c:112-146`）；**`/proc/<pid>/stat` 不存在**（pid 目录 lookup 只解析 `status`/`maps`/`fd`，`kernel/fs/procfs.c:596-608`）；`status` 已有 Version/Name/Pid/PPid/State/CPU/Priority/BirthId/StartNs/SampleNs/RuntimeTicks（`kernel/fs/procfs_fmt.c:73-106`），**仍缺 signal mask 与 utime/stime**；**`cmdline` 完全缺失**（`kernel/fs/` 全目录零命中） | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget —— **HTTPS 能力本身已具备**：`CONFIG_TLS=y`（`config/busybox.config.in:980`）启用 BusyBox 1.36 **自带**的 TLS 客户端（`networking/tls.c` + `tls_{aes,rsa,fe,pstm}*.c`，非 OpenSSL/mbedTLS 后端），`CONFIG_FEATURE_WGET_HTTPS=y`（`:994`）时 `wget` 直接走 `tls_handshake()`；`libmbedtls.a` 已入库到 sysroot 但 **wget 并不链接它**（BusyBox 由原生 Makefile 构建、仅覆盖 `CC`/`LD`，全树无 `-lmbedtls`；ASLR-B 只把该归档的构建 flag 改为 `-fPIE -mcmodel=small -mno-red-zone`（`mk/components/sysroot.mk:145`），它仍是静态 `.a` 且**依旧未被链接**）。**剩余**：二选一 —— 把 mbedTLS 真正接进 BusyBox，或明确改用 BusyBox 内建 TLS 并放弃 mbedTLS 路线；并补 HTTPS 端到端验证（**当前无 guest 端 `wget https://` 实测证据**：guest 网络测试走的是 `http://`，`user/nettest.c:224`；`qemutests/build_contract.sh` 的 HTTPS 分支是宿主侧假 wget 夹具，只断言下载 URL 前缀，不构成 guest 端到端证据） | mbedTLS ✅ 已入库为 `libmbedtls.a` | |
| AF_UNIX/socketpair | 本地 socket IPC | socket ✅；**现状**：`socketpair()` 是 `ENOSYS` 桩（`libc/unistd/net_stubs.c:45-48`）；内核 `socket_alloc()` **不校验 `domain`**（`kernel/net/socket.c:64` 的 `socket_alloc(domain, type, protocol)`，`:67-71` 的 switch 只按 `SOCK_STREAM`/`SOCK_DGRAM` 映射 TCP/UDP，`domain` 仅在 `:81` 被原样存入 `s->domain`），请求 `AF_UNIX` 会拿到 TCP/UDP socket 而不是报错——这是正确性缺口，不只是缺功能 | |
| BusyBox applet 悬空软链 | ⚠️ **`config/busybox.config.in` 在 grep/sed/awk/xargs/tar/dmesg 这六个中只启用了 find**（`CONFIG_FIND=y`，`:451`；其余六个均为 `# CONFIG_* is not set`：grep `:483`/sed `:426`/awk `:417`/xargs `:487`/tar `:186`/dmesg `:609`）——生成的 `applet_tables.h` 里也没有这些 applet。**连带缺陷**：`config/rootfs.mk:15` 仍创建 `/bin/grep`、`/bin/sed`、`/bin/awk`、`/bin/xargs`、`/bin/tar`、`/bin/dmesg` 等软链，执行时得到 "applet not found"（悬空软链）→ **待决：启用这些 applet 还是撤掉对应软链**。libc 侧 POSIX 正则引擎未实现（`libc/include/regex.h` 为桩头，`regcomp`/`regexec` 全空转），`glob` 为返回 `GLOB_NOMATCH` 的桩（`libc/stdlib/glob.c:5-12`；libc-completion T16 正则/glob：2026-11-09 用户决定推迟到后续周期）；`fnmatch` 已实现（`*`/`?`/`[]`），**仅 `FNM_NOESCAPE` 被消费，其余 flags 全忽略**（`libc/string/fnmatch.c:12` 的 `(void)flags;` 注释，消费点在 `:55` 的 `*p == '\\' && !(flags & FNM_NOESCAPE)`） | libc | |
| Alpine apk/musl | musl 二进制包兼容路线 | 动态链接器 | cavOS |

## 其他 OS 设计借鉴

| 建议优先级 | 新任务与首版范围 | 完成条件 | 设计来源 |
|---|---|---|---|
| P0 | **在宿主运行完整的 ext2 文件操作链**。沿用现有 RAM 盘夹具，把真实 VFS、ext2 与块缓存接到同一份测试镜像，补上单测之间的接缝。 | 从宿主创建文件、写过直接块边界、同步、卸载，再用全新的缓存重新挂载并逐字节校验。另在指定块读操作注入一次 I/O 错误，调用方收到错误，解除注入后可重新挂载读取。纳入 `make test-host`。 | NetBSD rump 把内核组件链接进用户态程序运行。[rump(3)](https://man.netbsd.org/rump.3) |
| P2 | **给 PID 1 的服务重启加上限流与恢复策略**。现有 `inittab` 已支持 `respawn`；按服务记录连续短命退出，对该服务逐次延长重启间隔，稳定运行后恢复初始间隔。 | QEMU 中同时启动立即退出的测试服务和正常服务。记录前者的重启时间，验证退避生效且有上限；后者持续响应，PID 1 仍能回收子进程。让测试服务稳定运行一轮，再验证退避复位。 | MINIX 3 由独立监督者跟踪服务并处理故障恢复。[设计论文](https://www.minix3.org/doc/reliable-os.pdf) |
| P2 | **输出可查询的根设备诊断快照**。沿 PCI 枚举、块设备注册、根分区匹配三步记录设备来源、候选数和最近错误。挂载失败时从早期串口导出，启动成功后从只读 procfs 节点读取相同字段。 | 用 QEMU 准备无块设备、有盘但根 GUID 不匹配、正常挂载三种配置。前两种仅凭串口即可区分停在哪一步；成功配置可从 shell 查到选中的块设备和分区。 | Fuchsia Inspect 让组件发布可按层次读取的当前状态。[Inspect](https://fuchsia.dev/fuchsia-src/development/diagnostics/inspect) |
| P1 | **让用户程序主动收紧系统调用权限**。借鉴单向收紧原则，首版允许程序在打开输入后放弃后续路径打开权限；原生和兼容入口共用策略，限制随 fork/exec 继承。 | 两架构 systest 证明：已打开的 FD 仍可读，`open` 和 `openat` 均被拒绝，子进程和替换后的程序维持限制，再次调用也无法恢复权限。兼容入口在支持它的 profile 上覆盖。 | OpenBSD 允许进程逐步放弃一组操作。[pledge(2)](https://man.openbsd.org/pledge.2) |
| P5 | **为用户态事件循环提供持久订阅**。在现有 poll 就绪判定上试做 BSD 风格的事件队列，首版只接管道可读和单调时钟定时器，供终端或桌面循环一次等待两类事件。 | systest 覆盖先写后订阅、先订阅后写、定时器到期和删除订阅；关闭管道再复用同一 FD 编号时，新管道不得收到旧订阅的通知。aarch64 SMP 验收前先关闭上文的 poll 偶发挂起问题。 | BSD kqueue 的注册、就绪检查与批量取回机制。[kqueue(2)](https://man.openbsd.org/kqueue.2) |

建议先并行推进 ext2 宿主集成测试与 PID 1 重启限流，两项都有现成测试入口。根设备诊断可随两块 ARM 板的 rootfs 联调推进，首次验收可在 QEMU 完成。权限收紧和事件队列涉及新用户态 ABI，各自先用一个调用方验证语义，再扩大覆盖范围。

---

## Parked（未闭环 follow-on，随时可拾起）

当前仍挂起项（均不阻塞主线）：

- **aarch64 RP1 MSI-X / 控制台 RX 中断评审 deferred minors（2026-11-28）**：
  - `kernel/arch/x86_64/mm/kasan_boot.c`：x86_64 既有文件（本分支未改动）。
  - `aarch64` 既有函数栈帧超 512 B：`aarch64_pcie_prepare`（896 B）与 `aarch64_uart_select_from_handoff`（608 B）在 `aarch64-clang` `kernel.elf` 上超出 `qemutests/stack_frame_audit.py` 512 B 上限（源自 `master` 既有实现，未纳入 `BASELINE_EXCEPTIONS`，本分支未引入；`dtb_init` 已压回 `720 B <= 768 B` 基线）。
  - RPi5 真机板测（`待测`）：需基于最新提交重编 `/tmp/os01-rp1-irq-board` 与 `/tmp/os01-rp1-poll-board` 诊断镜像，并在树莓派 5 实机执行 `tools/tests/aarch64_rp1_irq_board.py` 采集实机证据（详见 `docs/testing/aarch64-rp1-irq.md`）。

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
  - 退役名陈旧注释：原记录的三处中，**aarch64 侧两处仍在**——`kernel/arch/aarch64/runtime/early_print.c:1` 把自身路径写成 `kernel/arch/aarch64/early_print.c`、`:8` 引用已删除的 `kernel/arch/aarch64/main.c`（**2026-11-26 bring-up 后 `:6` 的 `kernel/arch/aarch64/pl011.c` 也已迁到 `kernel/arch/aarch64/platform/pl011.c`，变为第三处退役名**）；x86 侧原 `kernel/arch/x86_64/smp/boot.c:67` 一行随文件变动移位，`boot.c:19` 的 `arch_boot_aps()` 引用经核对属实（该函数仍在 `kernel/core/smp.c:84`）、非退役名。

- **x86 PTE 权限评审 deferred minors（2026-11-13，4 项，均 Minor）**：
  - ① `kernel/selftest/{test_uaccess,test_deep_copy_argv}.c` 中间表 PA 仍用 `PAGE_4K_MASK`（如 `kernel/selftest/test_uaccess.c:257,268,303,321`）；② `kernel/include/arch/x86_64/pte.h:22` 遗留含糊 `PAGE_PAT` 别名（零用户——`pte_bits.h:30-32` 已用 `PAGE_PAT_LARGE` 取代含糊名，但旧别名仍在）；③ `hosttests/cases/test_x86_vmm_backend.c` 缺文件尾换行。
  - ~~原第 ③ 项「`trap.c` 无 VMA COW resolve 未走 `x86_pte_4k_make`」已闭环~~：VMA COW resolve 的 privatize 与 in-place upgrade 两个分支现均走 `x86_pte_4k_make`（`kernel/arch/x86_64/intr/trap.c:622,628`）。

- **内存管理架构边界评审 deferred minors（2026-11-14，M-1..M-5）**：
  - ~~① `memory/user_vm.h` 首行 guard 外 include ARM `vmm_backend.h`~~ **已闭环**：该头已归位到 `kernel/include/arch/aarch64/user_vm.h`，且不再 include ARM `vmm_backend.h`；② ~9 个夹具 + `printk_fb.c` 注释级旧路径（`printk_fb.c` 现按架构落在 `kernel/arch/x86_64/runtime/` 与 `kernel/arch/aarch64/runtime/`，自身头部路径已更新）；③ syscall 审计豁免是仓库级非按文件；④ `mm_header_boundary.py` flag 传递依赖全 dash 前缀（`qemutests/mm_header_boundary.py:98-103` 的 argparse 全为 `--` 前缀）；⑤ ARM fixture shadow mmu.h 用 bool 无 stdbool.h（`hosttests/mock/page_table_aarch64_include/arch/aarch64/mmu.h`，`bool` 有 2 处、`stdbool.h` 未 include）。
  - **中立目录归位第二轮**：`driver/serial.c`（仍 include `intr/apic.h` + `arch/io.h` 等 x86 专属头）等 x86-only 文件仍在原目录；**2026-11-22 输入消费面解耦后已先行清掉一批**——`i8042.c` / `ps2_kbd.c` / `ps2_mouse.c` 下沉到 `kernel/arch/x86_64/driver/`，`kernel/driver/keyboard.c` 与 `mouse.c` 改为只消费 `input_route` / `input_ps2` / `mouse_proto` 中立面（`kernel/driver/keyboard.c` 现只 include `arch/spinlock.h` 等架构中立头）。剩余整体迁移可另开一轮。

- **Craton 改名暂缓（2026-11-17 用户决定）**：
  - README + boot logo 已改（`13c379be`）并验收；**GitHub repo 名与其余 OS01 字符串（2026-11-17 实测 5159 处 / 926 文件：docs 317、kernel 207、hosttests 154、qemutests 99、user 42、libc 23）明确暂缓**，含 `OS01_*` 构建标识符族（`OS01_PROFILE_FILE` / `OS01_SYSTEST` / `OS01_SELFTEST` 等）、include 路径 `<os01/compiler_rt.h>`、LICENSE/README/AGENTS.md 标题行。机械但广面的替换，宜与 repo 重命名同批做。
  - aarch64 boot logo 字标区缝隙透出灰色 (152,152,152) 底层小字（2026-11-08 起既有，疑为固件/早期控制台文字残留，logo 不做整带清除）：可作日后清理项（画字标前清带）。

---

## 近期闭环索引（最近 2 天：2026-11-26 ~ 2026-11-27）

只列条目名与文档指针；完整「做了什么 / 关键结论 / 验证」见 `docs/changelog.md` 对应日期条目。更早的闭环记录同样在 changelog，不在本索引重复。

| 闭环项 | 日期 | 详情 |
|--------|------|------|
| **AArch64 PCIe 一期、DTB `interrupt-map` INTx、`pci_iomap_range()`、AHCI 跨架构与 GICv2m MSI-X**（`OS01_HAS_PCI=1`，QEMU `virt` ECAM 枚举、`/dev/pci`、`lspci`、`interrupt-map` INTx 路由、`ich9-ahci` `/dev/hdb` 读写、统一 `pci_iomap_range()` 与 `v2m@8020000` MSI-X 钩子；`test-aarch64-pcie` 与宿主测试全绿） | 2026-11-27 | [changelog](changelog.md#2026-11-27) · [spec](superpowers/specs/2026-11-27-aarch64-pcie-bus-design.md) · [driver](driver/driver.md) |
| **Radxa Dragon Q8B（SC8280XP）早期内核打通 + GICv3 + ACPI 拓扑回退 + GENI UART**（`dragon-q8b` `0xA0000000` 启动块与 M0 `0xC0000000` 扩展、ACPI `RSDP`/`XSDT`/`SPCR`/`MADT`/`GTDT` handoff、GICv3 `intids=992`、31.3 GiB M0/M1；停在 `boot-3` 根文件系统 `-ENODEV`） | 2026-11-27 | [changelog](changelog.md#2026-11-27) |
| **aarch64 `systest` E2E 接入与 GUI/gfx sysroot 解禁**（`MODE=systest` UP 121/122 PASS；`libgui`/`libgfx` sysroot 与桌面/图形应用解禁） | 2026-11-27 | [changelog](changelog.md#2026-11-27) |
| **aarch64 双平台 UEFI 启动配置（ESP `/config.txt`）+ bring-up 构建开关退役**（公共 `/config.txt` 解析器、`AARCH64_PLATFORM=qemu-virt\|rpi5` 双镜像打包、4 个临时编译开关退役、DTB/handoff/virtio-mmio 校验与补齐测试全绿） | 2026-11-26 | [changelog](changelog.md#2026-11-26) · [spec](superpowers/specs/2026-11-25-aarch64-uefi-boot-config-design.md) · [plan](superpowers/plans/2026-11-26-aarch64-uefi-boot-config.md) |

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
| [aarch64 树莓派 5 bring-up handoff](aarch64-rpi5-bringup-handoff-2026-11-25.md) | RPi5 真机 bring-up 交接记录（硬件与 DTB 事实、三处真机专属根因、板测镜像与判读配方、剩余阻断） |
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

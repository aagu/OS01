# AArch64 UEFI Boot Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal**　让 QEMU virt 和 RPi5 的独立磁盘镜像共用 `BOOTAA64.EFI` 和 `kernel.elf`，由 OS01 ESP 的 `/config.txt` 决定早期启动配置。

**Architecture**　公共 UEFI 入口读取一次 ESP 配置，aarch64 加载器用配置和 DTB 生成 `boot_context` v3 的早期 MMIO 数据。内核按 handoff 建表、选择串口，随后在 SMP 门禁前根据 DTB 映射 virtio-mmio。构建平台选择只进入镜像打包。

**Tech Stack**　C11 freestanding、AArch64 汇编、posix-uefi、GNU Make、mkdisk/mtools、宿主测试、QEMU GICv2 和 RPi5 UEFI v0.3。

**Spec**　[AArch64 UEFI 启动配置设计](../specs/2026-11-25-aarch64-uefi-boot-config-design.md)

## Global Constraints

- 以交接记录和当前未提交 bring-up 差异为实施基线。获得实施授权后，在独立工作树或授权的工作区副本中应用该差异，核对 27 个 tracked 文件的补丁和未跟踪交接文档。空白的 `master` HEAD 缺少实测的缓存清理和双窗口实现。保留原始工作树以及它的索引。
- 在实施副本启动时执行 `os01_bootcfg_build=$(mktemp -d /tmp/os01-bootcfg.XXXXXX)`，把这个绝对路径传给下文 aarch64 构建和验收。准备已有的本地 QEMU EFI 绝对路径 `os01_bootcfg_qemu_firmware`，各 QEMU 构建传入 `AARCH64_UEFI_FIRMWARE_SOURCE="$os01_bootcfg_qemu_firmware"`。Task 5 的依赖验证使用另一个全新构建根。
- `boot_context` v3 固定 200 字节，`early_mmio` 偏移 176。`BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED = 1u << 8`。EFI 表 DTB 最大 2 MiB，RPi5 物理回退只读 `[0x1f0000, 0x210000)`，最大 128 KiB。RP1 排针 CPU PA 为 `0x1f00030000`。
- 配置文件最大 4096 字节。正常调用 `ExitBootServices()`，成功后执行 AArch64 交接缓存清理。保持 4 KiB M0 早期栈以及两段 2 MiB 块的 Device 窗口能力。超过 2 GiB 的每个早期窗口限一个 1 GiB 槽，地址小于 512 GiB。
- `AARCH64_PLATFORM=qemu-virt|rpi5` 只参与打包。默认 QEMU 镜像为 `$(BUILD_DIR)/image/aarch64-uefi.img`，普通 RPi5 镜像为 `$(BUILD_DIR)/image/rpi5/aarch64-uefi.img`。
- 首版配置项为 `platform`、`console` 和兼容 x86_64 的 `resolution WxH`。通用 kernel/init 参数、RPi5 rootfs 和 RP1 RX IRQ 在本期范围之外。
- 新增宿主用例按 `@os01-test` 元数据和所属 `hosttests/mk/*.mk` 的 link group 接入，运行 `make regen-test-catalog` 并将输出与用例一起评审。每完成一项做红绿验证和 `git diff --check`。只有已授权在隔离副本里提交时，才按任务边界提交，明确选择本任务文件。

## Review Focus

- 预备 EFI map 声明可读时仍须同时验证 header、完整 blob 及 v0.3 固定上界。Task 2 覆盖保留 RAM、缺洞和属性拒绝。
- v3 头或 MMIO 准入失败时，早期异常打印和 C panic 都应保持 UART 未发布。Task 3 覆盖坏版本、坏大小、坏窗口和坏串口范围。
- virtio `[base, base + 0x200)` 可能跨块，后续存储阶段还会重复调用架构映射入口。Task 4 覆盖两块、已映射属性和门禁后的再次调用。
- x86 旧分辨率配置和来自其他机型的键可能与新语法混用。Task 1 覆盖默认值、CRLF、混合项和畸形文件。
- 共享构建根里的 RPi5 镜像可能被误交给 QEMU runner。Task 5 覆盖显式拒绝、镜像二进制一致、配置单独重包与负例启动。

## Task 1　公共 ESP 配置读取与 x86 兼容

**Files**　创建 `boot/uefi/boot_config.c`、`boot/uefi/boot_config.h`、`hosttests/cases/test_uefi_boot_config.c`。修改 `boot/uefi/main.c`、`boot/uefi/Makefile`、`mk/components/uefi.mk`、`boot/uefi/arch/arch.h`、两架构的 `boot.c`、`hosttests/mk/drivers.mk` 和生成的测试目录。

**Interfaces**

- `int uefi_boot_config_parse(const char *data, size_t size, bool aarch64, struct uefi_boot_config *out, unsigned *error_line)`，成功返回 0，文件本身错误或 aarch64 已知项错误返回负值。`NULL, 0` 表示缺文件／空文件。`struct uefi_boot_config` 有 `enum uefi_boot_platform platform`（`AUTO`、`QEMU_VIRT`、`RPI5`）、`enum uefi_boot_console console`（`DTB`、`RP1_HEADER`）及 `uint32_t width, height`。枚举常量使用 `UEFI_BOOT_PLATFORM_`／`UEFI_BOOT_CONSOLE_` 前缀。
- 两架构的 `efi_status_t arch_setup_graphics(struct boot_context *ctx, const struct uefi_boot_config *config)`、`efi_status_t arch_fill_firmware(struct boot_context *ctx, const struct uefi_boot_config *config)` 接收加载器栈上的配置。Task 1 只在 x86 GOP 消费分辨率，Task 2 在 aarch64 固件收集中接线。

- [ ] 加宿主用例 `test_uefi_boot_config`。覆盖缺文件默认 `AUTO, DTB, 1440, 900`，混合注释及 `platform\trpi5\r\nconsole rp1-header` 且末尾无换行为成功，`rp1-header` 缺显式 `rpi5`、重复 `console`、裸 CR、嵌入 NUL、4097 字节均失败。验证 `console dtb` 与未知 `kernel.arg` 能共存，且 x86 跳过平台项，用最后一个有效 `resolution 1024x768`，之后的 `resolution 0x768` 或溢出尺寸不抹掉前值。
- [ ] 注册 link group `uefi-boot-config`，只编译其无固件依赖的解析代码。运行 `make regen-test-catalog` 和 `make test-host`，预期先看到新实现缺失，既有用例维持基线。
- [ ] 实现字节解析和一次有界 ESP 读取。文件句柄从原 `fopen` 所用的 LoadedImage 卷获取。读取至 EOF 或 4097 字节，区分 ENOENT 和实际打开／读取错误，关闭后才进入图形、固件采集和最终内存图阶段。aarch64 记录受控的配置错误，x86 在文件整体读取或格式失败时以默认值启动。删除 `x86_parse_config` 和该函数专用的窄字符串辅助代码。x86 保持现有 GOP 枚举循环，只有宽和高均严格增长且不超过目标时才替换候选。目标 `1440x900` 下，先遇 `1024x768`、后遇 `1280x720` 时，应保留第一个模式。新源文件加入内层 `SRCS` 和上层 `UEFI_BOOT_SRCS`，新头加入 `UEFI_BOOT_INPUTS` 以触发 header-only 改动重编。
- [ ] 运行 `make test-host`、`make check-test-catalog`、`make PROFILE=x86_64-clang validate-uefi`、`make PROFILE=aarch64-clang AARCH64_BUILD_ROOT="$os01_bootcfg_build" "$os01_bootcfg_build/artifacts/uefi/BOOTAA64.EFI"` 及 `make PROFILE=x86_64-clang test-qemu SUITE=resolution`。要求宿主、测试目录和分辨率套件通过，两个 EFI 都能链接。

## Task 2　DTB 选择、普通总线翻译及最终内存图验证

**Files**　创建 `kernel/include/arch/aarch64/{fdt_address.h,uefi_memory_map.h}`、`boot/uefi/arch/aarch64/boot_memory.c`、`hosttests/cases/test_aarch64_uefi_boot_memory.c`。修改 `boot/uefi/arch/aarch64/{boot.c,fdt_early.c,loader.h}`、`boot/uefi/{main.c,Makefile}`、`mk/components/uefi.mk`、`boot/uefi/arch/{arch.h,x86_64/boot.c}`、`kernel/arch/aarch64/platform/dtb_parse.c`、`kernel/include/core/bootinfo.h`、`hosttests/cases/{test_aarch64_fdt_early.c,test_bootinfo_abi.c}`、`hosttests/mk/drivers.mk` 和相关目录生成物。

**Interfaces**

- `static inline bool aarch64_fdt_translate_reg_step(const struct aarch64_fdt_bus *bus, uint64_t size, uint64_t *address)`。头文件定义 `struct aarch64_fdt_bus`，含 `const uint8_t *ranges`、`uint32_t ranges_len`、`uint32_t child_cells, parent_cells, size_cells` 与 `bool has_ranges`。walker 从最近的父总线到最远逐层调用，根没有翻译步骤。各侧复用原有祖先缓存，仅临时构造单层视图，避免在 4 KiB 栈中另开大数组。
- `enum aarch64_fdt_scan_result aarch64_fdt_boot_scan(const void *blob, uint32_t size, const char *required_compatible, uint64_t firmware_console_pa, struct aarch64_early_mmio_handoff *out)`，结果为 `AARCH64_FDT_SCAN_OK`、`_INVALID`、`_PLATFORM_MISMATCH`、`_MISSING_MMIO`。`required_compatible == NULL` 表示 `auto`，`firmware_console_pa == 0` 表示 DT 控制台。保留既有纯提取接口 `aarch64_fdt_early_mmio()` 的 GIC-only／UART-only 宿主契约，共用同一 walker。`aarch64_fdt_boot_scan()` 的实际交接要求 GIC 和控制台齐备。
- `static inline int aarch64_uefi_range_overlaps_ram(const void *map, uint32_t entry_size, uint32_t count, uint64_t start, uint64_t end)` 定义在共享的 `uefi_memory_map.h`，返回 1（冲突）、0（无冲突）或负值（描述符或区间无效）。从 UEFI 固定的字节布局读取 type、PA、页数和属性，支持不同 descriptor stride，生产和宿主都不用 EFI 头或额外链接对象。最小 stride 40，检查 `count * stride`、页数扩展和区间端点。按 loader/boot/runtime/conventional/ACPI RAM 及 WB reserved 判定，EFI MMIO 类型始终不当作 RAM。原始缓冲区的可读性、长度和寿命由调用方保证。
- `bool aarch64_fw_dtb_readable(const void *map, uint32_t entry_size, uint32_t count, uint64_t pa, uint64_t length)` 校验固定窗及完整覆盖，复用描述符解码并按 spec 的 v0.3 专用规则验类型和属性。`bool aarch64_early_mmio_map_safe(const struct boot_context *ctx)` 使用最终 raw map 和共享 RAM 查询检查当前早期窗口。`arch_build_memory(...)` 改为 `efi_status_t`，参数沿用公共 hook，x86 返回其原成功路径的 `EFI_SUCCESS`。
- 本任务生产 `BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED`，供 Task 3 消费。用同一 build 的 EFI/内核执行集成测试，RPi5 配置在 Task 3 完成后纳入端到端运行。

- [ ] 扩展 `test_aarch64_fdt_early` 构造 QEMU GICv2／两 UART fixture，`stdout-path` 别名指向第二 UART 并带 `:115200n8`，期望选其 CPU PA。断言禁用／不存在目标、未被 `ranges` 完整覆盖的 `reg`、中间总线缺 `ranges`、platform mismatch 都失败，空 `ranges` 和合法嵌套一／二 cell 翻译成功。构造 Pi GIC-only fixture，固件模式应发布 `console_base=0x1f00030000`，主 Device 窗只覆盖 GIC。保留 `GICD=0x107fff9000`，让 GICC 的范围跨过总线映射端点，应拒绝交接。把 ABI flag 值加到现有 `test_bootinfo_abi`。
- [ ] 新增 map fixture。预备图单个和相邻两段保留 WB 页都允许读取头／完整 blob。拒绝 MMIO、RP、缺洞、跨过 `0x210000`、长度整数溢出。最终 map 用同一窗口覆盖普通 RAM、紧邻 RAM 边界、覆盖 handoff 页测试拒绝／允许，以及第二个 map key 下 RAM 分类变化测试重新判定。运行 `make regen-test-catalog`、`make test-host`、`python3 qemutests/aarch64_dtb_test.py`，确认新增之前失败的场景。
- [ ] 复用两侧现有 FDT walker，以共享函数区分缺失和空 `ranges`，计算全 span 覆盖及父端加法溢出。沿祖先传递启用状态。加载器的扫描先解析根机型、`/chosen` 和 `/aliases`，再按解析出的路径匹配 UART，未指定时用首个启用 UART。GICD/GICC 的头两个必需 `reg` 对都必须存在、各至少 4 KiB 且完整翻译，声明的额外 GIC 区间也必须通过验证后纳入窗口。UART 最小寄存器跨距 `0x48`，固件控制台位于主窗以外时添加自己的 2 MiB 块。保持加载器按实际 DTB 来源分配副本，禁止把固件指针直接写进 handoff。
- [ ] 在 `find_fdt()` 区分未安装 EFI DTB、损坏 EFI DTB 和有效 EFI DTB。只有前一种加显式 RPi5 允许回退。预先取得有长度和 stride 校验的 EFI map，分别检查头和完整 blob。读取后立即复制入 handoff 分配页，保留 EBS 后 cache clean。最终 map 检查识别普通 RAM、loader/boot/runtime RAM 和带 WB 的保留 RAM，并额外检查 B、trampoline、context、DTB 和 raw map 的实际分配区间。所有范围采用半开区间并检查端点溢出。按 spec 校验主窗与额外串口块的形状。加载器在 `.early_mmio` 数据写完后才置有效 flag，并给 RP1 置新位。新源和共享头进入相应的 EFI 编译和 `UEFI_BOOT_INPUTS`。
- [ ] 每次调用 `GetMemoryMap()` 成功后在 `arch_build_memory()` 重跑窗口冲突验证。首次 EBS 前可从该失败打印 UEFI 文本。首次 EBS 后的 `EFI_INVALID_PARAMETER` 只重新取 map、验证和重试，无可用快照或其他错误则本地停机。移除 `AARCH64_SKIP_EBS` 分支，保持 EBS 成功后才交接。留现有 cache clean 及屏障。加载器的阶段标记清理在 Task 3 同步处理。
- [ ] 运行 Task 2 的宿主与 DTB 命令，构建两端 EFI。QEMU 用现有 runner 进入用户态及显示 `UEFI-A64: layout`。在阶段验证中保留 RPi5 启动日志缺少新实现板测证据的状态。

## Task 3　发布可信控制台并同步 M0/M1 窗口

**Files**　修改 `kernel/arch/aarch64/{head.S,boot/boot_symbols.S,boot/early_vectors.S,boot/main.c,memory/device_window.c,platform/dtb.c,platform/dtb_parse.c,platform/pl011.c}`、`kernel/include/arch/aarch64/{boot_offsets.h,boot_layout.h,device_window.h,dtb.h}`、`boot/uefi/arch/aarch64/{boot.c,handoff.S}`、`hosttests/cases/test_aarch64_device_window.c`、`qemutests/aarch64_dtb_test.py`。

**Interfaces**

- `bool aarch64_early_mmio_valid(const struct boot_context *ctx)` 在 `device_window.h` 提供内核和宿主可测的准入验证。需要给 pre-MMU 路径共享时，把 pure 部分放在架构启动区可用的 C 代码段，用 `head.S` 调用，先检查 `ctx` 起始对齐和访问范围，再验证内容；无可靠输出时直接停机。保留 `aarch64_boot_early_console_pa()`，新增 `bool aarch64_boot_console_firmware_configured(void)` 访问已验证的标记。
- 在 `dtb.h` 增加 `int aarch64_dtb_parse_for_console(const void *blob, uint32_t size, uint64_t bsp, uint64_t console_pa, bool firmware_configured, struct aarch64_platform_info *out)`。原 `aarch64_dtb_parse()` 是不提供 console PA 的宿主调用入口，错误码保持 `-1..-4` 的既有分类。

- [ ] 扩展 `test_aarch64_device_window`。合法 v3 双高窗口产生两段排序窗口，覆盖同槽以及 console 已包含时去重，坏 magic、version、size、flag、端点或 4 字节不对齐均使准入失败，超出已验证映射的 PL011 访问跨度、跨 2 GiB 和高窗跨 L1 槽各有反例。扩展 DTB 用例，指定第二 UART 的 handoff PA 并检查其独立 clock/IRQ，固件模式无 UART 仍成功且二者为零，普通模式没有或重复匹配为 `-4`。先运行 `make test-host` 和 `python3 qemutests/aarch64_dtb_test.py`。
- [ ] 将普通窗口及 console 块的区间约束与 Task 2 的 loader 检查对齐。`head.S` 在写停靠区之前检查传入 handoff 可读范围和 v3 契约。若调用公共 C 验证逻辑，低地址入口放入已有 `.boot.text.*` 映射域，保持 B 内且不产生全局数据、栈保护或 sanitizer 运行时访问。本地寄存器或者启动栈保存候选值，不把候选 PA 暴露给 `W!`、`P!` 或 C panic。未发布 PA 时内核控制台无 MMIO 操作。合法 PA 发布后给 M0 建表和 M1 窗口同样的数据。宿主的 `aarch64_device_window_init(NULL)` 保留测试默认窗口。
- [ ] `dtb_init()` 调用按 handoff 选择 console 的 parser，删掉事后覆盖地址并遗留 clock 的路径。`pl011_init()` 在固件配置模式沿用固件已初始化的串口，普通模式使用 DT clock 初始化，RP1 RX IRQ 保持 0。删除 C 的裸 QEMU 串口回退。同步清理 loader/kernel 的 `P/Q/H/K`、framebuffer 阶段标记和 trampoline 多余的 beacon 参数，保留 loader 的正常 GOP 交接及缓存清理。
- [ ] 执行 `make test-host`、DTB 用例和 `make PROFILE=aarch64-clang test-aarch64-audit`。确认汇编可链接、`dtb_init` 未突破仓库栈审计。在隔离 QEMU 镜像用 GDB 在 `_start` 破坏 handoff `version` 或 `size`，验证停在未发布 PA 的分支，串口无 `P!`/`W!` 和内核启动行。正常输入应越过该分支，并在 M0 自测读取到预期的 Device 窗。

## Task 4　按 DTB 在 SMP 门禁之前映射 virtio-mmio

**Files**　修改 `kernel/arch/aarch64/{boot/main.c,driver/virtio_mmio.c,memory/page_table.c,platform/dtb_parse.c}`、`kernel/include/arch/aarch64/page_table.h`。创建 `kernel/include/arch/aarch64/virtio_mmio.h`、`hosttests/cases/test_aarch64_virtio_mmio_map.c`，扩展 `hosttests/mk/memory.mk` 与生成目录。跨架构 `<arch/virtio_mmio.h>` 的现有探测入口保持给 virtio-blk 使用。

**Interfaces**

- `int aarch64_virtio_mmio_prepare(const struct boot_context *ctx)` 在 controllers 阶段调用。之后 `arch_virtio_mmio_map_early(void)` 作为已准备状态入口供现有存储阶段调用。准备完成后它直接返回 0。
- `int aarch64_pt_ensure_device_2m(uint64_t *root, uint64_t va, uint64_t pa)` 成功安装或确认等效的既有块。同 PA、内核 RW、Device、PXN/UXN 的有效 L2 block 可以复用。冲突保持原描述符，返回 `AARCH64_PT_ECONFLICT`。

- [ ] 新增 `aarch64-virtio-mmio-map` 宿主 link group，按 `test_aarch64_pt_split` 的 `AARCH64_PT_SW_HOST_CFLAGS` 编译真实 `virtio_mmio.c`、复用 `aarch64_pt_production.o`、`vmm_gate_production.o`、`vmm_gate_test_stubs.o`。测试提供 mock TTBR1 root PA、DT slot accessor 和页分配器，页表通过已有 mock `ARCH_PAGE_OFFSET=0x100000000` 的 mmap backing 区访问。raw handoff map 使用独立的 2 GiB 以下 host mmap backing。每个成功发布准备状态的场景用独立子进程隔离，只在 fixture 中处理进程级状态。
- [ ] 验证零 slot 不访问页表。`0x0a000000` 和 `0x0a000200` 合为一块，一个在 `0x0a1fff00` 的有效 slot 覆盖 `0x0a000000` 及 `0x0a200000` 两个块。等效 Device 块幂等，Normal、错误 PA、L3 表或非空软件描述符冲突且未覆盖。准备成功后进入模拟 SMP 门禁，重复调用不读取 TTBR1 或改页表。DTB 用例覆盖 slot 地址及 `reg` 溢出、短 `reg`、跨 `ranges` 和错误对齐。运行 `make regen-test-catalog`、`make test-host` 和 DTB 用例，先观察新增行为失败。
- [ ] 从 `arch_boot_memory()` 移除硬编码调用，在 `arch_boot_controllers()` 里 `dtb_init()` 后准备映射，失败用 `log_err` 并 halt。校验完整 DTB `reg` 后，对驱动访问 `[base, base + 0x200)` 取首尾 2 MiB 块、去重。使用 Task 2 的共享 RAM 查询检查每块，包括小于 2 MiB 的 RAM 区间。此时 TTBR1 已切到 M1，raw map 通过仍然存活的 TTBR0 M0 恒等映射读取，和同阶段 `dtb_init()` 读取 DTB 的路径一致。检查 flag、格式、版本、stride、总长度、map 地址低于 2 GiB 且与 B 不相交。存储阶段只消费已发布的结果，无需继续引用 handoff。页表复用现有 L2 walk 和锁写入，`aarch64_pt_map_2m_block()` 的 `EEXIST` 契约独立。全部 slot 成功后再发布准备状态。
- [ ] 运行宿主测试和 DTB 用例。运行 `make PROFILE=aarch64-clang AARCH64_BUILD_ROOT="$os01_bootcfg_build" test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product`。要求 virtio rootfs 可用，测试能在启动的 shell 执行交互用例。运行 `MODE=smp` 并检查四核结果和 M0 页表自测。

## Task 5　平台镜像打包及最终验收

**Files**　创建 `config/aarch64/uefi-qemu-virt.txt`、`config/aarch64/uefi-rpi5.txt`。修改 `mk/targets/aarch64.mk`、`mk/profiles/aarch64-clang.mk`、`mk/{project.mk,components/uefi.mk,components/image.mk,components/run.mk}`、`boot/uefi/Makefile`、`tools/mkdisk.c`、`docs/build/build.md`。新增受控的打包验证用例 `tools/tests/test_aarch64_uefi_images.py`，复用现有 GPT ESP 查找方式。

**Interfaces**

- `AARCH64_PLATFORM` 支持且只支持单值 `qemu-virt` / `rpi5`，省略默认 QEMU，显式空或多个词报错。`mkdisk` 新增可选 `--esp-config <file>`，复制到 FAT32 ESP 根目录 `/config.txt`；省略时保留现有打包行为。使用 argv 方式执行新增的 `mcopy`，保留工具的错误退出传播。
- 验证用例以 `--build-root <absolute-path>` 获取两个产物，使用 `mcopy -i image@@offset` 读 ESP，校验配置和二进制 hash。现有 image variant 目录由 QEMU 测试继续使用。

- [ ] 编写 `test_aarch64_uefi_images.py` 断言两份 ESP 中 `EFI/BOOT/BOOTAA64.EFI` 与 `kernel.elf` 逐字节相同，并各有如下内容。验证错误平台、空值、多个词及 `rpi5 KERNEL_SELFTEST=1` 的失败。对 `rpi5` 选择分别 dry-run `run-aarch64-uefi`、`test-aarch64`、`test-aarch64-boot-logo`、`test-aarch64 MODE=boot-contract`，须在打印任何固件获取、镜像构建或 QEMU 命令前失败。验证普通 RPi5 镜像的 dry-run 不依赖 `QEMU_EFI.fd`。先运行用例观察现有构建失败。

  ```text
  platform qemu-virt
  console dtb
  ```

  ```text
  platform rpi5
  console rp1-header
  ```

- [ ] 在 aarch64 镜像路径层校验选择并决定配置依赖、产物目录、manifest 和 `--temp-dir`。`aarch64-uefi` 的 QEMU firmware prerequisite 只在 QEMU 选择下存在。从普通 RPi5 镜像依赖链剥离 `QEMU_EFI.fd`。对公共 QEMU 入口和现有需要 QEMU 的测试目标用解析期／依赖前的平台 gate，包括 `run-aarch64-uefi`、`test-aarch64`、`test-aarch64-boot-logo` 和 aarch64 boot-contract。保留默认路径和 firmware source 变量。将原四个 bring-up 开关从 profile、受控参数转发和两层重建戳删除，保留实际参与链接的 `AARCH64_BOOT_BLOCK_BASE` 及布局头的失效依赖。
- [ ] 从顶层执行下列命令。`${os01_bootcfg_qemu_firmware}` 是可用的本地 QEMU EFI 绝对路径。RPi5 构建用不存在的 QEMU 固件来源证明该依赖未被求值或获取。另分配一个全新构建根，先 QEMU 后 RPi5，重复同样的 ESP 比较。增量用例记录两个镜像、EFI 和 ELF 的 hash、`mtime_ns` 以及实际 `mkdisk` 执行记录。在隔离副本改变 RPi5 配置的注释，执行两平台构建并检查只运行了 RPi5 的 `mkdisk`，新注释确实已进入 ESP，QEMU 镜像和两个共享可执行文件的内容及时间戳均不变。验证后还原配置。现有构建入口有 `FORCE` 前置项，因此用实际产物和执行记录作判据。

  ```bash
  os01_bootcfg_package_build=$(mktemp -d /tmp/os01-bootcfg-package.XXXXXX)
  make PROFILE=aarch64-clang AARCH64_BUILD_ROOT="$os01_bootcfg_package_build" \
    AARCH64_PLATFORM=rpi5 AARCH64_UEFI_FIRMWARE_SOURCE=/no-os01-qemu-firmware aarch64-uefi
  make PROFILE=aarch64-clang AARCH64_BUILD_ROOT="$os01_bootcfg_package_build" \
    AARCH64_UEFI_FIRMWARE_SOURCE="$os01_bootcfg_qemu_firmware" aarch64-uefi
  python3 tools/tests/test_aarch64_uefi_images.py --build-root "$os01_bootcfg_package_build"
  ```

- [ ] 执行 `make test-host`、`make check-test-catalog`、DTB suite、aarch64 audit、QEMU `MODE=user-runtime USER_RUNTIME_CASE=product` 和 `MODE=smp`、x86 `test-qemu SUITE=resolution`。另对隔离副本运行一次无 ESP 配置的 x86 普通镜像并观察默认 `1440x900` 目标选择。QEMU 负例用同一 runner 拓扑以及正常 `virt` DTB，把 `-drive` 的镜像改为 RPi5 产物，确认在 UEFI platform mismatch 诊断后停止，串口不出现 `UEFI-A64: layout`。再用没有 DTB 的同镜像确认受控缺失分支。
- [ ] 在 RPi5 上用 TF 固件 v0.3 与 `enable_rp1_uart=1`，经 UEFI 从另一个存储设备装载 RPi5 镜像。记录板型、固件、EFI map 对应保留区的类型及属性、`console=0x1f00030000`、`M1 BSP PASS`、GIC 和 `[boot-4] checkpoint=devices status=ok`。当前允许根挂载失败 `FATAL stage=fs component=root rc=-19`。更新构建文档中的双镜像命令、ESP 与 TF 配置路径、对应证据。没有板子或新日志时记录真机待测。

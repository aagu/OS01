# aarch64 树莓派 5 bring-up — handoff 2026-11-25

> **Status（2026-11-25 深夜，最终）**：**真机已跑通内核**——串口有完整启动日志，
> HDMI 显示 Craton logo，M1、GIC-400、DTB、计时器、CSPRNG 全部通过；
> 卡在根文件系统（`FATAL stage=fs component=root rc=-19`，无块设备）。
> 两个真机专属根因已定位并修复：**loader 交接前缺 D-cache clean**、
> **RP1 UART 的 CPU 地址是 `0x1f00030000`**（非固件日志的 `0x1c00030000`）。
> 详见文末「更新 2026-11-25 深夜」。
>
> 全部改动在主仓 `master` 工作树中、**未提交**（按用户要求）。默认构建回归全绿
> （kernel hash 未变、QEMU 启动到 BusyBox）。用户要求：**只写代码，暂不补测试**。
> 下文「更新 2026-11-25（接手后）」一节是当天下午的中间状态（当时排针串口仍无输出），
> 其结论已被文末深夜一节取代。

## TL;DR

- 树莓派 5 的 40-pin 排针 GPIO14/15 是 **RP1 芯片的 UART**（PA `0x1c00030000`），不是 SoC 的 PL011。
  SoC PL011（DTB 里的 `serial@7d001000`）只到 3 针调试座。用户的适配器接在排针上、没有调试线。
- 内核原来在真机上**静默**：解析器拒掉树莓派 5 的设备树（`-4`），且早期打印回退到 QEMU 常量地址。
- 第一步修好了设备树解析与控制台地址；第二步加了构建开关
  `AARCH64_FIRMWARE_CONSOLE_PA` 和「2 GiB 以上的 Device 窗口」，让内核能驱动排针上的 RP1 UART。
- **最大的未验证项**：M1 运行期页表按「两段 Device 窗口」建表的路径，没有在运行中的系统上跑过
  （QEMU 没有那个地址的设备）。真机上如果内核在打印完内存信息后停住，先查这里。
- 可直接上板测试的镜像：`build/aarch64-clang/image/aarch64-uefi-rpi5.img`（见下）。

## 硬件与环境事实（实测）

- 树莓派 5 Model B Rev 1.0，固件日志 `boardrev c04170`（内存字段为 4，推断 4 GiB）。
- TF 卡：VideoCore 固件 + worproject `RPI_EFI.fd`（2,031,616 字节），经 `config.txt` 的 `kernel=` 链载；
  HDMI 显示正常。
- U 盘：OS01 aarch64 镜像。UEFI 菜单能识别并加载 `BOOTAA64.EFI`，之后 HDMI 黑屏。
- 串口：PL2303 USB-TTL，本机节点 `/dev/ttyUSB0`，115200 8N1，接 40-pin 排针 GPIO14/15。
- `config.txt` 三种组合的实测结果：

| 配置 | 排针上的现象 |
| --- | --- |
| `enable_rp1_uart=1` | 固件日志出现，首行 `RP1_UART 0000001c00030000` |
| `enable_uart=1`、`dtparam=uart0=on` | 无输出 |
| 再加 `uart_2ndstage=1` | 仍无输出（日志走调试座） |

- 固件日志要点：`Read bcm2712-rpi-5-b.dtb bytes 76038`、`dtparam: uart0=on`、
  `Device tree loaded to 0x1f0000 (size 0x12fbc)`、`FB0 disp 0 max-fb 1 1920x1080 stride 7680 base 0x3f400000`。
- 完整抓取留在 `/tmp/os01-serial/rpi5-serial.raw`（6471 字节，**临时目录**）。`config.txt` 里
  `#` 开头是注释。

## 设备树事实

离线分析原用的是 raspberrypi/firmware master 的 `bcm2712-rpi-5-b.dtb`（79,237 字节）；
后续已取得与板卡固件日志（76,038 字节）一致的 UEFI 版 DTB：
`/home/aagu/Downloads/RPi5_UEFI_Release_v0.3/bcm2712-rpi-5-b.dtb`（76,038 字节），并已用宿主探针复核：

- 树莓派 5 在 UEFI 固件下是 `arm,gic-400`（GICv2 兼容），**不是 GICv3**；但 `gicv2: interrupt-controller@7fff9000` 位于 `/axi` 下（而非 `/soc`），`reg` 直接使用 `0x10 0x7fff9000 ...`（GICD `0x107fff9000`、GICC `0x107fffa000`）。
- `/soc` 节点 `#address-cells=1/#size-cells=1`，`ranges = <0x7c000000 0x10 0x7c000000 0x4000000>`，将 `0x7c000000..0x80000000` 映射到 PA `0x10_7c000000..0x10_80000000`。
- PL011（`_uart0: serial@7d001000`）位于 `/soc` 下，`reg = <0x7d001000 0x200>`、`status = "okay"`、`interrupts = <0x00 0x79 0x04>`（SPI 121 -> GIC INTID 153）。`chosen/stdout-path = "serial10:115200n8"`。
- 宿主实测 `/home/aagu/Downloads/RPi5_UEFI_Release_v0.3/bcm2712-rpi-5-b.dtb`：
  - `aarch64_dtb_parse(..., bsp=0x0) = 0`：`cpus=4`（`mpidr` `0x0 0x100 0x200 0x300`）、`psci conduit=1 compatible=1`、`gicd=0x107fff9000`、`gicc=0x107fffa000`、`pl011=0x107d001000`、`pl011_spi=153`、`pl011_clock=9216000`、`cntp_ppi=30`、`virtio_mmio=0`。
  - `aarch64_fdt_early_mmio` 返回 `1`：`console=0x107d001000`、`window_base=0x107d000000`、`blocks=24`（`window_end=0x1080000000`）。
- 原解析器返回 `-4` 的全部原因：设备只认根的直接子节点；无 `ranges` 翻译；PL011 寄存器块 0x200
  小于要求的 0x1000；`status != okay` 的 PL011 直接报错。timer、PSCI（smc）、CPU（psci）都能过。
- RP1 UART 在 DTB 里是 `/axi/pcie@120000/rp1/uart0`（`serial@30000`，compatible 仅 `arm,pl011-axi`，`status = "disabled"`），
  经 PCI 风格 ranges 翻译到 `0x1f00030000`（41 位，超出内核 40 位 PA）；固件用的别名是
  `0x1c00030000`（37 位）。因此第二步**没有做设备树自动探测**，改用构建开关。

## 代码改动（未提交）

### 第一步：设备树解析与控制台地址

- `kernel/arch/aarch64/platform/dtb_parse.c`：GIC / PL011 / virtio-mmio 在任意深度查找，地址逐级做
  `ranges` 翻译；`status` 不是 `okay` 的 GIC 跳过，PL011 只在没有可用串口时兜底；
  `PL011_REG_SPAN 0x48`、`GIC_REG_SPAN 0x1000`。
- `boot/uefi/arch/aarch64/fdt_early.c`、`loader.h`：加载器侧同样的翻译与 `status` 过滤。
- `kernel/arch/aarch64/platform/pl011.c`、`boot/boot_symbols.S`、
  `kernel/include/arch/aarch64/device_window.h`：早期打印回退改读 handoff 停靠的控制台地址
  （`aarch64_boot_early_console_pa`）。
- `kernel/arch/aarch64/platform/dtb.c`：新增 `[dtb] pl011=… gicd=… gicc=…` 诊断行；
  handoff 停靠的控制台与 DTB 的 PL011 不同时，控制台留在 handoff 那一侧（仅输出，`spi=0`）。
- `kernel/arch/aarch64/head.S`、`boot/early_vectors.S`：窗口被拒时 `W!` 后打印被拒的三个数值。
- `qemutests/aarch64_dtb_test.py`：既有反例 `short_uart_reg` 的尺寸 0xfff → 0x40（意图不变）。

### 第二步：固件控制台开关 + 高地址 Device 窗口

- `mk/profiles/aarch64-clang.mk`：新增 `AARCH64_FIRMWARE_CONSOLE_PA`（非零、4 字节对齐、
  所在 2 MiB 块低于 512 GiB 且不压启动块），并入 `AARCH64_LAYOUT_CFLAGS`；
  `mk/project.mk` 白名单、`boot/uefi/Makefile` 与 `mk/components/uefi.mk` 的重建戳同步。
- `boot/uefi/arch/aarch64/boot.c`：开关打开时用它覆盖 `early_mmio.console_base`。
- `kernel/include/arch/aarch64/boot_layout.h`：`AARCH64_BOOT_HIGH_END 0x8000000000`（512 GiB）。
- `kernel/arch/aarch64/head.S`、`boot/early_mmu.c`、`early_mmu.h`：窗口可落在 2 GiB 以上
  （一个 1 GiB 槽内，用预留的 L2 表）；控制台块在窗口之外时单独映射；新增 `aarch64_build_boot_window`。
- `kernel/arch/aarch64/memory/device_window.c/.h`：`aarch64_device_windows_get`，最多两段窗口。
- `kernel/arch/aarch64/memory/early_arena.c`、`runtime_tree.c`：页表池计数、建表、校验按两段窗口。
- `kernel/arch/aarch64/boot/main.c`、`memory/page_table_selftest.c`：启动页表自测新增「第 4 步」，
  校验 2 GiB 以上槽位。
- `pl011.c`：开关打开时不改波特率与 FIFO 设置（沿用固件的 115200）。
- `boot_context` ABI **未改**。

## 已验证 / 未验证

| 项 | 结果 |
| --- | --- |
| 默认构建（无开关）：编译 | 通过，改动文件无新增警告 |
| 默认构建：QEMU 启动 1 核 / 4 核 | 启动到 BusyBox shell，无 FATAL |
| 默认构建：栈帧审计 `qemutests/stack_frame_audit.py` | PASS；`dtb_init` 688 B（基线 768） |
| `make test-host` | 258 PASS / 0 FAIL（含 `test_aarch64_boot_layout`、`device_window`、`fdt_early`、`early_arena`、`page_table_tree`、`test_bootinfo_abi`） |
| `qemutests/aarch64_dtb_test.py` | 41 个用例通过 |
| 树莓派 5 DTB → 内核解析器（宿主） | 返回 `0`；原始 ranges / 低地址别名 / uart0 禁用三种都过 |
| 开关变体：编译 | 通过，宏在 204 条编译命令中生效 |
| 开关变体：M0 早期页表 | QEMU 中读回内存：`L1[112] → 0x40090003`，该 L2 表第 0 项 `0x0060001c00000401`；`TTBR0=TTBR1=0x4008c000`、`TCR=0x2b5103510` |
| 开关变体：高地址槽位翻译 | gdb 把该表项临时指向 QEMU 的 PL011 后，恒等地址与高半区别名都能读写 |
| **开关变体：M1 运行期页表两段窗口** | **未验证**（代码路径：`device_window.c` → `early_arena.c` → `runtime_tree.c`） |
| **开关变体：真机** | **未验证** |
| 开关变体：自测第 4 步（`KERNEL_SELFTEST=1`） | 未运行 |
| 固件是否在 UEFI 阶段改动 RP1 UART / PCIe 窗口 | 未知 |

QEMU 上开关变体的现象：内核在 `arch_early_init` 第一次写控制台时收到 Data Abort
（`ESR 0x96000050`），随后早期向量打印 `P!` 进入循环。这是**预期内**的——virt 机型在
`0x1c00030000` 没有设备，报的是 Synchronous External Abort，不是翻译故障。

## 镜像

- 树莓派 5 变体：`build/aarch64-clang/image/aarch64-uefi-rpi5.img`
  SHA-256 `854746f988d648d8773cdd730f116c604ddcc38b515069b74e343e705460068e`
  （`kernel.elf` `005f1299…8befe5`，`BOOTAA64.EFI` `d7a55ca4…a5e47`）。
  重新生成：`make PROFILE=aarch64-clang AARCH64_FIRMWARE_CONSOLE_PA=0x1c00030000 aarch64-uefi`
  然后把 `image/aarch64-uefi.img` 另存。**该文件在 `build/` 下，不入库，`make clean` 会删。**
- 默认产物已恢复为无开关构建：`image/aarch64-uefi.img`
  SHA-256 `9294641019d38fcb68e8b77608eeae5e02e5ef5d4ba079441b1d443548e01679`。
- 开关构建会覆盖 `build/aarch64-clang` 下的默认产物，切回默认直接重跑不带开关的 `aarch64-uefi`。

## 上板步骤与结果判读

1. 把 `aarch64-uefi-rpi5.img` 写入 U 盘（会清空整盘）。
2. TF 卡 `config.txt` 保留 `enable_rp1_uart=1`（固件据此把 RP1 UART 初始化成 115200 并接到排针）。
3. 抓串口（puTTY 需关闭，否则端口被占）：
   `stty -F /dev/ttyUSB0 115200 raw -echo cs8 -parenb -cstopb clocal; cat /dev/ttyUSB0 | tee rpi5.log`
4. 上电。正常情况下固件日志之后应出现内核的第一行 `UEFI-A64: layout entry=… context=…`，
   接着是 `[boot] early device window …`、`UEFI-A64: RAM ranges=…`、`[dtb] pl011=0x1c00030000 spi=0 …`。

| 现象 | 含义 / 先查哪里 |
| --- | --- |
| `W! <窗口基址> <块数> <控制台地址>` 后停住 | `head.S` 判定窗口或控制台块的位置无法表示（跨 2 GiB、跨 1 GiB 槽、超 512 GiB） |
| 有固件日志，没有内核输出，也没有 `P!` | `RPI_EFI.fd` 阶段改动或复位了 RP1 UART / PCIe 窗口；或加载器失败（看 HDMI 上的 `UEFI:` 报错行） |
| 立刻出现 `P!` | 早期异常。用 gdbstub 或 `-d int` 取 `ESR`/`FAR`/`ELR`，`llvm-addr2line -e kernel.elf` 定位 |
| 打印完 `UEFI-A64: RAM ranges=…` 后停住或 `P!` | **M1 两段窗口路径**（最可能的未验证缺陷） |
| 内核日志继续、停在别处 | 控制台已通，进入下一项 bring-up（GIC-400 的 Group 1 / PMR、PSCI、熵源等） |

## 下一步（建议顺序）

1. **在 QEMU 上完成 M1 两段窗口路径验证**（不需要板子）：
   - 已重编开关变体并实测修正 `gdbredirect.py` 中的两处脚本问题：
     1. M0 启动页表中 `L1[112]` 实际指向 `0x40091003`（`tables[5]`，`0x40091000`），而非 `0x40090000`（`tables[4]`）；
     2. `break *ffff0000400db028` 缺少 `0x` 前缀导致 GDB 报 `No symbol table is loaded`。
   - 修正后的脚本已成功改写 M0 控制台 L2[0] 为 `0x0060000009000401`，但随后在 25 秒超时前未命中 `arch_vmm_init`（`0xffff0000400db028`），串口仍停在 `UEFI: ESP GUID=...`，尚无内核输出。
   - **待排查**：`0x40080270` 之后到 `arch_vmm_init` 之间内核停在何处（建议在 GDB 中中断或配合 `-d int` 查看 `PC`/`ELR_EL1`/`ESR_EL1`/`FAR_EL1`）。
2. 上板，按上表判读。
3. 控制台通了之后，让内核把固件实际交付的 DTB 从串口吐出来，再据此设计 RP1 UART 的自动探测
   （替代构建开关），并复核「设备树事实」一节。
4. 之后的真机阻断：GIC-400 的 Group 1 与 PMR 交接、PSCI（smc）实测、熵源（aarch64 只有 RNDR 与
   `EFI_RNG_PROTOCOL`，无 rng200 驱动）、存储（无 SDHCI 驱动，根文件系统挂不上）。
5. 补测试（用户明确推迟）：解析器嵌套总线与 `ranges`、`fdt_early` 翻译、`aarch64_build_boot_window`、
   运行期页表两段窗口、自测第 4 步；更新 arch.md / roadmap / changelog；在特性分支上提交。

## 调试配方与踩过的坑

- 宿主上直接跑真解析器 / 加载器遍历器（不经 QEMU）：
  `clang -O2 -Wall -Wextra -Werror -std=c11 -I kernel/include probe.c kernel/arch/aarch64/platform/dtb_parse.c`；
  加载器侧加 `-I boot/uefi/arch/aarch64` 链 `fdt_early.c`。定位拒因：把 `return -4;` 替换成带 `__LINE__` 的
  `fprintf` 再编一次。
- QEMU 对照组 DTB：`qemu-system-aarch64 -M virt,gic-version=2,dumpdtb=x.dtb -cpu cortex-a53 -smp N`，
  再 `dtc -I dtb -O dtb` 打包。**`dumpdtb` 的 `-smp` 必须与启动时一致**，否则
  `[gic] FATAL: invalid/duplicate GIC target`（是命令失误，不是镜像缺陷）。
- 手工 QEMU 启动要带 `acpi=off` 和 `-dtb`（默认机型开了 ACPI，固件就不交 FDT）；
  `make PROFILE=aarch64-clang print-run-paths` 对 aarch64 不可用。
- **ESR 故障码**：ISS 低 6 位 `0b0100LL`（如 `0x10`）是 Synchronous External Abort；
  翻译故障是 `0b0001LL`（`0x04` 起）。我一开始把 `0x10` 误读成「0 级翻译故障」，绕了很久。
- QEMU 访问没有设备的地址会报 External Abort；gdb 读这种地址只报 `Cannot access memory`，
  **不能**据此判断翻译是否成功。
- 系统寄存器：HMP `info registers` 不显示；gdb 要用 `info all-registers`
  （单独 `info registers ttbr0_el1` 报 Invalid register）。`-d mmu` 无输出；`-d int` 有异常现场。
- HMP socket 写法：`-monitor unix:PATH,server=on,wait=off`。
- 栈帧审计只看函数开头 25 条指令，对 `aarch64_dtb_parse` 量到 96 B，真实帧 1824 B（以编译器警告为准）；
  `dtb_init` 基线上限 768 B 不能破；4 KiB 早期栈上最深调用链约 2720 B。
- 串口：PL2303 逐字节到达，文本视图要按换行符断行；板子断电瞬间会收到一个 `0x00`（break）。

---

## 更新 2026-11-25（接手后）：QEMU M1 验证完成 + 板测 beacon

### 结论速览

- 原 handoff「下一步 1」的阻塞点**不是内核缺陷**，而是 gdb 重定向脚本的地址算法错误。
  2 MiB block 描述符只能整块重映射；控制台在 `0x1c00030000`（块内偏移 `0x30000`），
  把块基址指向 PL011 的 `0x09000000` 后实际访问落到 `0x0903xxxx`（无设备），
  而不是 `0x09000018`。所以内核日志从未出现过，`arch_vmm_init` 断点当然也不会命中。
  **正确做法**：给控制台所在块挂一张 4 KiB 页表（L2[0] → L3 表），
  `L3[0x30] = 0x0060000009000403`（VA 页 `0x1c00030000` → PA `0x09000000`，Device）。
- M1 树的重定向必须打在其**第二次** `aarch64_runtime_tree_validate` 与 `zones_match`
  之后、第一条 `M1 ZONE` 日志之前（`0xffff0000400d06a8`）。提前打会让树校验失败，
  内核打印 `M1 FATAL reason=runtime-init`。
- 按此方法，开关变体（`AARCH64_FIRMWARE_CONSOLE_PA=0x1c00030000`）在 QEMU 上
  **完整启动到 BusyBox**：`M1 ZONE/COVERAGE`、`M1 BSP PASS root=0x41400000`、
  `[dtb] pl011=0x1c00030000 spi=0`、全部子系统、用户态 init + shell。
  M1 两段窗口（低 device window + 高 console block）路径**已验证**。

### 代码修正（工作树，未提交）

- `kernel/arch/aarch64/boot/main.c`：自测第 4 步有 5 个多余的 `"`（`UINT64_C(0x...")`）。
  该段在 `#if OS01_SELFTEST` 下，默认构建看不到，所以一直没编译过；
  `KERNEL_SELFTEST=1` 会直接编译失败。已修。
- 新增板级 bring-up beacon（仅 `AARCH64_FIRMWARE_CONSOLE_PA` 且
  `AARCH64_FW_CONSOLE_BEACON=1` 时编译）：
  - loader 的 handoff trampoline（MMU 关闭）向该 PA 写 `H\r\n`；
  - 内核 `_start` 停靠完 handoff 后（MMU 关闭）写 `K`。
  - 两者都**只写**。`AARCH64_FW_CONSOLE_BEACON` 默认随 knob 打开；
    QEMU 必须设 `AARCH64_FW_CONSOLE_BEACON=0`——该地址在 QEMU 无设备，
    store 会同步异常，把 loader/内核打死。
- `mk/profiles/aarch64-clang.mk`、`mk/project.mk`、`boot/uefi/Makefile`、
  `mk/components/uefi.mk`：新开关的定义、传递与重建戳。

### 构建注意（重要）

- 现在 `build/aarch64-clang/` 里的产物是**混合状态**：`aarch64-uefi.img` 里的
  kernel `pl011_init` 是 knob 版本（跳过波特率配置），不是默认构建。
  不要拿它当默认镜像；也不要相信 handoff 里「默认产物已恢复」的 SHA。
- 变体构建请用独立的 `AARCH64_BUILD_ROOT`（或 `make clean`）：
  - 默认：`make PROFILE=aarch64-clang AARCH64_BUILD_ROOT=/tmp/... aarch64-uefi`
  - 开关：再加 `AARCH64_FIRMWARE_CONSOLE_PA=0x1c00030000`
  - QEMU 验证开关变体：再加 `AARCH64_FW_CONSOLE_BEACON=0`
- 已验证：干净默认构建 kernel hash `db696e71…`；在干净 root 里增量 flip 到 knob
  得到的与其干净 knob 构建一致（`e2a1096f…`），说明 flags stamp 正常。
- 判别镜像里的 kernel 是否为 knob：反汇编 `pl011_init`，40 条 = knob，83 条 = 默认。

### 板测镜像与判读

- 已生成（2026-11-25）：
  `build/aarch64-clang/image/aarch64-uefi-rpi5-beacon.img`
  SHA-256 `885153126986db9e62b0114a6225b8f601deb312dbc75d2db6a9b45fda1796e9`
  （kernel.elf `b4577de2…`，为 knob 版本：`pl011_init` 40 条指令，跳过波特率配置）。
  重新生成：
  `make PROFILE=aarch64-clang AARCH64_BUILD_ROOT=/tmp/... AARCH64_FIRMWARE_CONSOLE_PA=0x1c00030000 aarch64-uefi`
  （`AARCH64_FW_CONSOLE_BEACON` 默认随 knob 打开；QEMU 复验要显式设 0。）
- 串口（115200 8N1，GPIO14/15）应依次出现：
  1. `H` —— loader 的 ExitBootServices 成功、跳入 trampoline；
  2. `K` —— 内核 `_start` 完成 EL 切换/栈/清 BSS/停靠 handoff；
  3. `UEFI-A64: layout …` —— MMU 已开、首次内核日志；
  4. `M1 ZONE …` / `M1 BSP PASS …`、`[dtb] pl011=0x1c00030000 spi=0 …`。
- 判读：
  - 没有 `H` → loader 在 ExitBootServices 前失败，或 RP1 UART 在 UEFI 阶段被关；
  - 有 `H` 没 `K` → 内核入口/trampoline 失败；
  - 有 `H`+`K` 没有内核日志 → head.S 的窗口/MMU 路径失败（留意预 MMU 的 `W!`）；
  - 过了 `M1 BSP PASS` 再失败 → 进入后续 bring-up（GIC-400 Group 1/PMR、PSCI、存储）。

### QEMU 复验配方

```
# 1) 构建（beacon 关）
make PROFILE=aarch64-clang AARCH64_BUILD_ROOT=/tmp/os01-m1/build-knob \
     AARCH64_FIRMWARE_CONSOLE_PA=0x1c00030000 AARCH64_FW_CONSOLE_BEACON=0 aarch64-uefi
# 2) 启动：-M virt,gic-version=2,acpi=off + QEMU 生成的 virt DTB + -gdb tcp::1240
# 3) gdb：
#    hbreak *0x40080270            ; MMU 开启前
#    T4[0] = (0x40100003)          ; 4 KiB L3 表
#    L3[0x30] = 0x0060000009000403
#    hbreak *0xffff0000400d06a8    ; 第一条 M1 日志之前
#    TTBR1 走表找到 L2[0] (0x...401)，替换为 0x40180003；L3[0x30] 同上
#    hbreak *0xffff0000400db028    ; arch_vmm_init
```

### 仍未验证

- RPi5 真机（待上板）。
- head.S 的「高 device window」分支（device window 本身在 2 GiB 以上，L1 slot 65）。
  QEMU 这条路径只有低 device window + 高 console block；真机会走两个高窗口。
- `KERNEL_SELFTEST=1` 第 4 步实跑。

---

## 更新 2026-11-25 深夜：真机首次跑通内核（Craton logo on HDMI）

### 根因（终于）：loader 交接前缺少 D-cache clean
- 内核 `_start` 在 EL2 下开始执行；`eret` 到 EL1 后 MMU 关闭，取指/读 handoff 直接访问 DRAM。
- loader 写内核镜像与 `boot_context` 走可缓存的 UEFI 映射，数据脏在 D-cache，DRAM 是旧的 →
  eret 后取指/读 context 读到垃圾 → 异常进固件 VBAR → 挂死。QEMU 不模拟 cache，只在真机出现。
- 修复：`boot/uefi/arch/aarch64/boot.c` 的 `arch_enter_kernel()` 在交接前对 boot block
  （内核镜像）、trampoline、boot_context、FDT 副本、内存图做按 VA 的 `dc civac`（到 PoC），
  再 `dsb sy; ic iallu; dsb sy; isb`。该修复对所有 aarch64 交接生效，默认构建在 QEMU 无行为变化。

### 关键地址：RP1 UART 的 CPU 别名是 0x1f00030000
- 固件日志里的 `RP1_UART 0x1c00030000` 是 VideoCore 自己的别名，ARM CPU 写不进去（静默丢弃）。
- DTB 的 `/axi/pcie@120000/ranges` 把 RP1 翻译到 **0x1f00030000**；loader 的 P/Q 探测
  （`P`→0x1c…、`Q`→0x1f…）只有 `Q` 出现在排针串口上。
- 内核控制台 PA 必须用 `AARCH64_FIRMWARE_CONSOLE_PA=0x1f00030000`。

### DTB 获取
- RPI_EFI 没有安装 EFI DTB 配置表；`find_fdt()` 增加回退：读 VideoCore 加载 DTB 的 PA `0x1f0000`
  （magic + totalsize 校验，≤2 MiB）。
- 真机解析结果：`/cpus: 4 CPUs`、`gicd=0x107fff9000 gicc=0x107fffa000`、早窗口
  `0x107d000000..0x1080000000`。控制台块与设备窗口是两段高地址窗口（L1 slot 124 与 65），
  head.S/M1 的两段窗口路径在真机上验证通过。

### 真机当前进度（2026-11-25 23:40）
```
PQH K
UEFI-A64: layout entry=0x40080000 context=0x37f43000
[cpu] MIDR_EL1 = 0x414fd0b1
[boot] early device window base=0x107d000000 end=0x1080000000
UEFI-A64: RAM ranges=5 pages2m=2011
M1 ZONE/COVERAGE ×5, M1 BSP PASS root=0x41400000 ranges=5
[logo] boot logo drawn (aarch64, 1920x1080)      ← HDMI 上的 Craton logo
[fb] frame buffer remap succeed
[dtb] /cpus: 4 CPUs, BSP MPIDR=0x0
[dtb] pl011=0x1f00030000 spi=0 clk=44236800 gicd=0x107fff9000 gicc=0x107fffa000
[gic] GICv2 driver: intids=320
subsys: init clocksource/timer/timekeeping/virtio-blk/thermal ... ok
CSPRNG: seeded STRONG from UEFI GetRNG
[boot-3] FATAL stage=fs component=root rc=-19
```

### 下一个阻断：根文件系统（-ENODEV，无块设备）
- RPi5 没有 virtio-mmio；内核还没有 SDHCI / USB / PCIe-RP1 存储驱动。
- 新增开关：`AARCH64_VIRTIO_MMIO=0`（跳过 QEMU 专用 virtio-mmio 映射；该地址在 RPi5 是 RAM）。
- 可选推进方向：RAM 根文件系统 / initramfs、SDHCI 驱动、或 UEFI Block I/O 一次性搬运 rootfs。

### 板测镜像
- `build/aarch64-clang/image/aarch64-uefi-rpi5-virtio0.img`
  SHA-256 `393ab0d43fdc4b96c293e192c7e347edb38fdb981e3d83350420fbb78972e35a`
- 构建：
  `make PROFILE=aarch64-clang AARCH64_BUILD_ROOT=/tmp/... AARCH64_FIRMWARE_CONSOLE_PA=0x1f00030000 AARCH64_VIRTIO_MMIO=0 aarch64-uefi`
- `AARCH64_FW_CONSOLE_BEACON` 默认随 console PA 打开；QEMU 复验设 0。

### 新增/修改的开关（全部未提交）

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `AARCH64_FIRMWARE_CONSOLE_PA` | 无 | 固件已初始化的 UART PA；RPi5 用 `0x1f00030000` |
| `AARCH64_FW_CONSOLE_BEACON` | 随 console PA 打开 | loader 帧缓冲阶段块 + trampoline `H` + 内核 `K`/帧缓冲标记；**QEMU 必须设 0** |
| `AARCH64_VIRTIO_MMIO` | 1 | QEMU virtio-mmio 早映射；RPi5 设 0 |
| `AARCH64_SKIP_EBS` | 0 | 跳过 ExitBootServices（本次排查用，最终**不需要**，保持 0） |

### 本次改动的文件（工作树）

- `boot/uefi/arch/aarch64/boot.c`：**cache clean 交接修复**（`flush_handoff_caches`）、
  `find_fdt` 的 `0x1f0000` 回退、帧缓冲阶段块与 pre-EBS UART 探测、`arch_enter_kernel` 参数扩展。
- `boot/uefi/arch/aarch64/handoff.S`：trampoline `H` + 帧缓冲 checkpoint 参数。
- `boot/uefi/main.c`：`AARCH64_SKIP_EBS` 分支（保留但不启用）。
- `kernel/arch/aarch64/boot/main.c`：自测第 4 步 5 个多余 `"` 修复；`AARCH64_VIRTIO_MMIO` 跳过。
- `kernel/arch/aarch64/head.S`、`kernel/arch/aarch64/boot/early_vectors.S`、
  `kernel/include/arch/aarch64/boot_offsets.h`：内核入口帧缓冲标记（仅 beacon 构建）。
- `kernel/arch/aarch64/driver/virtio_mmio.c`：`AARCH64_VIRTIO_MMIO=0` 时 no-op。
- `mk/profiles/aarch64-clang.mk`、`mk/project.mk`、`boot/uefi/Makefile`、
  `mk/components/uefi.mk`：上述开关的定义/传递/重建戳。
- `docs/aarch64-rpi5-bringup-handoff-2026-11-25.md`：本文档。

### 回归证据（2026-11-25 深夜）

- 干净默认构建 kernel hash `db696e71f5f1…`（与本次改动前一致；beacon 代码编译期剔除）。
- QEMU 默认镜像仍完整启动到 BusyBox（`M1 BSP PASS root=0x41400000`）。
- 默认 loader 增加了 cache clean 与 DTB 回退；QEMU 上均为无副作用（配置表优先、cache 操作 no-op）。

### 下次建议顺序

1. **根文件系统**（当前唯一阻断）：评估三条路——
   - loader 把 rootfs 镜像/initramfs 搬到 RAM，内核支持 RAM 根；
   - SDHCI 驱动（RPi5 SD 卡控制器）；
   - UEFI Block I/O 在 loader 阶段一次性把 rootfs 读进 RAM。
2. 4 核 SMP（DTB 已是 4 CPUs；PSCI 实测、AP 启动）。
3. 存储/PCIe-RP1（USB、SD）与网络。
4. 补测试（用户推迟）：cache clean、DTB 回退、两段高窗口、beacon 开关矩阵。

### 上板抓串口配方（本机已验证）

```
stty -F /dev/ttyUSB0 115200 raw -echo cs8 -parenb -cstopb clocal
python3 -I /tmp/os01-serial/capture.py /dev/ttyUSB0 /tmp/os01-serial/<name> 1200
# 确保只有一个读者（不要同时 cat/puTTY），否则字节会被分流
```

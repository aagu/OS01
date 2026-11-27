# aarch64 U 盘启动实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标**是在 QEMU `virt` 与 Raspberry Pi 5 Model B 上从 U 盘启动 aarch64 OS01。内核接管 xHCI 后，用 UEFI 交接的 ESP GUID 找到原盘并从该盘挂载两个文件系统，进入用户态。

**架构**共用 xHCI、BOT 与块设备路径，QEMU 使用已支持的 PCI ECAM，RPi5 验证并使用 WoR v0.3 留下的 RP1 PCIe 窗口和 DWC3 host 状态。启动身份、块设备注册和挂载沿用现有接口。

**工具链**使用 `x86_64-clang`、`aarch64-clang`、GNU Make、QEMU、`dtc`、Python `unittest` 和现有宿主测试框架。镜像上置入验收文件使用 `mcopy`、`debugfs` 和 `sfdisk`。

**设计**见已评审的 [aarch64 U 盘启动设计](../specs/2026-11-27-aarch64-usb-boot-design.md)。

## 全局约束

- 仍用 `boot_context` v3 与 GPT 双分区镜像。固件从 U 盘 ESP 启动 `BOOTAA64.EFI`，loader 从同一 ESP 读取 `config.txt` 和 `kernel.elf`，内核按其 ESP GUID 选盘。重复匹配为 `-EINVAL`，有盘但无匹配为 `-ENOENT`，现有几何告警和旗标缺失兼容路径继续有效。
- aarch64 接入 512 B、单 LUN、已插入、直连、High-Speed BOT 设备。QEMU PCI 只匹配 `1b36:000d` 与 class `0x0c0330`，BAR0 长度 `0x4000`。RPi5 xHCI 子窗口长 `0x8000`，DMA 上限为 `0xc0000000`，另受 RC 窗口读回、outbound 冲突和 PMM 所有权约束。
- x86_64 继续使用原有 PCI class 匹配、HID、Hub、devfs 和 MSC 持久化路径。aarch64 不打开 `OS01_HAS_USB`。两种架构的运行日志使用 `<log/log.h>`，aarch64 公共路径用其运行时支持的 `%s`、`%u`、`%d`、`%lu`、`%lx` 等。保留既有 x86 E2E 识别的日志事件。
- 新宿主用例使用 `@os01-test`、现有 `LINK_CFLAGS_*` 和 `LINK_GROUP_*`，登记关联场景并运行 `make regen-test-catalog check-test-catalog`。任务中的宿主命令使用 `PROFILE=x86_64-clang`。现有生成目录和 legacy bridge 由项目扫描规则维护。
- QEMU 与 RPi5 分别记录结果证据。RPi5 的完成条件包括两次真实冷启动，私有验收盘的串口脚本不进入产品镜像。Dragon Q8B、其他 RP1 固件、通用 RC/DWC3 初始化和 USB 输入功能都由其独立工作跟进。

## 重点回归

| 风险 | 必须覆盖的验证 |
| --- | --- |
| 默认架构仍有旧路径和配置组合 | T1 用现有宿主、x86 USB boot/HID 端到端比较迁移前后 |
| 高地址 BAR0 尚未映射、doorbell 只够 slot 0、固件控制器仍在工作 | T2 在 fake PCI/MMIO 中断言拒绝原因、命令寄存器顺序与资源保留 |
| 非恒等 DMA 地址、跨窗或 cache 时序错误 | T2/T4 断言寄存器和完成事件使用设备地址、对象跨界被拒绝、CYCLE 与 IN 生命周期正确 |
| USB 正确启动但首盘是 virtio，或 USB GUID 不对应合法 ESP | T3 合法双盘和错误 ESP 类型负例都从 USB 交接，同时读取实际挂载结果 |
| 固件 DTB 声明与真实 RP1 窗不一致，或板上串口只能 TX | T4 比较硬件读回夹具与 DT 偏移，T5 通过实际 shell 脚本从串口采证 |

### T1 · 迁移 xHCI 核心与保留 x86 行为

**文件**

- 移至 `kernel/driver/` 的源文件为 `xhci.c`、`xhci_hub.c`、`xhci_msc.c`、`xhci_hid_desc.c`，原文件在 `kernel/arch/x86_64/driver/`。新建 `kernel/driver/xhci_pci.c`。
- 修改 `kernel/include/driver/xhci.h`、`kernel/Makefile`、`hosttests/mk/drivers.mk`、`hosttests/mock/arch9/xhci_runtime.h`。现有 `hosttests/cases/test_xhci_{init,phase2,phase3,hid_desc,hid_service,intr_in}.c` 检查其直接调用与 mock 边界。
- x86 HID 服务继续由 `kernel/arch/x86_64/driver/xhci_hid.c` 管理，保留其消费者使用的 PCI BDF 及 HID ABI。

**接口**

- 共用核心提供 `int xhci_program_rings(struct xhci_controller *ctrl)`、`int xhci_run(struct xhci_controller *ctrl)`、`int xhci_stop(struct xhci_controller *ctrl)`、`int xhci_publish_controller(struct xhci_controller *ctrl)` 和 `void xhci_unpublish_controller(struct xhci_controller *ctrl)`。`xhci_start()` 保留为 program → run 的兼容入口。`xhci_alloc_dma()`、`xhci_free_dma()`、`xhci_parse_capabilities()`、`xhci_reset()` 和 `xhci_probe_ports()` 继续作为核心入口。
- PCI 前端保留 `xhci_probe()`、`xhci_remove()` 和 PCI driver 声明。核心接收已映射的 MMIO 基址和长度。x86_64 初始路径继续使用同等 BAR mapping 与 legacy handoff 语义，PCI 装载后沿用已有控制器注册与端口枚举顺序。
- 核心日志通过一段前端填写的稳定位置描述标明控制器，x86_64 保留 `bdf=bb:dd.f` 证据，RP1 可用硬件节点名。共享代码的空转使用 `arch_cpu_pause()`。aarch64 先暂缓链接生产 xHCI，待 T2 接通。

- [ ] **Step 1** 先用现有 xHCI host tests 和 x86 两条 USB E2E 记录基线。把 MMIO 夹具显式填入大小。新增测试 `test_program_rings_keeps_controller_stopped`，断言编程 CRCR/DCBAAP/ERST 时 `USBCMD.RS == 0`，调用 run 后才变成 1。
- [ ] **Step 2** 运行 `make PROFILE=x86_64-clang test-host`。新增测试应因缺少拆分后的核心接口失败。
- [ ] **Step 3** 移动源文件并拆分 PCI 前端与共用核心。把宿主生产对象重新接到迁移文件，HID 解析器和服务各自保留原有覆盖。共享核心退出控制器登记前先完成安全停止。x86 PCI 前端调用原 `xhci_hid_detach_controller()`，共享核心独立于该服务。
- [ ] **Step 4** 运行 `make PROFILE=x86_64-clang test-host`、`make PROFILE=x86_64-clang test-qemu SUITE=usb-boot`、`make PROFILE=x86_64-clang test-qemu SUITE=usb-hid`。预期控制器、MSC、x86 6 组 boot 和 HID direct/hub 通过。
- [ ] **Step 5** 提交本任务涉及的源文件及覆盖，提交消息 `refactor(usb): share the xHCI controller core`。

### T2 · 接通 aarch64 PCI xHCI 与 DMA 所有权接口

**文件**

- 修改 `kernel/driver/{xhci,xhci_pci,xhci_msc,xhci_hub,usb_msc,usb_msc_bot}.c`、`kernel/include/driver/xhci.h`、`kernel/Makefile` 、`kernel/include/arch/aarch64/pcie.h` 和 `kernel/arch/aarch64/bus/pci.c`。新建 `kernel/driver/xhci_dma.c`、`kernel/include/driver/xhci_dma.h`、`kernel/arch/aarch64/driver/rp1_xhci.c`。本任务的 `rp1_xhci.c` 仅提供依赖链中的空 provider，T4 接入硬件。
- 修改 x86 HID 调用者中引用共享 DMA ring 和 staging 的字段。在 `hosttests/cases/test_aarch64_pcie_ecam.c` 扩展早期高窗映射用例。新建 `hosttests/cases/test_xhci_dma.c`，复用 `hosttests/mock/arch9/xhci_runtime.h` 和 `hosttests/mk/drivers.mk` 链接生产代码，更新 `config/test-scenarios.json` 及生成目录。

**接口**

- `struct xhci_dma_buf` 保存 `void *cpu`、`uint64_t phys`、`uint64_t bus`、`size_t size` 和后端释放所需的分配 cookie。`int xhci_dma_alloc(struct xhci_controller *ctrl, size_t bytes, size_t align, size_t boundary, struct xhci_dma_buf *out)`，`void xhci_dma_free(struct xhci_controller *ctrl, struct xhci_dma_buf *buf)`。块内 ring 指针写设备地址，事件中回显地址也与 `bus + offset` 比较。
- 控制器 DMA ops 提供 `int (*alloc)(void *ctx, size_t bytes, size_t align, struct xhci_dma_buf *out)`、`void (*free)(void *ctx, struct xhci_dma_buf *buf)`，以及 `void (*sync)(void *ctx, void *cpu, size_t size, enum xhci_dma_sync_op op)`。`enum xhci_dma_sync_op` 为 `XHCI_DMA_TO_DEVICE`（clean）、`XHCI_DMA_PREPARE_FROM_DEVICE`（交接前消除脏线）、`XHCI_DMA_COMPLETE_FROM_DEVICE`（invalidate 并完成屏障）。核心封装偏移和长度检查，coherent ops 也执行所需的内存屏障。设备地址验证含 AC64、对齐、溢出、DMA 边界和连续长度。
- aarch64 的 `aarch64_pcie_prepare()` 在 `smp_starting_enter()` 前复用已就绪的 ECAM 寻找根总线直连的 `1b36:000d/0x0c0330`。用只读 config space 取得已赋值的 BAR0，合并 64 位 BAR 半部，经 DT 的 MMIO32/MMIO64 范围转换并验证 16 KiB 全区间。只映射该区间所占的 Device 2 MiB 页，继续执行已有 UEFI RAM、Boot Block、PA 上限和既有页表属性检查。以 `domain/BDF`、CPU PA、长度记录精确授权区间，失败或超过控制器数量上限时不发布记录。保留原有 MMIO32 预映射，完整 MMIO64 窗的 `mapped` 标志不启用。`bool aarch64_pcie_xhci_bar0_ready(const struct pci_device *pdev)` 在 Phase 6 核对该 BDF 的当前 BAR0 CPU PA 与早期记录，随后 `pci_iomap_range()` 只读查找该完整范围。
- `int xhci_pci_probe_aarch64(struct pci_device *pdev)` 承载 aarch64 的接管顺序，同一函数供 `OS01_HOST_TEST` 的 fake PCI 后端直接调用。先禁用 BME，打开 MEM decode，确认 `USBCMD.RS=0`、`USBSTS.HCH=1`，检查 CNR 后 reset。随后编程 DMA 地址和 interrupter，开启 BME，再设置 RS。新路径保存 PCI command 原值，安全失败后仅在固件与 OS ring 都已停且可回滚时恢复安全位，失败路径保持 bus master 关闭。用 `pci_config_read32(pdev, 0x04, &value)` 保存 command，写回通过 `pci_config_write32(pdev, 0x04, command & 0xffffu)` 使 Status W1C 位为零。出现不安全结果时保持 bus master 禁用并确认读回，保留 owner。x86_64 维持原来的接管流程。
- Stage 6 中 `rp1-xhci` 依赖 `device-boot`，aarch64 `usb-msc` 依赖 `rp1-xhci` 和现有 `virtio-blk`，确保双盘试验明确可生成 virtio 首盘。profile 上没有 RP1 或没有 virtio 盘仍由 provider 正常返回。控制器级的 DMA 失控分别经 PCI `DEVICE_UNSAFE` 和 optional 子系统内的 `device_fail_unsafe()` 升级。仍安全的 slot 级失败按现有隔离和撤销语义处理。

- [ ] **Step 1** 增加宿主夹具。断言 `phys=0x100000`、`bus=0x2100000` 时实际 DCBAAP/ERST/Link TRB 和事件关联均使用 `0x2100000 + offset`，free 仍使用 CPU PA。对 AC64=0 的 `bus=0x100000000`、跨 64 KiB 的对象、尾部溢出和非法 doorbell 读写跨度，断言报错且没有 ring 发布。ECAM fake 断言 `0x8000000000` 对应的 BAR0 只映射一个 2 MiB 页，越过精确 16 KiB 授权、未匹配 PCI ID、BAR 重定位、MMIO64 范围不足、与 RAM 重叠时均拒绝。PCI mock 初始 Status 放入非零的 W1C 位，断言每次 command 写入的高 16 位为零。trace 顺序断言 `clean payload → clean CYCLE → doorbell`、`prepare IN → doorbell → invalidate event → invalidate IN → copy`，涵盖回绕和短 IN。让 fake controller 在 reset 前存活、start 后超时、枚举时失控、BOT 试探时失控，分别断言 bus master 状态、隔离资源和正确的停机入口。
- [ ] **Step 2** 按宿主元数据流程生成清单后运行 `make PROFILE=x86_64-clang test-host`。预期新增用例因 DMA 接口或行为缺失判红。历史 USB-only 基线已观察到 `fs/root rc=-19`。
- [ ] **Step 3** 完成 DMA 对象迁移、ARM 屏障和 PCI 接入。覆盖所有控制器及 slot 对象、scratchpad、EP0、bulk 64 KiB staging 和被复用的 interrupt IN ring，避免 CPU PA 从旧字段流入硬件。中断保持屏蔽；aarch64 的 reset/port recovery 引入正常 clocksource deadline。xHCI capability header 至少 0x20 字节，并验证所有可用端口、interrupter、doorbell（含允许的最大 slot）及每次扩展能力访问的边界。在 aarch64 的显式 `KERNEL_C_SOURCES` 注册 `driver/xhci.c`、`xhci_pci.c`、`xhci_dma.c`、`xhci_hub.c`、`xhci_hid_desc.c`、`xhci_msc.c`、`usb_msc.c`、`usb_msc_bot.c`。HID detach 只由 x86 前端调用。把 `usb_msc_probe()` 成功日志中的 `%llu` 改为 `%lu` 并传递 `unsigned long`，逐条检查新链接路径中的格式和变参类型，保留 x86 既有日志事件与宽度。启动 MSC 通过现有 Phase 6 依赖图接通。
- [ ] **Step 4** 运行 `make PROFILE=x86_64-clang test-host test-header-object-audit test-driver-model-boundary` 与 `make PROFILE=aarch64-clang aarch64-uefi`。运行 `make PROFILE=aarch64-clang test-aarch64-audit` 核对 arm64 ELF 的边界与栈开销。独立运行下列 USB-only smoke。预期 Stage 3 映射高地址 BAR，开 MEM 后读到 HCH，之后出现 MSC 发布与 USB GUID 选盘。观察命令到达 30 s 后由 timeout 返回 124，按上述串口证据判定。现有固件基线实测 PCI command 为 `0x0`、BAR0 为 `0x8000000000`，仅开 MEM 后 `USBCMD=0x0`、`USBSTS=0x9`。这些地址和寄存器值用于诊断，运行代码取实际读回。
  ```bash
  os01_usb_probe_dir=$(mktemp -d)
  qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512 \
    -display none -machine "dumpdtb=$os01_usb_probe_dir/virt.raw"
  dtc -I dtb -O dtb -o "$os01_usb_probe_dir/virt.dtb" "$os01_usb_probe_dir/virt.raw"
  timeout 30s qemu-system-aarch64 -M virt,gic-version=2,acpi=off \
    -cpu cortex-a53 -smp 1 -m 512 -dtb "$os01_usb_probe_dir/virt.dtb" \
    -drive if=pflash,format=raw,readonly=on,file=build/aarch64-clang/firmware/QEMU_EFI.fd \
    -drive if=none,format=raw,readonly=on,file=build/aarch64-clang/image/aarch64-uefi.img,id=usb \
    -device qemu-xhci,id=xhci,p2=4,p3=0 \
    -device usb-storage,drive=usb,bus=xhci.0,port=1,bootindex=0 \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-device,rng=rng0 \
    -serial stdio -display none -monitor none -no-reboot -no-shutdown
  ```

- [ ] **Step 5** 提交 `feat(aarch64): boot USB mass storage on QEMU PCI xHCI`。

### T3 · 自动化验证 USB-only、双盘与 ESP 错误类型

**文件**

- 修改 `qemutests/harness/qemu_cmd.py`、`mk/components/run.mk`、`docs/build/build.md`。新建 `qemutests/aarch64_usb_boot.py`、`qemutests/test_aarch64_usb_boot.py`，在 `config/test-scenarios.json` 登记 `drivers.usb.aarch64.boot`。套件采用专用顶层入口 `test-aarch64-usb-boot`，与已有 `test-aarch64-pcie` 同样进行 aarch64/uefi 与 QEMU 平台门控。在单测模块用 `TEST_MANIFEST` 声明 `layer=harness`、`subsystem=drivers`、`tier=pr` 及关联场景，再运行扫描器重新生成 catalog。
- 复用 `qemutests/aarch64_uefi_smp.py::generate_diagnostic_dtb`、`qemutests/usb_boot.py::prepare_image`、`qemutests/harness/process.py::ProcessSession` 和 `qemutests/harness/result.py::RunArchive`。

**接口**

- 为 `build_qemu_aarch64()` 加 keyword `storage: str = "virtio"`、`decoy_image: str | None = None`。支持 `virtio`、`usb`、`usb+virtio`。USB 组合的 pflash 和 guest drive 以只读打开，`qemu-xhci,id=xhci,p2=4,p3=0` + `usb-storage,drive=usb,bus=xhci.0,port=1,bootindex=0`。双盘加 `virtio-blk-device,drive=decoy,bootindex=1`。使用 `-M virt,gic-version=2,acpi=off`、512 MiB、1 CPU、成对 RNG 与匹配 DTB。
- runner 接受 `--case {all,usb-only,dual,wrong-esp-type}`、`--qemu`、`--firmware`、`--image`、`--build-dir`、`--profile`。顶层 `AARCH64_USB_BOOT_CASE` 默认 `all`。各 case 独立私有副本和归档，超时 120 s。runner 返回 0 为通过、1 为运行失败或超时、2 为环境或参数错误。
- `check_case(case: str, serial: str, expected_guid: str, root_marker: str, esp_marker: str) -> list[str]` 返回违例列表。去除终端控制序列及 CR 后，只从行首 `UEFI: ESP GUID=` 开始识别交接记录。逐 token 消费整行，token 为重复的帧前缀、可选空白或固定 36 字符的 UUID。允许 UUID 直接相邻。每条记录至少含一个 UUID，总体每个 UUID 都须等于预期值，余下任何无法解码的字符算失败。正例先见 BusyBox ash banner 和行首 `# ` 提示符，发送 `/bin/busybox cat /usb-root.txt /boot/USBESP.TXT` 后要求两个分区标记各独立成行和新提示符。核对 BOT、512 B USB 块、`first_disk`、根盘源名／ESP 源名与 `/boot ok` 按序出现。

- [ ] **Step 1** 新增 Python 单测。断言 USB-only argv 只有一块盘，默认 argv 沿用 virtio，dual 的 bootindex 与首盘证据可同时成立。构造串口 fixture，复刻串口实录的 `PREFIX PREFIX GUID GUID` 直接相邻形态（`PREFIX` 为 `UEFI: ESP GUID=` 且双 UUID 之间无空格），两份相同 UUID 应通过。覆盖双 UUID 相邻但第二个不同、尾随多余字符、不同 GUID 分行混入、缺少 MSC、标记只在命令回显中、没有第二个提示符、晚到 fatal。负例必须看到 USB MSC 发布、首盘 `hda`、指定 GUID 匹配失败和 `rc=-2`，shell 或其他 fatal 均失败。
- [ ] **Step 2** 运行 `python3 -m unittest qemutests.test_aarch64_usb_boot`。应因新 runner 或拓扑接口缺失失败。
- [ ] **Step 3** 实现 runner 与镜像准备。先用 `prepare_image()` 注入相异标记。在 dual 与 wrong case 中用 `sfdisk --disk-id` 和 `--part-uuid` 使 virtio 镜像的 GPT 磁盘及 ESP UUID 唯一。wrong case 用 `sfdisk --part-type USB 1 EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` 改 USB 分区类型，验证原 GUID、LBA 和 FAT 分区摘要相同。正例第二次提示符或负例预期 fatal 到达后，再观察 1 s 并 drain 串口，对最终 transcript 重跑断言。提前退出、额外 fatal 或 panic 算失败。所有用例记录预期 GUID、完整 argv、日志与结果，比较运行前后的输入镜像及固件哈希。
- [ ] **Step 4** 运行 `python3 -m unittest qemutests.test_aarch64_usb_boot qemutests.test_harness_qemu_cmd`、`make PROFILE=aarch64-clang test-aarch64-usb-boot`、`make PROFILE=aarch64-clang test-aarch64-pcie` 和 `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product`。预期 USB 三组全通过，PCIe/AHCI 和普通 virtio 根盘继续可用。验证错误 profile 与 `AARCH64_PLATFORM=rpi5` 调用 QEMU 目标时在构建前直接报错。
- [ ] **Step 5** 提交 `test(aarch64): verify USB boot media identity in QEMU`。

### T4 · 接管已由 WoR 初始化的 RP1 USB 主控制器

**文件**

- 新建 `kernel/include/arch/aarch64/rp1_xhci.h`、`kernel/arch/aarch64/platform/dtb_parse_rp1.c`、`kernel/arch/aarch64/bus/rp1_pcie.c`、`kernel/arch/aarch64/driver/xhci_dma.c`。修改 `kernel/arch/aarch64/platform/dtb.c`、`kernel/arch/aarch64/boot/main.c`、`kernel/arch/aarch64/driver/rp1_xhci.c`。
- 将 `kernel/arch/aarch64/bus/pci.c` 的 M0 内存图和设备页校验提取到新文件 `kernel/arch/aarch64/memory/boot_mmio.c` 与 `kernel/include/arch/aarch64/boot_mmio.h`，供 ECAM 与 RP1 共用。保留既有 ECAM 映射及只在成功后发布范围的行为，并更新对应 host link group。
- 修改 `kernel/memory/pmm.c`、`kernel/include/memory/pmm.h`。新增 `hosttests/cases/test_aarch64_rp1_xhci.c`，在 `hosttests/cases/test_pmm_boot_reservation.c` 内添加范围测试，登记其链接组与场景。DTB 和 RC register fixture 仅保存覆盖合同所需的最小数据。

**接口**

- `int aarch64_dtb_parse_rp1_xhci(const void *blob, uint32_t size, struct aarch64_rp1_xhci_desc *out)`。描述符保存 RC CPU `reg` 基址与长度、两个带 enabled 的 USB slot 和各自相对 `rp1 ranges` 子起点的偏移及长度。无匹配 `-ENODEV`，结构无效或重叠/歧义 `-EINVAL`，错误清空输出。独立静态存储供 Stage 3 使用，避免增大 `dtb_init()` 栈帧和 `struct aarch64_platform_info`。
- `int aarch64_rp1_pcie_resources(const struct aarch64_rp1_xhci_desc *desc, const struct aarch64_rp1_pcie_io *io, struct aarch64_rp1_pcie_resources *out)`。IO 对象提供 RC 寄存器 32 位读取和 selector 的保存、选择、恢复。输出 BAR1 的 CPU 窗与允许分配的 DMA PA 半开区间。共享一份生产解码供 fake MMIO 单测执行。
- `int aarch64_boot_mmio_ctx_init(const struct boot_context *boot, struct aarch64_boot_mmio_ctx *out)` 验证内存图与当前 TTBR1 root。`int aarch64_boot_mmio_map(const struct aarch64_boot_mmio_ctx *ctx, uint64_t base, uint64_t size)` 复用已有 Device block 映射与全区间安全校验。
- `int aarch64_rp1_xhci_prepare(const struct boot_context *ctx)` 在 Stage 3 M1 后完成映射及资源验证。在存储阶段的 `rp1-xhci` 初始化中使用准备好的窗口，依次验证 DWC3 和 HCH，经 T1/T2 的核心 API 接管。每个控制器单独记录成功或安全拒绝。报告控制器级 DMA 不安全时调用 `device_fail_unsafe()`，不把错误留给 optional 返回值。
- `struct Page *alloc_pages_in_range(int32_t zone_select, uint64_t number, uint64_t page_flags, uint64_t min_pa, uint64_t limit_pa)` 和 `uint64_t alloc_4k_page_in_range(uint64_t min_pa, uint64_t limit_pa)` 以 `[min_pa, limit_pa)` 限整个分配，保持旧分配入口和释放 API。4 KiB 路径搜索范围内的现有子页池，必要时从合格的完整 2 MiB 页建立新池。RP1 adapter 以受限 4 KiB 页及 2 MiB frame 内对齐的 64 KiB staging 提供 DMA 对象，使用相应的原分配 token 回收。

- [ ] **Step 1** 编写红灯 fixture。有效 DWC3 两节点必须解析 `0x200000/0x300000` 和 `0x100000` span，禁用节点不可见，重复范围、异常 PCI cell 或挂错 RC 应拒绝。WoR fixture 的 DTB 声明 high DMA、运行 RC identity inbound 时应选择低窗，计算 BAR1 从 PCI 总线窗口到 CPU 窗的地址，并排除一个 `[0xb0000000,0xc0000000)` 的 outbound fixture 后得到上限 `0xb0000000`。验证链路 down 时没有走间接配置，失败和成功都恢复 selector，BAR1 失配或 RAM/type 冲突时没有 DWC3 MMIO。对已处运行态的 RP1 控制器分别模拟停机成功与超时，均须调用 `device_fail_unsafe()`。断言没有 HCRST、新 ring 写或块发布，停机未获确认时无释放操作。PMM 断言 4 KiB 与连续底层页各自整段在界内，空窗或耗尽零返回，隔离对象不回收。
- [ ] **Step 2** 运行 `make PROFILE=x86_64-clang regen-test-catalog test-host`。预期新 fixture 对现有代码缺接口或缺行为。
- [ ] **Step 3** 实现描述符发现与运行时校验。按批准 spec 的 WoR 源码解码 RC 链路、secondary bus、RP1 vendor/device、BAR1 类型与地址、bridge memory window、outbound CPU/PCI 范围和 inbound BAR2/UBUS remap/readback。禁止 sizing 写和桥配置重编程。给每个将映射的向外取整 2 MiB 单元做 RAM、Boot Block、地址上限与已有属性检查。读取 GSNPSID 和 host-only 或 GCTL host 模式后，首次核对 `RS=0` 和 `HCH=1`。若已有控制器仍在运行，尝试停止，保留未能确认释放所有权的资源，并直接调用 `device_fail_unsafe()`。无需触及全 RP1 的 bus master 状态。DT/BAR/DWC3 前置条件无法安全验证时，经 no-op provider 的同一安全返回路径诊断拒绝。只在确认 controller ownership 后执行 HCRST 与新 ring 编程，保留共享 RP1 串口。
- [ ] **Step 4** 实现受限 DMA 分配和 non-coherent ops，并连接 provider。用 `CTR_EL0` DminLine 遍历隔离的 cache line，分别用 `dc cvac`、`dc civac`、`dc ivac` 实现三个操作，执行到 PoC 的完成屏障。控制器地址限制为低窗内等同 PA。先把真实窗口与拒绝原因纳入 `[rp1-xhci]` 运行日志，再记录实际 VID:PID、速度和端口。
- [ ] **Step 5** 运行 `make PROFILE=x86_64-clang test-host check-test-catalog`、`make PROFILE=aarch64-clang AARCH64_PLATFORM=rpi5 aarch64-uefi`、`make PROFILE=aarch64-clang AARCH64_PLATFORM=qemu-virt test-aarch64-usb-boot`。构建 Dragon Q8B 镜像并核对 provider 可在 ACPI-only 路径安全缺席。自动测试对 WoR 窗口正确解码和所有拒绝条件变绿，RPi5 真机结果由 T5 补齐。
- [ ] **Step 6** 提交 `feat(aarch64): attach WoR-configured RP1 USB hosts`。

### T5 · RPi5 真机取证与最终回归

**文件**

- 新建 `docs/driver/aarch64-usb-boot-qualification.md` 记录命令和结果。另更新 `docs/build/build.md`、`docs/driver/driver.md` 与 `docs/roadmap.md` 的相关项。
- 板测时从 `build/aarch64-clang/image/rpi5/aarch64-uefi.img` 生成私有副本。归档制备命令、输入／输出摘要、设备标识和原始串口记录，镜像和原始大日志留在测试结果目录。

- [ ] **Step 1** 在私有镜像用 T3 的标记注入方式写入两处分区标记，用 `debugfs` 将 inittab 改成 `tty1:respawn:/bin/sh /etc/usb-boot-check.sh`。脚本执行下面的证据链。先在 QEMU 由 virtio 启动同一脚本，验证终端路径及真正的 ash prompt。现有产品镜像已用这条路径在 QEMU 读回两个试验标记并打印提示符。

  ```sh
  exec </dev/tty >/dev/tty 2>&1
  /bin/busybox cat /usb-root.txt || exit 1
  /bin/busybox cat /boot/USBESP.TXT || exit 1
  export PS1='OS01-USB# '
  exec /bin/sh -i
  ```

- [ ] **Step 2** 准备 RPi5 Model B、WoR v0.3 DT 模式、仅含平台固件及 VideoCore 配置的 TF 卡、直插 High-Speed BOT U 盘和 RP1 115200 8N1 串口。烧录前按容量与序列号核对被选中的测试 U 盘。从 UEFI 的 USB 启动项启动；记录 Raspberry Pi 板型、EEPROM、WoR/DTB 版本、实际 BAR1／outbound／inbound、DWC3 core ID、xHCI HCH、DMA 窗、端口和 U 盘 VID:PID／速率。
- [ ] **Step 3** 做两次完整断电的正例冷启动，逐次匹配验收盘 GUID → 内核 RP1 接管与 High-Speed BOT → MSC 块发布 → 选同盘 GPT → 两个不同标记 → `OS01-USB#`。再移除 USB 冷启动，证明固件 TF 卡上不存在可启动 OS01。若硬件安全拒绝，归档具体读回和拒绝点；保持板测为待完成，并对照设计范围确定后续板级工作。
- [ ] **Step 4** 运行 `make PROFILE=x86_64-clang test-host test-harness check-test-catalog`，两条 x86 USB E2E，以及 `make PROFILE=aarch64-clang test-aarch64-usb-boot test-aarch64-pcie`、`make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product`。预期自动化矩阵全部有独立 PASS 证据和未变的源镜像摘要。真机结果单独入表，只有板测全部实跑通过才关闭 RPi5 的路线图阻断。
- [ ] **Step 5** 提交 `docs(aarch64): record USB boot qualification`，关联 QEMU 结果和可取得的真机证据。

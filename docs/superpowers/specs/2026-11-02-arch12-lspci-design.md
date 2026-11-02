# ARCH-12 lspci PCI 设备列举命令 — 设计规格

- 日期：2026-11-02
- 状态：已评审批准，待实施
- 对应 roadmap：`docs/roadmap.md` 的 ARCH-12「lspci PCI 设备列举命令」
- 前置：ARCH-9（驱动模型/PCI/总线解耦）✅、ARCH-11（devfs 子目录化）✅

---

## 1. 目标与范围

### 1.1 问题

用户态**没有任何 PCI 设备列举入口**。QEMU 调试、驱动绑定验证、故障注入的证据收集，
只能翻 kernel 日志里的 `log_info`。roadmap 要求补齐与 Linux `lspci`
（pciutils，读 `/sys/bus/pci/devices/<BDF>/{vendor,device,class,config,resource,irq}`）
对齐的能力。

### 1.2 现状复核结论（关键）

roadmap ARCH-12 的原始方案是：

> 新 `user/lspci.c`：`syscall(SYS_pci_count)` 取 N → 循环 `pci_device_get(i)` 拿 BDF
> → 逐项 `open`+`read` `/dev/pci/<BDF>/<attr>`（依赖 ARCH-11）

该方案假设需要**新增 2 个 syscall**（`SYS_pci_count` + 一个取 BDF 的接口）并改动 UAPI。
逐项复核后该前提**已过时**：

1. `SYS_pci_count` / `SYS_pci_info` **不存在**——内核从未暴露过 PCI 枚举的 syscall。
2. 但 **ARCH-11 已落地**：devfs 现在支持 `/` 分隔的多级路径，中间分量返回真实目录节点。
   因此用户态可以**直接用目录枚举拿到 BDF 列表**：

   ```
   readdir("/dev/pci")         → BDF 目录名（"0000:00:1f.2" …）
   readdir("/dev/pci/<BDF>")   → 属性文件名（"vendor" "config" …）
   read("/dev/pci/<BDF>/vendor") → "0x8086"
   ```

3. 用户态枚举能力齐备：`libc/unistd/dirent.c` 实现了 `opendir`/`readdir`/`closedir`，
   `struct dirent` 带 `d_type`（可区分 `DT_DIR` 与 `DT_REG`），
   `SYS_getdents64`（`kernel/include/uapi/syscall.h:34`）已在 syscall 表内。

**因此本项零新增 syscall、零 UAPI 变更**——ARCH-11 把「枚举」这一步从 syscall 挪到了
文件系统命名空间里，这正是它作为 ARCH-12 前置条件的价值。

### 1.3 目标

1. 内核为每个已枚举的 PCI 设备注册 `/dev/pci/<BDF>/<attr>` 节点（多级路径，ARCH-11 提供）。
2. 属性集：`vendor` / `device` / `class` / `config` / `resource` / `irq` / `driver`。
3. 新增用户态 `/bin/lspci`：默认一行摘要（对齐 Linux lspci 格式），支持
   `-x`（config space hex dump）、`-n`（不做名称解析）、`-v`（详细信息）。
4. 内嵌 pci.ids 子集 + class 名表供名称解析；无匹配时回退 `8086:100e` 形式。
5. hosttest 验证每个属性文件的内容，QEMU 中 `/bin/lspci` 实跑。

### 1.4 非目标（明确不做）

- **不新增任何 syscall**，不改 `kernel/include/uapi/`（§1.2）。
- **不实现** Linux lspci 的全部选项：不做 `-s`（域/总线/槽过滤）、`-t`（树形输出）、
  `-M`（厂商过滤）、`-d`（不显示默认）、`--namespace` 等。
- **不实现** `pci.ids` 完整数据库，只做覆盖 QEMU 常见设备的子集。
- **不实现** 运行期 PCI 热插拔（`/dev/pci` 是启动期快照）。
- **不实现** `resource` 文件的完整 Linux 格式（只给 BAR 列表，不给逐寄存器转储）。
- **不修改** `kernel/fs/vfs.c`、`kernel/fs/devfs.c`、`kernel/bus/pci/core.c`、
  `kernel/arch/x86_64/bus/pci.c` 的既有行为。

---

## 2. 现状源码依据

| 事实 | 依据 |
|------|------|
| PCI 枚举在 subsys phase 6 完成，早于 devfs 挂载 | `kernel/device/boot.c:195` 注册 `register_subsys("device-boot", …, SUBSYS_PHASE_6, 0)`；`kernel/core/main.c:146` 的 `fs_boot_prepare()` 在 subsys 阶段之后 |
| `fs_boot_prepare()` 是 `devfs_init()` 的调用点 | `kernel/fs/boot.c:30-35` |
| 设备访问 API | `pci_device_count()`（`kernel/bus/pci/core.c:493`）/ `pci_device_get(i)`（`kernel/bus/pci/core.c:498`）、`pci_config_read32(pdev, offset, out)`（`core.c:573`，offset 0..252、4 字节对齐 → 完整 256 字节 config space）、`pci_route_gsi(pdev, &gsi)`（facade `core.c:658`，x86 实现 `kernel/arch/x86_64/bus/pci.c:87`，PIRQ[A-D] → GSI[16-19]） |
| BDF 与 ID 字段 | `struct pci_device{domain,bus,slot,fn,vendor,device,subvendor,subdevice,class_code,bars[6],driver}`（`kernel/include/bus/pci/pci.h:57`） |
| 绑定驱动名 | `struct pci_driver{ const char *name; … }`（`kernel/include/bus/pci/driver.h:8`） |
| config space 访问并发安全 | `x86_pci_read32()` 用 `s_pci_lock`（irqsave）保护 CF8/CFC 配对（`kernel/arch/x86_64/bus/pci.c:52`） |
| devfs 多级路径注册 | `devfs_register_path()`（ARCH-11 引入，`kernel/include/fs/devfs.h`） |
| devfs 设备 read 回调的 buffer 是内核内存 | `fd_read` 的 kernel-bounce 语义（`kernel/fs/file.c`），设备回调收到内核缓冲区 |
| 用户态目录枚举 | `libc/unistd/dirent.c`：`opendir`/`readdir`/`closedir`；`struct dirent.d_type` |
| 用户程序自动进 rootfs | `user/apps.mk` 的 `USER_FLAT_SOURCES` 自动收 `user/*.c`；`config/rootfs.mk:5` 用 `$(USER_PROGRAMS)` 生成 `/bin/<name>` |

---

## 3. 内核侧设计

### 3.1 模块位置

新文件（遵守 AGENTS.md 目录规范：源目录 ↔ 头目录对称、arch-neutral）：

- `kernel/bus/pci/pci_devfs.c`
- `kernel/include/bus/pci/pci_devfs.h`

放在 `bus/pci/` 而不是 `fs/`：这是 PCI 子系统对外暴露命名空间，不是文件系统实现。

### 3.2 对外接口

```c
// kernel/include/bus/pci/pci_devfs.h
#ifndef _BUS_PCI_PCI_DEVFS_H
#define _BUS_PCI_PCI_DEVFS_H

#include <stdint.h>
#include <stddef.h>
#include <bus/pci/pci.h>

// Attribute ids — the order matches the registration table below.
typedef enum {
    PCI_ATTR_VENDOR = 0,
    PCI_ATTR_DEVICE,
    PCI_ATTR_CLASS,
    PCI_ATTR_CONFIG,
    PCI_ATTR_RESOURCE,
    PCI_ATTR_IRQ,
    PCI_ATTR_DRIVER,
    PCI_ATTR_COUNT,
} pci_attr_id_t;

// Register /dev/pci/<BDF>/<attr> for every enumerated PCI device.
// Returns the number of attribute nodes published, or a negative errno.
// Safe to call once after devfs is mounted; a second call is a no-op that
// returns the published count.
int pci_devfs_register_all(void);

// Register /dev/pci/<BDF>/<attr> for ONE device.
// Exposed so the host test can publish a synthetic device without a PCI
// enumeration.  Returns the number of attribute nodes PRESENT after the
// call (an already-existing node counts as present, so the call is
// idempotent), or a negative errno for an invalid argument.
int pci_devfs_register_device(const struct pci_device *pdev);

// Render one attribute of one device into `buf` (raw bytes, NOT
// NUL-terminated).  Returns the TOTAL content length in bytes — which may
// exceed `size` when the content was truncated, so the caller always
// learns the stream length — or a negative errno.
// Exposed for the host test so the formatting logic is testable without
// touching devfs.
int pci_devfs_format_attr(const struct pci_device *pdev, pci_attr_id_t attr,
                           char *buf, size_t size);

#endif
```

`pci_devfs_format_attr()` 单独暴露，让 hosttest 能直接验证每个属性的**内容**
（字符串与字节），不必经由 devfs 往返。

`pci_devfs_register_device()` 是 `pci_devfs_register_all()` 的单设备版本。两者分离的
理由：`register_all()` 依赖 PCI 枚举结果（`pci_device_get()`），而 hosttest 要在没有
真实枚举的情况下发布一个**合成设备**并验证它真的出现在 devfs 树里。

### 3.2.1 属性长度

各属性的内容长度：

| 属性 | 长度 | 是否固定 |
|------|------|----------|
| `vendor` / `device` / `class` | `"0x…\n"` 的实际字节数 | 否 |
| `config` | 256 | 是 |
| `resource` | 各 BAR 行的字节和 | 否 |
| `irq` | `"<gsi>\n"` 或 `"none\n"` | 否 |
| `driver` | `"<name>\n"` 或 `"none\n"` | 否 |

### 3.3 注册模型

每个属性节点一条 devfs 记录，`private_data` 指向一个属性上下文：

```c
typedef struct pci_attr_ctx {
    struct pci_device *pdev;
    uint8_t            attr;      // pci_attr_id_t
} pci_attr_ctx_t;
```

注册调用（ARCH-11 的多段 API）：

```c
devfs_register_path("pci/0000:00:1f.2/vendor", ctx, &pci_attr_ops);
```

- 路径相对 `/dev`，3 段（`pci` / `<BDF>` / `<attr>`），深度 ≤ `DEVFS_MAX_DEPTH`(16) ✓
- BDF 段名格式 `dddd:dd:dd.d`（domain:bus:slot.fn），最长 12 字节 < `DEVFS_NAME_MAX`(32) ✓
- 中间目录 `pci` 与 `pci/<BDF>` 由 devfs **隐式创建**并作为真实目录出现在 readdir 中 ✓

所有属性共用**一张** `devfs_ops` 表，read 回调按 `ctx->attr` 分派——避免为每个属性
维护一张 ops 表。

### 3.4 read 回调

```c
static int pci_attr_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
```

- `buffer` 是**内核内存**（`fd_read` 的 kernel-bounce 语义）→ 直接 `memcpy`，无需 uaccess 包装。
- 属性上下文从节点取回：`devfs_get_private(node)` → `pci_attr_ctx_t *`。
  （这是 devfs 的既有约定：`devfs_get_private()` 解析 `node->fs_data` 指向的 devfs 记录
  并返回其 `private_data`。）
- 实现是「**先整体格式化，再按 offset 切片**」：
  1. 在栈上开一个 512 字节缓冲 `tmp`；
  2. `len = pci_devfs_format_attr(pdev, attr, tmp, sizeof(tmp))`；
  3. `offset >= len` → 返回 0（EOF）；`size == 0` → 返回 0；
  4. `n = min(size, len - offset)`，`memcpy(buffer, tmp + offset, n)`，返回 `n`。
- 512 字节足够容纳最长的属性内容：`config` 恰好 256 字节；`resource` 最长
  6 行 × ≈ 40 字节 ≈ 240 字节。**不分配动态内存**——read 路径零分配。
- `config` 属性内容是 256 字节原始 config space（不走文本格式化）。

### 3.5 各属性的内容与格式

| 属性 | 内容 | 格式 | 长度 |
|------|------|------|------|
| `vendor` | vendor id | `"0x8086\n"` | 8 |
| `device` | device id | `"0x2922\n"` | 8 |
| `class` | 24-bit class code | `"0x010601\n"` | 10 |
| `config` | 256 字节 config space | 原始字节 | 256（固定） |
| `resource` | BAR 列表 | 每行一个 BAR | ≤ 256 |
| `irq` | 中断 pin / GSI | `"16\n"` | ≤ 4 |
| `driver` | 绑定的驱动名 | `"ahci\n"`；未绑定时 `"none\n"` | ≤ 32 |

**`resource` 的行格式**（对齐 Linux `/sys/.../resource` 的可读子集）：

```
Region 0: I/O ports at 0xc000
Region 2: Memory at 0xfebf0000
```

- BAR kind → 文本：`PCI_BAR_IO` → `I/O ports`；`PCI_BAR_MMIO32` / `PCI_BAR_MMIO64` → `Memory`；
  `PCI_BAR_NONE` / 其它 → 跳过。
- 只输出 `bar->valid` 的 BAR，编号沿用 BAR 索引（0..5）。
- **不输出 `(size=...)`**：ARCH-9 契约明确「BAR 不做破坏性 sizing，length 是 driver 已知
  寄存器窗口而不是无损探测出的 BAR 容量」，本子系统无从得知 BAR 容量——编造 size 会
  给出误导性输出。只报地址与类型。

**`irq`**：用 `pci_route_gsi(pdev, &gsi)` 取得路由后的 GSI（x86 backend 会把
PIRQ 映射到 GSI 16..19）。取不到时输出 `"none\n"`。

**`driver`**：`pdev->driver ? pdev->driver->name : "none"`。

### 3.6 注册时机与错误处理

- 调用点：`kernel/core/main.c` 的 `fs_boot_prepare()` **之后**一行。
  此刻 devfs 已挂载（在 `/dev`），且 PCI 设备已在 subsys phase 6 枚举完毕。
- **注册失败不阻断启动**：单个属性注册失败只跳过该节点并 `log_warn` 记录；
  `pci_devfs_register_all()` 返回已发布的节点数。
  理由：lspci 是诊断工具，不是启动必需设施；启动必需设备的注册失败策略见
  `kernel/fs/boot.c` 的既有做法。
- **errno 分类**：
  - `-EEXIST`：节点已存在（重复注册）→ **不算失败**，计入「已发布」计数，**不记日志**
    （否则幂等重入会刷屏）。
  - `-ENOMEM` / 其它负 errno：真失败 → `log_warn` 记录并从计数中扣除。
- 重复调用是幂等的：`pci_devfs_register_all()` 用一个静态标志 + 计数，第二次调用
  直接返回已发布计数，不再重新走注册路径。
- **属性上下文对象永不释放**（与 ARCH-10「devfs 记录存活至关机」一致）：
  `pci_attr_ctx_t` 随 devfs 记录一起存活。
- **`config` 属性的读放大**：`config` 每次 read 都要跑 64 次 `pci_config_read32`
  （每次一对 CF8/CFC 端口 I/O）。这是 Linux sysfs 的同款行为；x86 backend 用
  `s_pci_lock`（irqsave）保护 CF8/CFC 配对，读的是只读寄存器。
  用户态 `lspci -x` 会一次性读满 256 字节，因此每设备 64 次端口 I/O 可接受。

---

## 4. 用户态设计

### 4.1 文件与构建

- 新文件 `user/lspci.c`（平铺程序，`user/apps.mk` 的 `USER_FLAT_SOURCES` 自动收录）。
- **无需改 `config/rootfs.mk`**：`ROOTFS_USER_PROGRAMS` 由 `$(USER_PROGRAMS)` 派生，
  `/bin/lspci` 自动生成。
- 无新增 libc 依赖：`opendir`/`readdir`/`closedir`/`open`/`read`/`close`/`printf` 已有。

### 4.2 枚举流程

```
opendir("/dev/pci")
  → 每个 d_type == DT_DIR 的条目 = 一个 BDF
      opendir("/dev/pci/<BDF>")
        → 每个 d_type == DT_REG 的条目 = 一个属性文件
            open("/dev/pci/<BDF>/<attr>") + read()
```

- `/dev/pci` 不存在（内核未注册 PCI 命名空间，例如无 PCI 总线的架构）→
  打印一行诊断到 stderr 并以非零码退出。
- 某个 BDF 目录读不出属性 → 跳过该设备并继续（不因单个设备失败中止整表）。
- 设备按 BDF **字典序**输出（`readdir` 顺序不保证，排序后输出稳定）。

### 4.3 输出格式

**默认**（对齐 Linux lspci）：

```
00:1f.2 SATA controller: Intel Corporation 82801IR/IO/IH SATA AHCI Controller (rev 00)
```

格式：`BB:DD.F <class name>: <vendor name> <device name> (rev RR)`

- `BB:DD.F` = bus:slot.function（省略 domain，与 Linux lspci 默认一致）
- class name 由 24-bit class code 查表（§4.4）
- vendor/device name 由 §4.4 的两级查表得到，回退规则见 §4.4 的表格
- `rev` 来自 config space 的 **byte 0x08**（revision ID）
- 名称解析失败时不省略整行——回退形式保证输出始终可用

**`-n`**（不做名称解析）：

```
00:1f.2 SATA controller: 8086:2922 (rev 00)
```

vendor/device 输出数字 ID；class 名保留（class 名来自 24-bit code 的固定映射，
不依赖 pci.ids 数据库）。

**`-x`**（config space hex dump）：

```
00:1f.2 SATA controller: Intel Corporation 82801IR/IO/IH SATA AHCI Controller (rev 00)
00: 86 80 22 29 07 00 00 02 00 00 00 00 08 00 00 00
10: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
...
f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

- 16 字节一行，行首是 2 位十六进制偏移，小写字节、空格分隔。
- 读不到 config space（`pci_config_read32` 失败）→ 打印一行 `config space unavailable` 并继续。

**`-v`**（详细信息）：

```
00:1f.2 SATA controller: Intel Corporation 82801IR/IO/IH SATA AHCI Controller (rev 00)
	Class: mass storage, subclass: SATA, ProgIF: 01
	IRQ: 16
	Kernel driver in use: ahci
	Region 0: I/O ports at 0xc000
	Region 2: Memory at 0xfebf0000
```

- `Class:` 行把 24-bit class code 拆成 base / subclass / ProgIF：
  `Class: <base 名>, subclass: <subclass 名>, ProgIF: <2 位十六进制>`。
  base 名用小写短名（`mass storage`、`network`、`display`、`bridge`、`serial bus` …）；
  subclass 名只对 base `0x01`（mass storage）细分（`IDE` / `SCSI` / `RAID` / `SATA` /
  `SAS` / `NVMe` …），其它 base 的 subclass 一律显示 `0x%02x`。
- `IRQ:` 行取 `irq` 属性；属性为 `none` 时打印 `IRQ: none`。
- `Kernel driver in use:` 行取 `driver` 属性；未绑定驱动时打印
  `Kernel driver in use: none`（**总是打印这一行**，输出因此是确定的）。
- `Region` 行直接来自 `resource` 属性的内容，每行前加一个 **TAB**。
- `rev` 取自 config space 的 **byte 0x08**；config 读不到时摘要行显示 `(rev ??)`。

### 4.4 名称解析表

**class 名表**（24-bit class code → 名称）。至少覆盖：

| class | 名称 |
|-------|------|
| 0x000000 | Unclassified device |
| 0x010601 | SATA controller |
| 0x0108?? | NVMe controller |
| 0x0100?? | IDE interface |
| 0x0200?? | Ethernet controller |
| 0x0300?? | VGA compatible controller |
| 0x0403?? | Multimedia video controller |
| 0x0600?? | Host bridge |
| 0x0601?? | ISA bridge |
| 0x0604?? | PCI bridge |
| 0x0c03?? | USB controller |

未知 class → `"Unknown class [XXYYZZ]"`。

**vendor:device 表**（子集，覆盖 QEMU q35 常见设备）：

| vendor:device | 名称 |
|---------------|------|
| 0x8086:0x100e | 82540EM Gigabit Ethernet Controller |
| 0x8086:0x10d3 | 82574L Gigabit Network Connection |
| 0x8086:0x2922 | 82801IR/IO/IH SATA AHCI Controller |
| 0x8086:0x2923 | 82801IR/IO/IH SATA AHCI Controller |
| 0x8086:0x29c0 | Q35 Express Chipset |
| 0x8086:0x7000 | 82371SB PIIX3 ISA |
| 0x8086:0x7010 | 82371SB PIIX3 IDE |
| 0x8086:0x7113 | 82371AB/EB/MB PIIX4 ACPI |
| 0x1234:0x1111 | QEMU Virtual Video Controller |
| 0x1af4:0x1000 | Virtio network device |
| 0x1af4:0x1001 | Virtio block device |
| 0x1af4:0x1041 | Virtio console |
| 0x1af4:0x1042 | Virtio modern block device |
| 0x1af4:0x1100 | Virtio GPU |
| 0x10de:0x10xx | NVIDIA Corporation |

**vendor 名表**：`0x8086` → `Intel Corporation`；`0x1af4` → `Red Hat, Inc.`；
`0x1234` → `QEMU/Bochs`；`0x10de` → `NVIDIA Corporation`；`0x1002` → `Advanced Micro Devices, Inc. [AMD/ATI]`；
`0x1b36` → `Red Hat, Inc. (QEMU Virtual Machine)`。

**回退规则**（两级，vendor 与 device 各自独立回退）：

| 情况 | 默认模式输出 | `-n` 模式输出 |
|------|--------------|----------------|
| vendor 命中、device 命中 | `<vendor 名> <device 名>` | `<vendor_hex>:<device_hex>` |
| vendor 命中、device 未命中 | `<vendor 名> <device_hex>` | `<vendor_hex>:<device_hex>` |
| vendor 未命中 | `<vendor_hex> <device_hex>` | `<vendor_hex>:<device_hex>` |

- `vendor_hex` / `device_hex` 是 **4 位小写十六进制、不带 `0x` 前缀**（如 `8086`、`2922`）。
- 未知 class → `"Unknown class [XXYYZZ]"`（`XXYYZZ` 是 6 位大写十六进制）。
- `0x10de`（NVIDIA）的 device 命中规则是**按 vendor 通配**：任何 `0x10de:*` 都回退为
  `"NVIDIA Corporation"`（对应 §4.4 表里的 `0x10de:0x10xx` 行）。

### 4.5 选项与退出码

| 选项 | 含义 |
|------|------|
| `-x` | 附加 config space hex dump |
| `-n` | 不做名称解析（vendor/device 用数字 ID） |
| `-v` | 详细信息（Class 细分、IRQ、驱动、Region） |
| `-h` | 用法帮助 |

- 三个选项可组合（`-x -v`）。
- 退出码：0 = 至少列出一个设备；1 = `/dev/pci` 不可用或未列出任何设备；2 = 用法错误。

---

## 5. 测试

### 5.1 新增 hosttest

分两个二进制——**属性层**与**devfs 集成层**分开，因为二者的链接面不同：属性层需要
PCI 的 mock backend（`arch9/pci_runtime.h` 一族），集成层需要 VFS 的 mock runtime
（`vfs_test_runtime.h` 一族），两套 force-include 的运行时互相不兼容。

#### A) `hosttests/cases/test_pci_devfs.c` → `test_pci_devfs`（`drivers` 类）

链接：`kernel/bus/pci/pci_devfs.c` + `kernel/bus/pci/core.c`（生产 TU）。
case TU 提供：

- mock `arch_pci_backend()`：一份 256 字节的合成 config space
  （vendor `0x8086` / device `0x2922` / class `0x010601` / rev `0x00` /
  BAR0 = I/O @ `0xc000` / BAR2 = MMIO @ `0xfebf0000` / 0x3C 处 int line+pin），
  以及 `route_gsi` → GSI 16。
- `g_log_level` 与 `_log_info_impl` / `_log_warn_impl` / `_log_err_impl` 桩。
- `devfs_register_path()` 桩：记录 `(path, private_data, ops)` 三元组。
- `devfs_get_private()` 桩：返回 `node->fs_data`（与生产语义同构）。

| 用例 | 验证 |
|------|------|
| `test_pci_devfs_attr_vendor_device_class` | `vendor` → `"0x8086\n"`；`device` → `"0x2922\n"`；`class` → `"0x010601\n"` |
| `test_pci_devfs_attr_config_is_256_raw_bytes` | `config` → 恰好 256 字节；前 4 字节 == `vendor \| device<<16`；byte 0x08 == rev |
| `test_pci_devfs_attr_resource_lists_bars` | 含 `Region 0: I/O ports at 0xc000` 与 `Region 2: Memory at 0xfebf0000`；无效 BAR 不出现；**不含** `(size=` |
| `test_pci_devfs_attr_irq_and_driver` | `irq` → mock `route_gsi` 的 GSI；`driver` → 绑定驱动名；未绑定 → `"none\n"` |
| `test_pci_devfs_format_reports_full_length` | `size` 小于内容长度时，返回值仍是**完整长度**（截断语义） |
| `test_pci_devfs_register_publishes_paths` | 每设备 7 次注册；路径串依次为 `pci/0000:00:1f.2/{vendor,device,class,config,resource,irq,driver}`；7 次调用共用同一张 `devfs_ops` 表；`private_data` 是属性上下文 |
| `test_pci_devfs_read_slices_by_offset` | 捕获到的 `ops->read` 按 `offset` 切片；`offset >= len` → 0（EOF）；`size == 0` → 0 |

#### B) `hosttests/cases/test_pci_devfs_paths.c` → `test_pci_devfs_paths`（`fs` 类）

链接：`kernel/bus/pci/pci_devfs.c` + **真实** `kernel/fs/devfs.c` + VFS 生产对象
（与 `test_devfs_hierarchy` 同款对象集）。
case TU 提供 mock `arch_pci_backend()` 与日志桩，并用 `pci_devfs_register_device()`
发布一个**合成设备**（无需 PCI 枚举）。

| 用例 | 验证 |
|------|------|
| `test_pci_devfs_paths_lookup_and_read` | `/dev/pci/0000:00:1f.2/vendor` 可 lookup，类型 `VFS_CHRDEV`，读回 `"0x8086\n"` |
| `test_pci_devfs_paths_readdir_lists_bdf` | readdir `/dev/pci` 列出 `0000:00:1f.2`；readdir `/dev/pci/0000:00:1f.2` 列出 7 个属性名 |
| `test_pci_devfs_paths_config_is_256_bytes` | 读 `config` 节点得到 256 字节 |

两个二进制都必须：按仓库约定用 `register_host_test()` 注册 + `TEST_BINS` 条目 +
构建规则，三者同一 commit 完成（AGENTS.md 硬性要求）。

### 5.2 QEMU E2E

- `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` 必须仍 PASS。
- 手工 headless 回归：shell 中 `/bin/lspci` 至少列出 QEMU q35 的实际设备
  （host bridge、ISA bridge、IDE、SATA、VGA、Ethernet 等），输出格式对齐 Linux lspci；
  `/bin/lspci -x` 输出 16 行 hex dump；`/bin/lspci -n` 输出数字 ID；
  `ls /dev/pci` 列出 BDF 目录；`cat /dev/pci/<BDF>/vendor` 返回 `0x8086` 形式。

### 5.3 静态审计

`make PROFILE=x86_64-clang test-static` 全绿；`make PROFILE=x86_64-clang test-host` 全绿。

---

## 6. 改动清单

| 文件 | 改动 |
|------|------|
| `kernel/include/bus/pci/pci_devfs.h` | 新增（属性枚举、`pci_devfs_register_all()`、`pci_devfs_register_device()`、`pci_devfs_format_attr()`） |
| `kernel/bus/pci/pci_devfs.c` | 新增（属性上下文、格式化、注册、read 回调） |
| `kernel/core/main.c` | 在 `fs_boot_prepare()` 之后调用 `pci_devfs_register_all()` |
| `user/lspci.c` | 新增（枚举、名称解析、输出、选项） |
| `hosttests/cases/test_pci_devfs.c` | 新增（属性层用例） |
| `hosttests/cases/test_pci_devfs_paths.c` | 新增（devfs 集成层用例） |
| `hosttests/mk/drivers.mk` | 注册 `test_pci_devfs` + `TEST_BINS` + 构建规则 |
| `hosttests/mk/fs.mk` | 注册 `test_pci_devfs_paths` + `TEST_BINS` + 构建规则 |
| `docs/fs/filesystem.md` | devfs 章节补 `/dev/pci` 命名空间说明 |
| `docs/roadmap.md` | ARCH-12 标记闭环 + 当前状态行 |
| `docs/changelog.md` | 闭环条目 |
| `docs/superpowers/specs/2026-11-02-arch12-lspci-design.md` | 本文档 |
| `docs/superpowers/plans/2026-11-02-arch12-lspci.md` | 实施计划 |

**`kernel/Makefile` 不需要改动**——`kernel/Makefile:133` 已用
`$(wildcard bus/pci/*.c)` 收集 PCI 源文件，新文件 `bus/pci/pci_devfs.c` 自动入构建。
aarch64 profile 用的是**显式**源文件清单（`kernel/Makefile:70-99`，不含 `bus/pci/*`），
故 aarch64 天然不编译本模块——与 ARCH-9「aarch64 不链接 PCI/NIC」的既有约定一致，
无需 arch gate。

**不改动**：`kernel/fs/vfs.c`、`kernel/fs/devfs.c`、`kernel/bus/pci/core.c`、
`kernel/arch/x86_64/bus/pci.c`、`kernel/include/uapi/` 任何文件、
`kernel/include/bus/pci/pci.h`、`kernel/include/bus/pci/driver.h`、
`config/rootfs.mk`（`/bin/lspci` 由 `$(USER_PROGRAMS)` 自动派生）。

---

## 7. 风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| `pci_config_read32` 在用户态 read 时触碰硬件 | CF8/CFC 端口 I/O 在 syscall 上下文 | x86 backend 用 `s_pci_lock`（irqsave）保护，读的是只读寄存器 |
| devfs 记录永不释放 → 属性上下文泄漏 | 每个设备 × 7 个 ctx | 启动期一次性分配，数量有界（设备数 × 7），与 devfs 记录同生命周期 |
| 用户态 `readdir` 顺序不确定 | 输出不稳定、难以 diff | 用户态按 BDF 字典序排序后输出 |
| `/dev/pci` 不存在（无 PCI 的架构） | lspci 崩溃 | 用户态检查 `opendir` 返回值，给出诊断 + 退出码 1 |
| `resource` 伪造 BAR 容量 | 误导性输出 | 不输出 size，只输出地址与类型（§3.5） |
| 内核注册失败静默 | lspci 缺设备 | 注册失败 `log_warn` + 计入返回值；用户态 `-v` 显示实际列出的设备数 |
| hosttest 链接面过宽（pci core 拖出 device core） | 测试二进制链接失败 / 引入无关依赖 | 用 `-ffunction-sections -fdata-sections` + `--gc-sections`；不调用 `pci_enumerate()`，改用 `pci_devfs_register_device()` 发布合成设备；两个用例二进制按运行时分开（§5.1） |
| read 路径在栈上开 512 字节缓冲 | 内核栈压力 | 512 字节远低于内核栈容量；read 路径零动态分配 |

---

## 8. 验收标准

1. `make PROFILE=x86_64-clang test-host` 全绿，含新增 `test_pci_devfs` 与
   `test_pci_devfs_paths`（基线 165 → 167）。
2. `make PROFILE=x86_64-clang test-static` 全绿。
3. `make PROFILE=x86_64-clang kernel.bin` 与 `disk.img` 构建通过。
4. QEMU 中 `/bin/lspci` 列出实际 PCI 设备，格式对齐 Linux lspci；
   `-x` / `-n` / `-v` 三个选项均可用。
5. `ls /dev/pci` 列出 BDF 目录，`cat /dev/pci/<BDF>/vendor` 返回 `0x…` 形式。
6. **零新增 syscall**：本项不改动 `kernel/include/uapi/`，`SYS_*` 编号集合不变。
7. `docs/fs/filesystem.md` 记录 `/dev/pci` 命名空间；
   `docs/roadmap.md` 的 ARCH-12 标记闭环；`docs/changelog.md` 有对应条目。
8. `kernel/Makefile` **零改动**（wildcard 自动收集）；aarch64 profile 仍不编译本模块。

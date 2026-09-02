# 系统引导流程

本系统使用 UEFI 引导方式，从 UEFI 固件启动到内核执行的完整流程如下。

## 准备工作

### [posix-uefi](https://gitlab.com/bztsrc/posix-uefi.git)

本系统使用posix-uefi来编写uefi引导程序，posix-uefi提供posix风格的编码方式来编写uefi程序，且不需要使用交叉编译。posix-uefi可以自动生成PE格式的可执行文件，无需手工干预。

posix-uefi 由 profile 私有的 adapter 受控拷贝到 `build/<profile>/uefi-runtime/` 并应用补丁后构建；不需要（也不允许）手工链接或修改 `thirdpart/posix-uefi` 源码树。构建入口是根 Makefile：`make PROFILE=x86_64-clang`（x86，产物 `BOOTX64.EFI`）或 `make PROFILE=aarch64-clang aarch64-uefi`（aarch64，产物 `BOOTAA64.EFI`）。

## 引导流程概述

x86_64 和 aarch64 共享同一个 UEFI 引导器源码（`boot/uefi/main.c` + `boot/uefi/arch/arch.h`），通过 `boot/uefi/arch/<arch>/boot.c` 里的钩子实现架构差异。两者都构建 `boot_context` v2 ABI 并通过 `kernel_main(const struct boot_context *)` 跳转到内核。

aarch64 仅支持 UEFI 引导（旧的直接 `-kernel` 启动方式已删除）。

1. **UEFI 固件初始化**：系统加电后，UEFI 固件初始化硬件并执行自检
2. **UEFI 引导管理器**：UEFI 固件加载并执行引导管理器
3. **引导程序加载**：引导管理器加载并执行系统的引导程序（x86_64: `BOOTX64.EFI`，aarch64: `BOOTAA64.EFI`）
4. **构建 boot_context**：引导程序在固定物理地址分配 `struct boot_context`，初始化 magic/version/size/flags
5. **加载内核**：通过 `arch_load_kernel` 钩子把内核加载到内存（x86 直接拷贝 `kernel.bin`，aarch64 通过 ELF 解析）
6. **系统信息收集**：图形模式（`capture_graphics`）、固件表（`arch_fill_firmware`，aarch64 解析 FDT，x86 取 RSDP）、内存映射（UEFI `GetMemoryMap`，由 `arch_build_memory` 转成 `BOOT_MEMORY_MAP`）
7. **退出引导服务**：`ExitBootServices` 循环（map_key 失效时重取）
8. **跳转到内核**：`arch_enter_kernel(entry, context_phys)` → 内核入口

## 详细引导流程

> 详细的内核加载、内存检测、图形模式设置等步骤的代码现在统一在 `boot/uefi/main.c`（共享生命周期）和 `boot/uefi/arch/<arch>/boot.c`（架构钩子）里。下面对每一步的描述对应到这个新结构，请直接阅读源码以了解具体实现。

### 1. UEFI 固件初始化

1. **系统加电**：系统电源开启，处理器开始执行复位向量
2. **UEFI 固件执行**：处理器执行 UEFI 固件代码
3. **硬件初始化**：UEFI 固件初始化各种硬件设备
4. **自检**：UEFI 固件执行硬件自检（POST）
5. **查找引导设备**：UEFI 固件查找可引导设备

### 2. UEFI 引导管理器

1. **加载引导管理器**：UEFI 固件加载并执行内置的引导管理器
2. **显示引导菜单**：引导管理器显示引导菜单（如果有多个可引导设备）
3. **选择引导项**：用户选择要引导的操作系统
4. **加载引导程序**：引导管理器加载选中的引导程序

### 3. 引导程序加载

1. **加载 BOOTX64.EFI**：UEFI 固件从 EFI 系统分区加载 BOOTX64.EFI
2. **执行引导程序**：UEFI 固件跳转到引导程序入口点

### 4. 内核加载

引导程序在 `main.c` 中执行以下操作：

1. **打开内核文件**：打开 `kernel.bin` 文件
   ```c
   FILE *kernFile = NULL;
   if((kernFile = fopen("kernel.bin", "r")))
   ```

2. **获取内核大小**：获取 `kernel.bin` 文件大小
   ```c
   fseek(kernFile, 0, SEEK_END);
   kernSize = ftell(kernFile);
   fseek(kernFile, 0, SEEK_SET);
   ```

3. **分配内存**：在固定地址 0x100000 分配内存用于加载内核
   ```c
   efi_physical_address_t kernel_address = 0x100000;
   status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, (kernSize + 0x1000 - 1) / 0x1000, &kernel_address);
   ```

4. **加载内核**：将内核文件读取到分配的内存中
   ```c
   fread((char *)kernel_address, kernSize, 1, kernFile);
   fclose(kernFile);
   ```

### 5. 配置文件读取

引导程序读取 `config.txt` 配置文件（如果存在）：

1. **打开配置文件**：打开 `config.txt` 文件
   ```c
   FILE *kconfig = NULL;
   if((kconfig = fopen("config.txt", "r")))
   ```

2. **读取配置内容**：读取配置文件内容
   ```c
   char *content = malloc(size + 1);
   fread(content, size, 1, kconfig);
   content[size] = 0;
   ```

3. **解析配置**：解析配置文件中的设置，如分辨率
   ```c
   if (strcmp(tok, "resolution") == 0)
   {
       tok = strtok(NULL, delim);
       struct GRAPHICS_INFO *info = GetResolution(tok);
       if (info != NULL)
       {
           EXPECT_VBE_HEIGHT = info->VerticalResolution;
           EXPECT_VBE_WIDTH = info->HorizontalResolution;
           free(info);
       }
   }
   ```

### 6. 图形模式设置

引导程序设置图形显示模式：

1. **获取图形输出协议**：获取 EFI_GRAPHICS_OUTPUT_PROTOCOL
   ```c
   efi_guid_t gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
   efi_gop_t *gop = NULL;
   status = BS->LocateProtocol(&gopGuid, NULL, (void**)&gop);
   ```

2. **查找合适的图形模式**：查找符合期望分辨率的图形模式
   ```c
   for(i = 0; i < gop->Mode->MaxMode; i++) {
       status = gop->QueryMode(gop, i, &isiz, &info);
       if(EFI_ERROR(status) || info->PixelFormat > PixelBitMask) continue;
       if(info->HorizontalResolution > currentVBEWidth && info->HorizontalResolution <= EXPECT_VBE_WIDTH)
       {
           if(info->VerticalResolution > currentVBEHeight && info->VerticalResolution <= EXPECT_VBE_HEIGHT)
           {
               currentVBEHeight = info->VerticalResolution;
               currentVBEWidth = info->HorizontalResolution;
               expectVBEMode = i;
           }
       }
   }
   ```

3. **设置图形模式**：设置选中的图形模式
   ```c
   status = gop->SetMode(gop, expectVBEMode);
   ```

### 7. 内存检测

引导程序获取系统内存映射：

1. **获取内存映射大小**：获取内存映射所需的缓冲区大小
   ```c
   status = BS->GetMemoryMap(&memory_map_size, NULL, &map_key, &desc_size, NULL);
   ```

2. **分配内存**：为内存映射分配缓冲区
   ```c
   memory_map = (efi_memory_descriptor_t*)malloc(memory_map_size);
   ```

3. **获取内存映射**：获取系统内存映射
   ```c
   status = BS->GetMemoryMap(&memory_map_size, memory_map, &map_key, &desc_size, NULL);
   ```

4. **转换为 E820 格式**：将 UEFI 内存映射转换为 E820 格式
   ```c
   for(mement = memory_map; (uint8_t*)mement < (uint8_t*)memory_map + memory_map_size; mement = NextMemoryDescriptor(mement, desc_size)) {
       // 转换内存类型
       switch (mement->Type)
       {
           case EfiReservedMemoryType:
           case EfiMemoryMappedIO:
           case EfiMemoryMappedIOPortSpace:
           case EfiPalCode:
               MemType = 2;    //2:ROM or Reserved
               break;
           // 其他内存类型转换...
       }
       
       // 合并相邻的内存区域
       if((LastE820 != NULL) && (LastE820->type == MemType) && (mement->PhysicalStart == LastEndAddr))
       {
           LastE820->length += mement->NumberOfPages << 12;
           LastEndAddr += mement->NumberOfPages << 12;
       }
       else
       {
           E820p->address = mement->PhysicalStart;
           E820p->length = mement->NumberOfPages << 12;
           E820p->type = MemType;
           LastEndAddr = mement->PhysicalStart + (mement->NumberOfPages << 12);
           LastE820 = E820p;
           E820p++;
           E820Count++;
       }
   }
   ```

5. **排序内存映射**：按地址排序内存映射
   ```c
   for(i = 0; i< E820Count; i++)
   {
       struct E820_ENTRY* e820i = LastE820 + i;
       struct E820_ENTRY MemMap;
       for(j = i + 1; j< E820Count; j++)
       {
           struct E820_ENTRY* e820j = LastE820 + j;
           if(e820i->address > e820j->address)
           {
               MemMap = *e820i;
               *e820i = *e820j;
               *e820j = MemMap;
           }
       }
   }
   ```

### 8. 引导参数准备

引导程序准备传递给内核的引导参数：

1. **分配内存**：在固定地址 0x60000 分配内存用于引导参数
   ```c
   efi_physical_address_t boot_param_address = 0x60000;
   status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, 2, &boot_param_address);
   ```

2. **初始化引导参数**：初始化 `BOOT_INFO` 结构
   ```c
   struct BOOT_INFO * kern_boot_para_info = (struct BOOT_INFO *)boot_param_address;
   kern_boot_para_info->RSDP = 0x0;
   kern_boot_para_info->BootFromBIOS = 0;
   kern_boot_para_info->Graphics_Info.HorizontalResolution = gop->Mode->Information->HorizontalResolution;
   kern_boot_para_info->Graphics_Info.VerticalResolution = gop->Mode->Information->VerticalResolution;
   kern_boot_para_info->Graphics_Info.PixelsPerScanLine = gop->Mode->Information->PixelsPerScanLine;
   kern_boot_para_info->Graphics_Info.FrameBufferBase = gop->Mode->FrameBufferBase;
   kern_boot_para_info->Graphics_Info.FrameBufferSize = gop->Mode->FrameBufferSize;
   kern_boot_para_info->E820_Info.E820_Entry_count = E820Count;
   ```

3. **查找 RSDP**：查找 ACPI RSDP 表
   ```c
   efi_configuration_table_t* configTable = ST->ConfigurationTable;
   efi_guid_t Acpi2TableGuid = ACPI_20_TABLE_GUID;
   
   for (uintn_t index = 0; index < ST->NumberOfTableEntries; index++)
   {
       if (CompareGuid(&configTable[index].VendorGuid, &Acpi2TableGuid))
       {
           kern_boot_para_info->RSDP = (unsigned long long)configTable[index].VendorTable;
           break;
       }
       configTable++;
   }
   ```

### 9. 退出引导服务

引导程序退出 UEFI 引导服务：

```c
if(exit_bs()) {
    fprintf(stderr, "error when exit boot service!\n");
    return 0;
}
```

### 10. 跳转到内核

引导程序跳转到内核执行：

```c
int (*kernel_main)(struct BOOT_INFO *);
kernel_main = (void*)0x100000;

kernel_main(kern_boot_para_info);
```

### 11. 内核初始化

内核在 `kernel_main` 函数中执行以下初始化操作：

1. **初始化帧缓冲区**：设置图形显示
   ```c
   Pos.Phy_addr = (uint32_t *)bootinfo->Graphics_Info.FrameBufferBase;
   Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
   Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
   Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;
   ```

2. **初始化自旋锁**：初始化帧缓冲区的自旋锁
   ```c
   spin_init(&Pos.lock);
   ```

3. **加载任务寄存器**：设置 TSS
   ```c
   load_TR(8);
   set_tss64(0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00);
   ```

4. **初始化中断**：安装系统向量和 IRQ
   ```c
   sys_vector_install();
   irq_install();
   ```

5. **初始化串口**：设置串口调试输出
   ```c
   init_serial();
   serial_printk("serial port init succedd\n");
   ```

6. **初始化物理内存管理**：检测内存，初始化物理内存管理
   ```c
   pmm_init(bootinfo->E820_Info);
   ```

7. **初始化虚拟内存管理**：初始化虚拟内存管理
   ```c
   vmm_init();
   ```

8. **初始化帧缓冲区**：重新映射帧缓冲区
   ```c
   frame_buffer_init();
   ```

9. **初始化 PIC**：初始化 PIC 控制器
   ```c
   pic_init();
   ```

10. **初始化定时器**：初始化系统定时器
    ```c
    timer_init();
    pit_init();
    ```

11. **初始化键盘**：初始化键盘驱动
    ```c
    keyboard_init();
    ```

12. **创建测试定时器**：创建一个测试定时器
    ```c
    timer = create_timer(test_timer, NULL, 10);
    add_timer(timer);
    ```

13. **进入主循环**：执行 hlt 指令，等待中断
    ```c
    while(1)
    {
        hlt();
    }
    ```

## 引导参数结构及关键 ABI

**⚠️ 关键 ABI 说明**: x86_64 引导器使用 clang `--target=x86_64-pc-win32-coff` 编译（LLP64 数据模型：`sizeof(long)=4`），而内核使用 SysV LP64（`sizeof(long)=8`）。为避免结构体布局不匹配，**`boot_context` 及其子结构中的所有字段必须使用固定大小类型（`uint32_t`、`uint64_t`）**，绝不能使用 `unsigned long`、`unsigned int` 或指针。参见 `kernel/include/kernel/bootinfo.h`。

x86_64 和 aarch64 两个 UEFI 引导器都构建同一个 `boot_context` v2 结构体（在 `boot_context_init` 里初始化），magic 为 `BOOT_CONTEXT_MAGIC = 0x4f533031`，version 为 `2`，size 等于 `sizeof(struct boot_context)`（104 字节）。内核通过 `boot_context_valid()` 校验三者后再继续。

引导程序传递给内核的引导参数结构如下（使用固定大小类型）：

```c
struct GRAPHICS_INFO
{
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
};

enum BOOT_MEMORY_FORMAT {
    BOOT_MEMORY_FORMAT_UNKNOWN  = 0,
    BOOT_MEMORY_FORMAT_E820     = 1,
    BOOT_MEMORY_FORMAT_GENERIC  = 2,
    BOOT_MEMORY_FORMAT_UEFI_RAW = 3,
};

struct BOOT_MEMORY_MAP {
    uint64_t entries;              /* 物理地址：条目 / 描述符数组 */
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t format;
    uint32_t descriptor_version;   /* UEFI 描述符版本，否则为 0 */
};

struct BOOT_FIRMWARE {
    uint64_t dtb;            /* FDT 物理地址（aarch64），否则为 0 */
    uint64_t acpi_rsdp;      /* ACPI RSDP 物理地址（x86_64），否则为 0 */
};

struct E820_ENTRY              /* x86_64 旧式 E820 记录，仍由 pmm.c 消费 */
{
    uint64_t address;
    uint64_t length;
    uint32_t type;
} __attribute__((packed));

struct boot_context {
    uint32_t magic;               /* = BOOT_CONTEXT_MAGIC */
    uint32_t version;             /* = BOOT_CONTEXT_VERSION (2) */
    uint32_t size;                /* = sizeof(struct boot_context) */
    uint32_t flags;               /* BOOT_CONTEXT_HAS_* 位 */
    uint32_t reserved;
    struct GRAPHICS_INFO graphics;
    struct BOOT_MEMORY_MAP memory;
    struct BOOT_FIRMWARE firmware;
    uint64_t boot_cpu_id;
};
```

## 内存布局

### 引导过程中的内存布局

| 地址范围    | 用途             |
|------------|-----------------|
| 0x00000000 | 内存开始         |
| 0x00006000 | 引导参数（BOOT_INFO） |
| 0x00100000 | 内核加载地址     |
| 0x00200000 | 内核结束地址     |
| ...        | ...             |
| 内存结束   | 内存结束         |

## 引导程序代码结构

### 主要文件

x86_64 和 aarch64 共享同一份主代码，架构差异在 `arch/` 子目录里：

* `boot/uefi/main.c` - 共享 UEFI 引导器主循环（生命周期、EBS loop、graphics/firmware/memory 收集）
* `boot/uefi/arch/arch.h` - 共享钩子声明（`arch_init_handoff` / `arch_load_kernel` / `arch_setup_graphics` / `arch_fill_firmware` / `arch_memory_buffer` / `arch_build_memory` / `arch_release` / `arch_enter_kernel` / `arch_puts`）
* `boot/uefi/arch/x86_64/boot.c` - x86_64 实现（固定 `0x100000` 直接加载 `kernel.bin`，RSDP via `arch_fill_firmware`，E820 via `arch_build_memory`，handoff `boot_context` @ `0x60000`）
* `boot/uefi/arch/aarch64/boot.c` + `elf.c` + `handoff.S` + `loader.h` - aarch64 实现（ELF 解析、FDT dtb 提取、`aarch64_handoff_el_supported` / `aarch64_page_interval`、handoff `boot_context` 由 `arch_init_handoff` 分配）
* `boot/uefi/Makefile` - 统一构建包装（profile-only 内部目标：由根 Makefile 经 `os01_submake` 调用，解析期要求 `OS01_PROFILE_FILE`；`ARCH=x86_64` → `BOOTX64.EFI`，`ARCH=aarch64` → `BOOTAA64.EFI`。独立构建入口是 `make PROFILE=x86_64-clang <target>` 或 `make PROFILE=aarch64-clang aarch64-uefi`）
* `BOOTX64.EFI` / `BOOTAA64.EFI` - 编译产物（位于 `build/<profile>/artifacts/uefi/`）
* `build/<profile>/firmware/OVMF.fd` - x86_64 的 profile 私有 UEFI 固件（首次使用时按 `OVMF_FIRMWARE_SOURCE` 自动获取）
* `/usr/share/edk2/aarch64/QEMU_EFI.fd` - aarch64 固件（`AARCH64_UEFI_FIRMWARE_SOURCE`，复制到 profile build 目录 `build/<profile>/image/QEMU_EFI.fd`）

### 核心函数

* `main` - 引导程序的主函数（`boot/uefi/main.c`）
* `arch_init_handoff` / `arch_load_kernel` / `arch_setup_graphics` / `arch_fill_firmware` / `arch_build_memory` / `arch_enter_kernel` - 架构相关的钩子
* `capture_graphics` / `guid_equal` - 共享辅助函数（`boot/uefi/main.c`）
* `aarch64_handoff_el_supported` / `aarch64_page_interval` - aarch64 专用辅助（`loader.h`）

## 内核入口点

内核的入口点是 `kernel_main` 函数，位于 `kernel/kernel/main.c` 文件中。两个架构的引导器都通过 `arch_enter_kernel` 跳转到同一签名：

```c
void kernel_main(const struct boot_context *bootctx);
```

`bootctx` 由引导器放置在固定物理地址（x86_64 @ `0x60000`，aarch64 由 `arch_init_handoff` 分配），并通过 `boot_context_valid()` 校验 magic / version / size 后才使用。

## 故障排除

### 引导失败的常见原因

1. **内核文件不存在**：确保 `kernel.bin` 文件存在于引导设备中
2. **内存不足**：确保系统有足够的内存来加载内核和执行引导过程
3. **图形模式设置失败**：检查显示器是否支持所选的分辨率
4. **内存映射获取失败**：检查 UEFI 固件是否正确提供内存映射
5. **引导服务退出失败**：检查 UEFI 固件是否正确处理引导服务的退出

### 调试技巧

1. **串口调试**：引导程序和内核都使用串口进行调试输出
2. **日志文件**：检查引导过程中的日志文件
3. **UEFI Shell**：使用 UEFI Shell 手动执行引导程序，查看错误信息
4. **QEMU 调试**：使用 QEMU 模拟器进行调试，设置 `-serial stdio` 查看串口输出

## 扩展引导功能

### 支持 BIOS 引导

当前系统只支持 UEFI 引导（x86_64 通过 OVMF，aarch64 通过 `QEMU_EFI.fd`）。aarch64 原本存在的直接 `-kernel` 启动方式已删除，整个 aarch64 路径现在也走 UEFI。可以通过以下方式添加 BIOS 引导支持：

1. **创建 BIOS 引导程序**：创建一个支持 BIOS 引导的引导程序
2. **修改引导参数**：调整 `boot_context` 结构，添加 BIOS 引导相关的字段
3. **修改内核初始化**：修改内核初始化代码，支持从 BIOS 引导

### 支持更多引导选项

可以通过修改 `config.txt` 文件，添加更多引导选项：

1. **添加内核参数**：在 `config.txt` 中添加内核参数
2. **支持多内核**：支持从多个内核中选择一个启动
3. **支持启动菜单**：添加启动菜单，允许用户选择启动选项

## 总结

本系统的引导流程是一个从 UEFI 固件到内核执行的完整过程，包括：

1. **UEFI 引导**：使用 UEFI 引导方式，x86_64 与 aarch64 共享 `boot/uefi/main.c` 主循环，架构差异在 `boot/uefi/arch/<arch>/boot.c` 中
2. **内核加载**：通过 `arch_load_kernel` 钩子加载（x86_64 直接拷贝 `kernel.bin` @ `0x100000`，aarch64 通过 ELF 解析）
3. **系统信息收集**：共享 `capture_graphics`、架构相关 `arch_fill_firmware`（x86: RSDP，aarch64: FDT dtb）、共享 `BS->GetMemoryMap` → `BOOT_MEMORY_MAP`
4. **引导参数传递**：统一通过 `boot_context` v2 结构体（magic / version / size / flags + graphics + memory + firmware + boot_cpu_id）
5. **内核初始化**：内核通过 `kernel_main(const struct boot_context *)` 执行初始化操作，启动系统

这种引导方式具有以下优点：

* **支持现代硬件**：UEFI 引导支持现代硬件特性
* **架构统一**：x86_64 与 aarch64 共享主代码，仅架构差异在 `boot/uefi/arch/` 钩子里
* **ABI 稳定**：`boot_context` v2 + 固定大小类型，跨 LP64/LLP64 数据模型可移植
* **图形模式支持**：支持设置图形模式，提供更好的用户体验

引导流程的实现为系统的启动提供了可靠的基础，同时也为后续的功能扩展留下了空间。
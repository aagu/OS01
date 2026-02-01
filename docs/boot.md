# 系统引导流程

本系统使用 UEFI 引导方式，从 UEFI 固件启动到内核执行的完整流程如下。

## 准备工作

### [posix-uefi](https://gitlab.com/bztsrc/posix-uefi.git)

本系统使用posix-uefi来编写uefi引导程序，posix-uefi提供posix风格的编码方式来编写uefi程序，且不需要使用交叉编译。posix-uefi可以自动生成PE格式的可执行文件，无需手工干预。

为方便使用，将thirdpart/posix-uefi/uefi文件夹链接到boot/uefi/uefi:

```shell
cd boot/uefi/
ln -s ../../thirdpart/posix-uefi/uefi

## 引导流程概述

1. **UEFI 固件初始化**：系统加电后，UEFI 固件初始化硬件并执行自检
2. **UEFI 引导管理器**：UEFI 固件加载并执行引导管理器
3. **引导程序加载**：引导管理器加载并执行系统的引导程序（BOOTX64.EFI）
4. **内核加载**：引导程序加载内核到内存
5. **系统信息收集**：引导程序收集系统信息（如内存映射、图形模式等）
6. **跳转到内核**：引导程序退出引导服务并跳转到内核执行
7. **内核初始化**：内核执行初始化操作
8. **系统启动完成**：内核进入主循环，等待中断

## 详细引导流程

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

## 引导参数结构

引导程序传递给内核的引导参数结构如下：

```c
struct BOOT_INFO
{
    struct GRAPHICS_INFO Graphics_Info;
    struct MEMORY_INFO E820_Info;
    unsigned long long RSDP;
    boolean_t BootFromBIOS;
};

struct GRAPHICS_INFO
{
    unsigned int HorizontalResolution;
    unsigned int VerticalResolution;
    unsigned int PixelsPerScanLine;
    unsigned long FrameBufferBase;
    unsigned long FrameBufferSize;
};

struct MEMORY_INFO
{
    unsigned int E820_Entry_count;
    struct E820_ENTRY *E820_Entry;
};

struct E820_ENTRY
{
    unsigned long address;
    unsigned long length;
    unsigned int  type;
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

* `main.c` - 引导程序的主要实现文件
* `Makefile` - 引导程序的编译规则
* `BOOTX64.EFI` - 编译后的引导程序
* `OVMF.fd` - QEMU 模拟 UEFI 环境所需的固件文件

### 核心函数

* `main` - 引导程序的主函数
* `GetResolution` - 解析分辨率配置
* `CompareGuid` - 比较 GUID
* `exit_bs` - 退出引导服务

## 内核入口点

内核的入口点是 `kernel_main` 函数，位于 `kernel/kernel/main.c` 文件中。该函数接收一个 `BOOT_INFO` 结构指针作为参数，包含了系统的内存映射、图形模式等信息。

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

当前系统只支持 UEFI 引导，可以通过以下方式添加 BIOS 引导支持：

1. **创建 BIOS 引导程序**：创建一个支持 BIOS 引导的引导程序
2. **修改引导参数**：修改 `BOOT_INFO` 结构，添加 BIOS 引导相关的字段
3. **修改内核初始化**：修改内核初始化代码，支持从 BIOS 引导

### 支持更多引导选项

可以通过修改 `config.txt` 文件，添加更多引导选项：

1. **添加内核参数**：在 `config.txt` 中添加内核参数
2. **支持多内核**：支持从多个内核中选择一个启动
3. **支持启动菜单**：添加启动菜单，允许用户选择启动选项

## 总结

本系统的引导流程是一个从 UEFI 固件到内核执行的完整过程，包括：

1. **UEFI 引导**：使用 UEFI 引导方式，支持现代硬件
2. **内核加载**：将内核加载到固定地址
3. **系统信息收集**：收集系统内存映射、图形模式等信息
4. **引导参数传递**：将系统信息传递给内核
5. **内核初始化**：内核执行初始化操作，启动系统

这种引导方式具有以下优点：

* **支持现代硬件**：UEFI 引导支持现代硬件特性
* **灵活的配置**：通过 `config.txt` 文件可以灵活配置引导选项
* **详细的系统信息**：传递详细的系统信息给内核，便于内核初始化
* **图形模式支持**：支持设置图形模式，提供更好的用户体验

引导流程的实现为系统的启动提供了可靠的基础，同时也为后续的功能扩展留下了空间。
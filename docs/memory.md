# 内存管理系统

本系统实现了完整的 x86_64 架构内存管理系统，包括物理内存管理和虚拟内存管理。

## 内存管理系统架构

OS01采用Higher Half Kernel内存布局，内核程序使用`0xffff800000000000`以上的虚拟地址。这要求链接脚本将内核程序的起始地址设置为`0xffff800000000000 + 0x100000`，即高地址的1M位移处。

引导器（Loader）将内核程序加载到物理地址`0x100000`处，并跳转到该地址执行。该地址存放的是内核执行头程序，内核执行头会立即加载位于物理地址`0x10100`处的页目录，并通过`lret`指令切换到虚拟地址`0xffff80000010000`附近继续执行。

因此进入c语言的`kernel_main`函数后，应尽早重映射帧缓冲区以便可以在屏幕上输出文字内容。

### 物理内存管理

系统使用物理内存管理器（PMM）来管理物理内存，主要功能包括：

1. **内存检测**：通过 E820 内存映射检测系统可用内存
2. **内存划分**：将内存划分为不同的区域（Zone）
3. **页面管理**：使用页面（Page）结构管理物理页面
4. **页面分配**：分配和释放物理页面
5. **页面跟踪**：使用位掩码（bits_map）跟踪页面使用情况

### 虚拟内存管理

系统使用虚拟内存管理器（VMM）来管理虚拟内存，主要功能包括：

1. **页表管理**：使用多级页表（PML4, PML3, PML2）
2. **地址映射**：将虚拟地址映射到物理地址
3. **页表操作**：设置和刷新页表项

### 内存分配器

系统实现了多种内存分配器：

1. **物理页面分配器**：分配和释放物理页面
2. **Slab 分配器**：用于小内存分配

## 核心数据结构

### 物理内存管理器

```c
struct Physical_Memory_Manager {
    struct E820_ENTRY e820_entrys[E820_MAX_ENTRY_COUNT];
    uint64_t e820_length;
    
    uint64_t *bits_map;
    uint64_t bits_size;
    uint64_t bits_length;
    
    struct Page *pages_struct;
    uint64_t pages_size;
    uint64_t pages_length;
    
    struct Zone *zones_struct;
    uint64_t zones_size;
    uint64_t zones_length;
    
    uint64_t start_code;
    uint64_t end_code;
    uint64_t end_data;
    uint64_t end_rodata;
    uint64_t start_brk;
    uint64_t end_of_struct;
};
```

### 内存区域（Zone）

```c
struct Zone {
    uint64_t zone_start_address;
    uint64_t zone_end_address;
    uint64_t zone_length;
    
    uint64_t page_using_count;
    uint64_t page_free_count;
    uint64_t total_pages_link;
    
    uint64_t attribute;
    struct Physical_Memory_Manager *manager_struct;
    
    uint64_t pages_length;
    struct Page *pages_group;
};
```

### 页面（Page）

```c
struct Page {
    struct Zone *zone_struct;
    uint64_t phy_address;
    uint64_t attribute;
    
    uint64_t reference_count;
    
    uint64_t age;
};
```

## 物理内存管理

### 内存检测与初始化

系统通过 `pmm_init` 函数初始化物理内存管理：

1. **内存映射检测**：读取 E820 内存映射信息，识别可用内存区域
2. **内存区域划分**：将可用内存划分为不同的区域（Zone）
3. **页面结构初始化**：为每个物理页面创建 Page 结构
4. **位掩码初始化**：使用位掩码跟踪页面使用情况
5. **Slab 分配器初始化**：初始化 Slab 分配器

### 页面分配与释放

#### 页面分配

使用 `alloc_pages` 函数分配物理页面：

```c
struct Page * alloc_pages(int32_t zone_select, int32_t number, uint64_t page_flags);
```

参数说明：
* `zone_select` - 内存区域选择（ZONE_DMA, ZONE_NORMAL, ZONE_UNMAPPED）
* `number` - 要分配的页面数量（最大 64）
* `page_flags` - 页面标志

#### 页面释放

使用 `free_pages` 函数释放物理页面：

```c
void free_pages(struct Page * page, int32_t number);
```

参数说明：
* `page` - 要释放的页面起始地址
* `number` - 要释放的页面数量

### 页面属性管理

#### 获取页面属性

```c
uint64_t get_page_attribute(struct Page *page);
```

#### 设置页面属性

```c
uint64_t set_page_attribute(struct Page * page, uint64_t flags);
```

## 虚拟内存管理

### 页表结构

系统使用 x86_64 架构的多级页表：

1. **PML4**（Page Map Level 4）：最高级页表
2. **PML3**（Page Directory Pointer Table）：中级页表
3. **PML2**（Page Directory）：低级页表

### 地址映射

#### 虚拟地址到物理地址的映射

```c
void vmm_map_page(uint64_t *pagemap, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags);
```

参数说明：
* `pagemap` - 页表基地址
* `physical_address` - 物理地址
* `virtual_address` - 虚拟地址
* `flags` - 页表项标志

#### 虚拟地址解除映射

```c
void vmm_unmap_page(uint64_t *pagemap, uintptr_t virtual_address);
```

参数说明：
* `pagemap` - 页表基地址
* `virtual_address` - 虚拟地址

### 虚拟内存初始化

系统通过 `vmm_init` 函数初始化虚拟内存管理：

1. **获取内核页表**：获取内核页表基地址
2. **映射物理内存**：将物理内存映射到虚拟地址空间
3. **刷新 TLB**：刷新 Translation Lookaside Buffer

## 内存分配器

### Slab 分配器

Slab 分配器用于小内存分配，初始化函数为 `slab_init`，位于 `kernel/memory/slab.c` 中。

## 内存初始化流程

1. **内核启动**：`kernel_main` 函数开始执行
2. **内存信息获取**：从引导加载程序获取内存信息
3. **物理内存初始化**：调用 `pmm_init` 初始化物理内存管理
4. **虚拟内存初始化**：调用 `vmm_init` 初始化虚拟内存管理
5. **Slab 分配器初始化**：在 `pmm_init` 中调用 `slab_init`
6. **内存映射**：将物理内存映射到虚拟地址空间

## 内存区域

系统将内存划分为三个主要区域：

1. **ZONE_DMA**：用于 DMA 操作的内存区域
2. **ZONE_NORMAL**：正常内存区域，已映射到页表
3. **ZONE_UNMAPPED**：未映射到页表的内存区域

## 页面大小

系统使用 2MB 大页面，定义如下：

```c
#define PAGE_2M_SIZE     0x200000
#define PAGE_2M_SHIFT    21
#define PAGE_2M_MASK     (~(PAGE_2M_SIZE - 1))
#define PAGE_2M_ALIGN(addr) (((addr) + PAGE_2M_SIZE - 1) & PAGE_2M_MASK)
```

## 代码结构

### 物理内存管理

* `kernel/memory/pmm.c` - 物理内存管理实现
* `kernel/include/kernel/pmm.h` - 物理内存管理头文件

### 虚拟内存管理

* `kernel/memory/vmm.c` - 虚拟内存管理实现
* `kernel/include/kernel/vmm.h` - 虚拟内存管理头文件

### 内存分配器

* `kernel/memory/slab.c` - Slab 分配器实现
* `kernel/include/kernel/slab.h` - Slab 分配器头文件

### 内存工具

* `kernel/memory/dump.c` - 内存转储功能

### 内存头文件

* `kernel/include/kernel/memory.h` - 内存管理公共头文件

## 注意事项

1. **内存对齐**：系统使用 2MB 大页面，内存分配需要对齐到 2MB 边界
2. **页表刷新**：修改页表后需要刷新 TLB，以确保修改生效
3. **内存区域选择**：根据不同的使用场景选择合适的内存区域
4. **页面引用计数**：页面有引用计数，确保在释放页面时引用计数为 0

## 内存管理示例

### 分配物理页面

```c
// 分配 1 个页面
struct Page *page = alloc_pages(ZONE_NORMAL, 1, PG_PTable_Mapped | PG_Kernel);

// 使用页面
uint64_t physical_address = page->phy_address;
uint64_t virtual_address = (uintptr_t)Phy_To_Virt(physical_address);

// 释放页面
free_pages(page, 1);
```

### 映射虚拟地址

```c
// 映射虚拟地址到物理地址
vmm_map_page(kernel_map, physical_address, virtual_address, PAGE_KERNEL_Page);

// 刷新 TLB
flush_tlb();

// 解除映射
vmm_unmap_page(kernel_map, virtual_address);
```

## 性能优化

1. **大页面使用**：使用 2MB 大页面减少页表层级，提高地址转换速度
2. **位掩码跟踪**：使用位掩码快速跟踪页面使用情况
3. **Slab 分配器**：使用 Slab 分配器提高小内存分配效率
4. **内存区域划分**：根据内存用途划分不同区域，提高内存使用效率

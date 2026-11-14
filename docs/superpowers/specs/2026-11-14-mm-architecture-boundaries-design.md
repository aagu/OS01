# 内存管理抽象边界设计

基线：`ed68ccc5`。第一类 x86 行为修复已经落地：分段 mprotect、预备后提交的 munmap/MAP_FIXED、NX 与描述符编解码、独占用户根与先同步后回收。本文约束第二类工作的目标：让代码实际依赖的架构能力与接口的可见范围一致，保留该基线的用户行为。历史行为 spec 只解释来由，实现以该基线代码为准。

## 取舍

只移动文件能纠正目录归属，却无法阻止驱动和 VFS 继续传递硬件位。反过来，以现有 AArch64 后端为模板强行统一用户 VM，会把两种根生命周期和回收协议压进同一个接口：x86 可在当前独占根上修改并本地同步，AArch64 受管用户根要先切离、取消发布，替换还可能需要 break-before-make。选用按调用者需求收敛接口：共享区间元数据、PMM 核心、TLB 广播协议与设备映射请求；各端自行决定如何编码、发布和回收页表。已有公共 uaccess 复制和快照接口保持；只整理其硬件依赖，写入准备的锁和 fault fixup 不统一。

## 1. 设备请求与 VMA 只表达语义

当前 `fb_mmap` 把 `fb_x86_user_pte_flags()` 生成的整字经 `vfs_mmap_commit_fn` 传给 `mmap_dev_commit`，后者又把它保存在公共 `vma_t.vm_page_prot`。这条路径跨设备、VFS 和 MM 三个边界。新回调原型：

```c
enum vm_device_cache { VM_DEVICE_CACHE_UC = 0, VM_DEVICE_CACHE_WB = 1 };
typedef int (*vfs_mmap_commit_fn)(void *ctx, uint64_t first_pa,
                                 enum vm_device_cache cache);
```

枚举由 `memory/vm_device.h` 提供。驱动在控制锁内将已发布 framebuffer cache policy 映射为枚举（仅显式 WRITEBACK 为 WB，其余沿用 UC），仍判断共享、长度、逻辑读写/执行及写入资格；可写性从已验证的 VMA 得出，无需再传一份。VFS/devfs 原样转发。x86 提交端在任何页表或 VMA 变动前校验枚举（未知值 `-EINVAL`），生成 USER + PRESENT + NX、按 VMA 置 WRITE，UC 使用现有 PCD|PWT，WB 清这两位；和内核/早期 framebuffer 别名共用现有 UC/WB 编码规则，不换内存类型。PA、VMA 和引用的生命周期及两锁顺序仍由现有事务负责。设备页始终借用；旧叶先撤销并同步，才释放旧自有页；新叶同步后发布 VMA，驱动回调成功才置 raw 标记。AArch64 不新增此 syscall/设备能力。

删除公共 `vm_page_prot`，以 `vm_flags` 为逻辑权限唯一来源。x86 在普通 VMA 缺页、堆增长、VMA COW 修复及 mprotect 处用同一个内部转换：零权限为仅 PAGE_USER 的模板（暂存仍由原路径添加 PROTNONE）；R/RW 为 NX，RX/RWX 可执行，WRITE 由 VMA 决定。该转换命名为 `uint64_t x86_vma_pte_flags(uint64_t vm_flags)`，由 x86 VMA 头提供；转换只处理已验证的 VMA 逻辑位。不改变 PROT 的 64 位输入校验、x86 对 EXEC 隐含读和无 READ 的 WRITE 拒绝、ELF/2 MiB 栈模板。常规 mprotect 仍从旧叶保留缓存位、COW 状态与 PA；普通 COW、PROT_NONE 及设备借用不是 VMA 权限转换的副作用。AArch64 的 VMA 和驻留记录逻辑权限算法保持原样，只删除未使用字段的赋值/复制。设备 VMA 仍为无文件的 VMA_IO，禁止普通缺页回填及 mprotect；设备缓存真值由已提交叶子和显示后端持有。

## 2. 页表接口按实际消费者分层

- `memory/vmm.h` 只留下双端共有的 `tlb_shootdown(void)`（保留双端同步广播实现；共享映射以及现有合法调用点继续遵守 IRQ、锁及目标 CPU 约束）。不从该头隐式获得 MMU 指令、raw root、PTE 位、uaccess 或 AArch64 操作。
- x86 页表实现 `memory/vmm.c` 归入 `arch/x86_64/memory/vmm.c`，raw API、启动辅助和 `kernel_map` 声明进入 `arch/x86_64/vmm.h`；`pte.h` 留给描述符。保留 x86 raw API 符号及调用契约；把 `vmm_init` 从 `memory/memory.h` 迁入 x86 VMM 头，移除公共 `PAGE_PGD_SHIFT`（迁入 x86 VMM 头）和 `get_cr3` 别名并把两处调用改为显式 x86 根读取。现有 `arch_vmm_query_4k` 在设备事务修复后只剩 x86 测试调用，移除这层不完整的语义查询，测试使用真正的 x86 raw query。x86 专属驱动的既有内核 MMIO 映射直接依赖 x86 头，本轮不引入 ioremap。
- AArch64 M3/M4 的 `arch_vmm_*`、`VM_*`、`kernel_map` 由现有 `arch/aarch64/vmm_backend.h` 提供；函数签名与返回行为（包括只在此私有层可见的 stash sentinel）、锁、BBM 与根准入不变。此处的语义位供该后端和 ARM 用户 VM 使用，不承诺是跨端映射 API。用户策略继续使用三态适配器，正常的 stash 是状态，不是策略错误。
- `arch/mmu.h` 只提供 `ARCH_PAGE_OFFSET` 和双端已有的本地全量 TLB 原语：保留这两项的短条件分支，不传递包含私有头。各端专属的根/单页操作和权限 walker 声明移到 `arch/<arch>/mmu.h`，由调用点直接包含。x86 walker 实体移入其 VMM 文件，保持 4 KiB/2 MiB 权限快照、父级权限、COW 可写候选及拒绝 1 GiB 权限快照；x86 futex 仍用原翻译算法，支持 4 KiB/2 MiB/1 GiB 与页内偏移。删除无调用者的 AArch64 通用翻译副本。walker 不提供 pin、逻辑 VMA 授权或回收同步。现有根字段布局及原始 CR3/TTBR0 寄存器值含义不变，不能把它们当 walker 接收的直映指针。
- `memory/uaccess.h` 保留实际共享的用户复制和范围检查，去掉对 MMU walker 的传递依赖；x86 的 `USER_MIN_ADDR`、锁定写入准备、begin/end、`strnlen_user` 及 jmp buffer 声明放 `arch/x86_64/uaccess.h`。已有 x86 getrandom/devfs 写入者使用该头，不给 AArch64 加假的后端。

## 3. 策略与启动责任归位

`memory/user_vm.c` 是 AArch64 专用策略：硬编码该端地址窗口，仅在 AArch64 产物链接，依赖其受管用户根。移至 `arch/aarch64/memory/user_vm_policy.c`，原策略头迁至 `arch/aarch64/user_vm_policy.h`；事务入口和 leaf 适配仍在现有 `arch/aarch64/memory/user_vm.c`。不重写策略、复制协议或 sparse residency，`mm_t` 暂不改布局。x86 专用 VMA 和 uaccess 实现归 `arch/x86_64/memory/`；双端共用 `vm_area.c`、`mm.c`、逻辑 VMA 定义、PMM、slab 和 TLB 广播协议继续在公共目录。公共 VMA 头不再传递 VMM 依赖；x86 的 mmap/heap/fork/teardown 声明放进 `arch/x86_64/vma.h`，现有逻辑元数据辅助函数不扩成事务框架。

PMM 仍以统一物理区间和分区阈值为输入，`pmm_reserve_boot_ranges` 仍是公共幂等执行器。三个必需策略符号 `pmm_arch_normalize`、`pmm_arch_zone_split`、`pmm_arch_boot_reservations` 每个架构必须显式定义，不让 AArch64 缺实现时静默使用 x86 的低地址前缀。x86 把 reservation 放入独立的 `arch/x86_64/memory/pmm_reserve.c`，便于合成 RAM 夹具使用真实 reservation 而无需链接 E820 normalize；保留原绝对 PA 前缀 `[0, ceil2M(metadata_end_pa))`；AArch64 保留已发布 arena 的单一区间，未准备仍报 `-EINVAL`，分区阈值显式为 `UINT64_MAX`。本轮不改 normalize/zone/reserve 顺序、容量协议、粒度、位图和计数。

## 验收边界

两个架构的普通产物都须真实链接。头边界检查应验证公共 VMA/VMM/uaccess 和 `memory/memory.h` 入口不再传播页表实现细节，并区分真实生产后端与宿主替身。迁移现有生产夹具、独立 PMM 运行脚本及依赖源码路径的静态审计；宿主的 x86 reservation 与 ARM zone 检查须链接对应生产定义，纯夹具桩只补测试未用的启动输入。沿用现有宿主行为用例覆盖 NX、缓存别名、设备提交失败、COW、页表和 PMM；缓存/NX 断言分别观察真实驱动发出的请求和真实 MM 提交生成的叶子，不能只测已经退出生产调用链的旧 helper。无效枚举测试须核对旧 VMA/PTE/引用和 raw 发布均无副作用。x86 分开运行 syscall 与 kernel selftest，AArch64 运行 M3 SMP probe 与单核 M4 EL0 变体（要求 `aarch64_user_mm`、`aarch64_user_task_lifecycle`、`aarch64_user_svc_uaccess`、`aarch64_user_vm`、`aarch64_user_fork_cow` 都通过）。无需新增 UAPI、运行时 ops 表、跨架构 VM 权限表或根对象。共享进程地址空间、AArch64 文件/设备 mmap、procfs 远程生命周期和本轮未触及的 MMIO 行为留在本轮范围之外。

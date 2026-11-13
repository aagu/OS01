# x86_64 用户 PTE 地址与执行权限

基线 `5acc1992`。对应评审第 3 项。任务 1 的纯描述符编解码是第 4 项的前置。任务 2 在第 1、2 项完成后发布权限策略。

## 当前行为

`prot_to_page_flags` 把 RX 映射成可写，R/RW 页没有 NX。EFER.NXE 已在 BSP 和 AP 开启。4 KiB 映射与多个读写调用者使用 `PAGE_4K_MASK` 分离地址和属性，该掩码用于对齐，包含 NX 位。2 MiB 栈叶子已经使用 NX，部分遍历只局部手动清除 bit 63。

## 契约

- `PROT_READ` 为 R/NX，`PROT_READ|PROT_WRITE` 为 RW/NX，`PROT_EXEC` 和 RX 为 RX，显式 RWX 仍为 RWX。带 WRITE 但无 READ 的请求维持 `-EINVAL`。`PROT_NONE` 维持无硬件翻译，已驻留页保留 PA 与共享引用。
- 描述符地址字段按 x86 页大小提取，4 KiB 为 bits 51:12，2 MiB 为 bits 51:21，1 GiB 查询为 bits 51:30。NX、A/D、软件位及大页 PAT 位不会混入 PA。保持 x86 当前支持的物理地址范围，平台 MAXPHYADDR 能力探测留给独立变更。
- x86 语义查询报告 4 KiB PRESENT、WRITE、USER、NO_EXEC、COW 和 PROTNONE。暂存页保持当前 x86 的成功返回及 PA，删除不存在叶子的 errno 沿用原接口。AArch64 返回契约及 mmap 匿名权限范围维持现状。
- 用户权限经过 mmap、首次缺页、mprotect、COW、fork、uaccess 和销毁链。堆数据页新增 NX，栈现有 NX 在 fork 后也必须保留。ELF 装载保持现有 ELF 段权限策略，本项修复其通用描述符编解码路径。设备缓存属性和 AP 启动布局沿用当前实现。framebuffer 接受 R 或 RW 并置 NX，继续遵守可写许可。NONE、EXEC 或未知 prot 返回 `-EINVAL`，准备阶段拒绝，保留旧映射。设备 offset 的现有行为沿用。

## 设计

在 `kernel/include/arch/x86_64/pte_bits.h` 定义 `x86_pte_4k_pa`、`x86_pte_4k_flags`、`x86_pte_4k_make`、`x86_pte_2m_pa`、`x86_pte_2m_flags`、`x86_pte_2m_make`、`x86_pte_1g_pa`，输入和输出均为 `uint64_t`。4 KiB PAT 位为 bit 7，大页 PAT 位为 bit 12，替换含糊的 PAGE_PAT 名。清点所有描述符读写点，包括 `arch/mmu.h` 两个 x86 遍历及裸 fork、fault、uaccess、PMD 映射、framebuffer 直映射。普通虚拟地址和物理地址对齐继续用 `PAGE_*_MASK`。避免 `arch/mmu.h` 引入 framebuffer 类型依赖，两个头共用纯位定义。

先完成编解码及全调用链迁移再发布 NX 权限。集中编解码能覆盖 teardown 与 COW，逐处手动屏蔽 NX 容易遗漏软件标记和其他大页层级。mprotect 更新已有叶子时保留缓存属性。普通用户叶子的 stash 保留 PA、COW 及 4 KiB 缓存位。保留 ELF 直接使用的 PAGE_USER_PTE(_RO) 宏值，通过各自 VMA 或设备入口启用 NX。缺页建页时按 VMA 权限，在分配前拒绝不允许的访问。仅真正的 COW 写故障可晋升共享只读叶子。

## 验收

主机用真实 x86 后端测试 NX + COW/PROTNONE 和 PA、flags 往返，检查 2 MiB NX/PAT 的地址提取及 1 GiB 查询，fork 的 2 MiB 子栈以及 4 KiB fork/COW/uaccess 保持 NX，释放收到纯 PA。用户态在子进程检查 RX 写入和 RW 取指终止于 SIGSEGV，显式 RWX 与已切换 RX 可执行，父进程继续运行。覆盖首次访问与已有叶子两种情况。

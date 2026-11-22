# ASLR-B：x86_64 静态 PIE 加载与用户程序迁移

对应 [路线图 P1 ASLR-B](../../roadmap.md)，以前置 [ASLR-A](2026-11-21-aslr-a-mmap-base-design.md) 为基线。

## 目标与范围

x86_64 每次成功 spawn 或 exec 静态 PIE 时独立选择映像加载偏移，并在首条用户指令前完成重定位。默认产品用户程序、BusyBox 和测试镜像中的常规用户程序均迁移，包括启用时的可选应用。固定地址 ET_EXEC 继续可加载，保留既有入口、布局和启动契约，用独立构建的 ET_EXEC 夹具验收兼容路径。加载失败时保留原有进程映像，且同一个文件不会因 PIE 校验或熵失败而重新按固定地址加载。

本期范围是 x86_64、OS01 libc 的无解释器静态 PIE。动态链接、共享库、TLS、IFUNC、RELR、符号解析、RELRO、aarch64 PIE、栈地址随机化以及独立堆随机化留待后续。aarch64 继续使用自身的 ET_EXEC 加载器、启动及构建配置。现有 ASLR-A mmap 窗口与 fork 继承规则继续适用。

## 方案选择

- 采用**内核重定位、既有低址 ELF/堆包络**。在未发布页表内使用受检查的内核物理直映写入数据页，可以统一处理原生程序和 BusyBox，并复用 staged exec 的失败路径。
- 用户态 crt0 自重定位会把重定位失败移到映像提交后，并要求现有 crt0 在清 BSS、调用 libc、使用任何 GOT 前增加一条启动路径。
- 使用独立高址映像区可扩大熵窗，需要同时迁移 brk 上界、映像范围保护与相关用户地址检查。本期优先保持现有内存布局接口。

## x86_64 映像几何与生命周期

| 项目 | 契约 |
|---|---|
| PIE 链接地址 | 首个页对齐的 PT_LOAD 从虚址 0 开始并覆盖 ELF 头及 PHDR。映像页包络在链接地址 `[0, 0x02000000)` 内，排他末端可等于上界 |
| 偏移槽 | `B = 0x00400000 + ((raw & 0xffff) << 12)`，偏移区间 `[0x00400000, 0x10400000)`，65536 个 4 KiB 对齐槽 |
| 运行时地址 | PT_LOAD 起点、入口、PHDR、动态表和重定位目标按各自的链接虚址加 B。文件偏移与文件边界按原值验证 |
| 堆、栈和保护区 | `mm->start_code/end_code` 记录重定位后的 PT_LOAD 字节包络，brk 从 `ALIGN_UP(end_code, 4096)` 起，仍至 `0x203ff000`。最大偏移加最大映像跨度为 `0x123ff000`。沿用既有 ELF+堆保护区及栈 `[0x20400000, 0x20600000)` |
| 熵 | 在 x86_64 加载阶段为有效 PIE 独立调用现有 `kernel_random_get_strong()`，按小端取低 16 位，和 mmap 基址、AT_RANDOM 分别取样。不能取样时返回 `-EAGAIN`。ET_EXEC 无此次取样 |
| fork | 继承映像 VA、已完成的重定位和 auxv，复用原有 COW。后续成功 exec 独立取样 |

PIE 格式验证先于 PIE 熵调用及首个映像叶分配。映像跨度上限与现有 8192 个叶回滚上限一致。所有原址、加法、端点、页取整、文件区间及成品地址均受检查。链接类型的判断在验证同一份 ELF 头和 PHDR 快照时完成。PIE 的 e_phnum 限制为 1 至 128，表在分配前检查文件范围，越界计为格式错误。x86_64 的 PIE 校验走独立入口，共用可复用的 ELF64 头和 PHDR 检查。现有公共 ET_EXEC 校验契约继续适用于原有调用者。

## 静态 PIE 格式与重定位

- 仅接受 ELF64、小端、EM_X86_64、ET_DYN。PT_LOAD 沿用文件、内存尺寸和不相交字节区间检查，首段 p_offset 为 0，要求各段 p_offset 与 p_vaddr 模 4096 同余、p_align 为 0、1 或至多 4096 的 2 的幂。入口须位于 PF_X 文件字节内。PIE 各 PT_LOAD 的页包络互不相交，单段 PF_W|PF_X 和可执行栈请求均拒绝。保留 ET_EXEC 既有共享页兼容规则。
- 要求 e_ehsize 为 64，e_phoff 至少为 64 且按 8 字节对齐。PHDR 表全长必须由链接地址为 0、包含 ELF 头的 R/RX 文件支持 PT_LOAD 覆盖，从文件偏移翻译为运行时 VA。装载段时将已验证的 ELF 头和 PHDR 快照写入这段，保证 AT_PHDR 指向参与加载决策的同一份表。若存在 PT_PHDR，须唯一且与计算出的表地址、偏移和长度一致。拒绝 PT_INTERP、PT_TLS 和 PT_GNU_RELRO。PT_DYNAMIC 恰好一个，由单个 R/W、NX、文件支持的 PT_LOAD 覆盖，按同一地址映射验证其文件偏移；动态表起点按 8 字节对齐，p_filesz 与 p_memsz 相等，含 1 至 64 个 16 字节项。必须找到 DT_NULL，到首个 DT_NULL 后停止。
- 功能型动态标签限定为 DT_RELA、DT_RELASZ、DT_RELAENT、DT_RELACOUNT。前三者三项齐全或同时缺省，DT_RELAENT 为 24，DT_RELASZ 是 24 的倍数；若有 DT_RELACOUNT，须等于总项数。允许链接器生成的 DT_STRTAB、DT_STRSZ、DT_SYMTAB、DT_SYMENT、DT_HASH、DT_GNU_HASH（由加载器忽略）、零值 DT_DEBUG 和值恰为 DF_1_PIE 的 DT_FLAGS_1，后者必需。已识别的非空标签不得重复。其他非空标签一律拒绝，包括 DT_NEEDED、DT_TEXTREL、DT_JMPREL、DT_REL、DT_RELR。零尺寸 RELA 三元组的 DT_RELA 值无需寻址，DT_RELACOUNT 若存在须为 0。三元组缺省时不接受 DT_RELACOUNT。
- 非空 RELA 表按 8 字节对齐，全长由单个文件支持的 PT_LOAD 覆盖，最多 65536 项。RELA 表与动态表的链接地址区间不得相交。每项只接受符号索引 0 的 R_X86_64_RELATIVE，目的地址自然 8 字节对齐，完整位于一个可写不可执行 PT_LOAD 的内存字节范围，可落入 BSS。拒绝写入动态表或 RELA 字节区。每个加数按有符号 ELF64 Rela 语义检查，接受的链接态加数在 `[0, image_page_end]`，因此允许映像起点及映像尾后指针。结果是 64 位小端 `B + r_addend`。按 RELA 表顺序应用。多条记录可写同一目的，最后一条生效，每条都须独立通过检查。
- 在首个映像叶分配前验证动态元数据和每条重定位。检查时取得的重定位记录由加载器拥有，应用时使用同一份记录，避免依赖映射中可能变化的元数据。记录可分块保存在内核内存中，遵守 slab 分配上限，全部失败边均释放。写入只通过本次映像拥有的 4 KiB PTE 对应的物理直映，在段拷贝与清零完成后执行；PIE 遇到既有叶即失败。用户页自安装起维持最终权限。
- 格式、边界、不支持的标签/记录、写入冲突及短读为 `-ENOEXEC`，分配或建页失败为 `-ENOMEM`，STRONG 熵失败为 `-EAGAIN`。x86_64 spawn/exec 传播加载错误，遵循现有 staged cleanup。失败时回收此次安装的叶和临时内存、交由调用方销毁新地址空间的中间页表，保留旧 mm/CR3、任务状态和 loader 输出。直到所有映射和重定位完成才更新加载结果。

## 启动 ABI

PIE 初始栈保持 16 字节对齐及 `argc | argv[] | NULL | envp[] | NULL | auxv[]` 格式。保留 AT_PLATFORM 和现有 16 字节 STRONG AT_RANDOM，增加 AT_PHDR（运行地址）、AT_PHENT（56）、AT_PHNUM、AT_ENTRY（偏置后入口）、AT_BASE（0）、AT_PAGESZ（4096），以 AT_NULL 结束。ET_EXEC 仍用原三对。栈容量检查按最大 auxv 大小计费，保持原 argv/envp 限额及安全余量。x86_64 crt0 延用现有 libc 调用约定与 Linux ABI 标记判定，移除 x86_64 的再次清 BSS，加载器对 ET_EXEC 和 PIE 都负责清零，避免抹去落入 BSS 的重定位结果。aarch64 crt0 与 auxv 无改动。

## 用户产物与构建缓存

x86_64 默认用户程序和所有实际参与链接的归档使用 PIE 兼容代码模型。迁移 libc、libm、用户 builtins、libgfx、libgui（含 LVGL）及独立 Doom engine archive，检查发布物是否还有其他被消费的归档。x86_64 libc/libm 使用 small code model，明确拆开用户与 libk 的编译标志来源，libk 保留原代码模型。普通应用和可选应用共用零基址 PIE linker script，保留原 ET_EXEC script 供夹具使用。PIE 脚本显式声明包含头和程序头的 RX LOAD、页分隔的 RW/NX LOAD 及 PT_DYNAMIC，纳入 `.rela.dyn`、`.dynamic`、GOT、动态字符串和符号表等产物。使用 LLD，4 KiB 最大页，禁止解释器、textrel、RELR 和 RELRO。保留 x86_64 的栈保护与现有 ABI，不向 libk、内核 runtime 或 aarch64 传播 PIE 标志。

BusyBox 在现有私有构建目录迁移。编译启用 PIE，最终 clang 链接使用 LLD、静态 PIE、OS01 crt0 和 libc。中间可重定位链接保持 `-r`，仅最终产物获得 PIE linker script 与最终链接选项。勿依赖 BusyBox 自带 CONFIG_STATIC 和 CONFIG_PIE 的组合产生目标格式；用 repository adapter 显式传递两个阶段的参数。保持 BusyBox 源树、受控子 make 环境、发布锁及 sysroot lease。用户编译/链接 stamp、sysroot generation 配方指纹、用户 runtime 变体和 BusyBox input receipt 必须覆盖各自有效的 PIE 标志、脚本和适配输入。验证增量由旧输入切换后重编、相同输入保持稳定。产物格式审计覆盖发布的真实 ELF 并检查禁止项，覆盖用户 runtime link probe、正常镜像和 `DOOM_TEST=1`、`FB_RESOLUTION_TEST=1` 的用户构建变体。旧 ET_EXEC 夹具独立构建，仅安装于 x86_64 systest rootfs，并单独断言固定地址。

## 验收

| 层 | 验收 |
|---|---|
| 宿主 | 扩充 ELF 校验和真实 loader 夹具，固定熵样本覆盖首末槽、零非零重定位、入口/PHDR/堆边界、数据与 BSS 指针、页权限、畸形/截断动态表、重定位溢出及越界、外部或文本重定位、旧 ET_EXEC。注入熵、分配、读取和建页失败，检查旧页、引用、输出、临时缓冲和 staged spawn/exec 回滚。成功 PIE 的 STRONG 取样顺序为 PIE、mmap、AT_RANDOM，各阶段独立注入失败并核对取样数。ET_EXEC 成功恰取 mmap 和 AT_RANDOM 两次。更新测试元数据并运行 `make regen-test-catalog`、`make test-host` |
| 构建与静态 | 在 x86_64 对全部迁移产物及 BusyBox 校验 ELF 类型、PHDR、W^X、动态白名单和重定位集合，通过 `make test-static`、`make check-test-catalog`、`make test-harness`。aarch64 使用 `make PROFILE=aarch64-clang user test-aarch64-audit` 验证产物仍为 ET_EXEC 且 x86 私有实现未进入内核 |
| syscall | 顶层 `make OS01_SYSTEST=1 test-syscall`。8 次 fork+exec 通过 pipe 上报同一探针入口、重定位数据、完整 auxv 和实际读写，全部符合槽位和 auxv 契约，至少 7 个不同槽。AT_BASE 清 errno 后值为 0 且 errno 为 0。fork 继承及 COW 隔离、固定 ET_EXEC 夹具真实执行。现有 `test_protected_ranges` 从运行时 PT_LOAD 取得实际映像页，选取真实 brk 之后的未提交页验证堆预留，并检查 `/proc/self/maps` 的实际映像地址。旧 ASLR-A/mmap/brk/exec 回归继续通过 |
| 产品 QEMU | x86_64 `make test-qemu SUITE=phase-0` 到达 BusyBox ash。以已迁移 init 和 BusyBox 的 ELF 审计与实际引导配对核验。`make test-qemu SUITE=terminal-present` 验证迁移后 terminal 实际运行。通过 `make test-qemu SUITE=gfx` 的既有 stdio runner 发送 `/bin/busybox printf '%s%s\n' OS01-PIE- APPLET-OK`，匹配独立输出行 `OS01-PIE-APPLET-OK`，覆盖 BusyBox applet 的执行 |

实现及验收完成后更新内存和构建文档、路线图与 changelog，记录实测结果与栈随机化边界。

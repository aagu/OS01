# ASLR-B：x86_64 静态 PIE 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal**：x86_64 默认用户产物（含 BusyBox）使用静态 PIE，内核在 spawn 和 exec 提交前随机装载并完成重定位。

**Architecture**：x86 私有 PIE 校验和重定位由现有每页 ELF 装载器调用，复用 staged exec 及映像/堆保护逻辑。原有 ET_EXEC 保留独立验证路径和固定地址启动 ABI。构建切换后使用宿主故障注入、发布 ELF 审计和真实来宾启动验证。

**Tech Stack**：C、x86_64 SysV ELF、Clang/LLD、GNU Make、Python 3、QEMU/OVMF。

**Spec**：[已通过评审的 ASLR-B 设计](../specs/2026-11-22-aslr-b-static-pie-design.md)。

## 全局约束

- x86_64 PIE 从零链接。4 KiB 页包络排他末端至多 `0x02000000`。偏移 `0x00400000 + ((raw & 0xffff) << 12)`，最大偏移 `0x103ff000`，最大映像末端 `0x123ff000`。既有堆上界 `0x203ff000`、固定栈和 ASLR-A 继续适用。
- PIE、mmap、AT_RANDOM 分别取 STRONG，fork 继承，熵失败 `-EAGAIN`。格式或短读 `-ENOEXEC`，分配或建页 `-ENOMEM`。所有新映像处理在提交前完成。
- PT_LOAD 权限从首次建页起固定。PIE 拒绝 W+X、可执行栈、共享叶、PT_INTERP、PT_TLS、PT_GNU_RELRO。动态表为 1–64 项，PHDR 为 1–128 项，RELA 为 0–65536 项。标签白名单及目标/加数范围见 spec。
- 只给 x86_64 用户构建加 PIE 标志。保留 ET_EXEC 兼容性、aarch64 用户/内核、x86_64 libk/内核 runtime 的现有模式与 ABI。测试 ET_EXEC 仅进入 x86_64 systest。
- 更新测试定义时使用 `@os01-test` 与 catalog 生成入口。systest 从仓库顶层运行，并和 `KERNEL_SELFTEST=1` 分开。

## 文件与评审重点

实现链由 Task 1 的 x86 ELF 格式校验与装载结果、Task 2 的 exec/栈 ABI、Task 3 的构建迁移及 ELF 审计、Task 4 的来宾运行验收组成。Task 1 和 Task 2 期间默认产物仍可用既有 ET_EXEC 构建运行。

- FILEHDR/PHDR 与实际加载内容的关系由 Task 1 的短读和文件变化夹具验证。
- 接近末槽的最大合法跨度及首个非法字节由 Task 1 固定熵样本验证。
- BSS 中的 RELATIVE 修补、重复目的的应用顺序由 Task 1 验证。Task 2 检查 crt0 对 BSS 清零符号的引用已移除。Task 4 验证真实来宾的文件支持数据区和函数指针重定位。
- 第一次、第二次和第三次熵失败的旧进程及任务表状态由 Task 2 分别验证。
- 构建键变化与命令回显假阳性分别由 Task 3 的两次增量构建和 Task 4 的 runner 夹具验证。

## Task 1 — 静态 PIE 验证与逐页装载

**生产文件** `kernel/include/fs/elf.h`、`kernel/include/arch/x86_64/{elf_image.h,user_layout.h,aslr.h}`、`kernel/fs/elf_layout.c`、`kernel/arch/x86_64/fs/{elf.c,elf_pie.c}`、`kernel/arch/x86_64/memory/aslr.c`、必要时 `kernel/arch/x86_64/make.config`。同步 `kernel/arch/x86_64/sched/exec.c` 的两个调用参数及宿主桩，保持编译贯通。

**宿主侧** `hosttests/cases/test_elf_load_4k.c`、`hosttests/mock/elf_load/elf_load_stubs.{c,h}`、`hosttests/mk/{fs,sched}.mk` 与现有 boot/exec 桩。

**接口** 在 x86 私有 `elf_image.h` 定义 `x86_elf_image_t`，字段 `uint64_t entry, phdr, phnum` 和 `bool is_pie`。将 x86 的 `int elf_load(vfs_node_t *, mm_t *, x86_elf_image_t *)` 声明移入该头。失败不改结果，成功时读取 `image.entry`。新增 `int x86_aslr_get_pie_bias(uint64_t *out_bias)`，成功写出 B，STRONG 失败原样保留输出。公共 `elf_layout_validate{,_machine}` 仍只支持 ET_EXEC。

- [ ] 扩展真实 ELF loader 测试并接真实 PIE 验证和 ASLR 模块，STRONG 接口使用可排队的 32 字节结果桩。将低址平铺 PTE 桩换为按访问的 VA 保存的稀疏记录，保持既有 helper 和旧页撤销断言。给 ET_DYN 夹具添加 `test_pie_load_bias_and_permissions`（样本 `0, 0xffff, 0x10000` 对应 `0x400000, 0x103ff000, 0x400000`。末槽与页末 `0x02000000` 对应运行时 `0x123ff000`，页末多一字节须拒绝），`test_pie_rela_and_bss`（数据和 BSS 指针值为 `B+addend`，尾后指针恰为 `B+image_page_end`，重复目标采用后值），`test_pie_reject_before_mapping` 和 `test_pie_failure_unwinds`。覆盖 RELA 缺省/零尺寸和带 RELACOUNT、PHDR/文件越界、终止缺失、未知和重复标签、非零符号、非 RELATIVE、负加数、溢出、无权限/未映射/未对齐目标、表相交、共享页与 W+X。增加 `test_pie_phdr_snapshot`，让读桩在预读后改写文件中的 ELF 头和 PHDR 字节，断言最终映射的头和表仍等于预读快照。测试坏输入无熵无叶，注入短读、分配和建页故障时结果哨兵及旧页保持、已安装叶与临时缓冲均释放。
- [ ] 运行 `make test-host`，确认新增 PIE 成功用例在实现前失败于 ET_DYN 被拒，既有 ET_EXEC 用例通过。若因新增符号缺失而断在编译，先用返回 `-ENOEXEC` 的接口桩到达该失败。
- [ ] 在 `fs/elf.h` 增加标准 ET_DYN、缺少的 PT/DT 常量和 ELF64 Dyn/Rela 格式定义，断言结构尺寸为 16 和 24。在 `elf_pie.c` 中提供 `int x86_elf_pie_prepare(vfs_node_t *, const elf64_ehdr_t *, const elf64_phdr_t *, uint64_t file_size, x86_elf_pie_t *)` 与 `void x86_elf_pie_dispose(x86_elf_pie_t *)`。私有描述保存链接态 PHDR 地址、映像页末端及有序 RELA 块。复用通用头/表边界验证，类型和地址策略各走自己的入口。PIE header preflight 在读取 PHDR 前检查范围、计数和项长。prepare 检查 spec 中每项几何、标签和重定位契约。将 RELA 按最多 256 项的块连续读入和保存，每块至多 6144 字节记录数据，使用经验证的链接 VA→文件偏移转换读动态表和 RELA。失败释放已取得块，清空描述。
- [ ] 在 `aslr.c` 实现 `x86_aslr_get_pie_bias` 和几何静态断言，接入 `elf.c` 的 PIE 验证成功分支。复用现有分段拷贝及 rollback，使用检查过的 B 后地址产生叶、复制区间、`start_code/end_code`、入口及 PHDR。PIE 路径遇到既有叶失败。复制后使用本次拥有的 4 KiB 叶对应的物理直映写入 ELF/PHDR 快照与保存的 RELA，按页切分 PHDR 快照。可按 VA 走已安装 PTE，依靠 PIE 禁用既有叶、PT_LOAD 页不相交及撤销表所有权。使用 64 位写入或等价的 8 字节复制。成功再提交 mm 边界与加载结果。保持 ET_EXEC 的原有页复用及权限逻辑。
- [ ] 运行 `make regen-test-catalog`、`make test-host`。确认旧 ELF/布局用例与新增故障注入全部通过。提交格式与装载器改动。

## Task 2 — staged exec 和启动 ABI

**生产文件** `kernel/arch/x86_64/sched/exec.c`、`libc/arch/x86_64/crt0.S`。**测试文件** `hosttests/cases/{test_x86_aslr_exec,test_boot_spawn_failure,test_boot_services_spawn,test_boot_services_vfs}.c`。`exec.c` 的 `<sys/auxv.h>` 来自 `libc/include/sys/auxv.h`，已有各项编号。

**接口** 给 `setup_user_stack` 增加 `const x86_elf_image_t *image`，成功路径用 `image.entry` 初始化启动入口。现有仅测 AT_RANDOM/AT_PLATFORM 的任务自测调用点传零初始化的 ET_EXEC 描述。

- [ ] 使用真实 `exec.c` 的 `test_x86_aslr_exec` 夹具，设置伪加载结果和可控 PIE 取熵；Task 1 的真实 loader 夹具独立验证第一次取样。扩展成功/失败测试为两条建像路径各一组。断言 PIE 成功 3 次和 ET_EXEC 成功 2 次，依次注入三个阶段的 `-EAGAIN`，断言计数、引用释放、所有 mm 销毁和不发布任务/不切换 CR3。对 `-ENOEXEC/-ENOMEM` 分别断言传播。把 fake 初始栈转成用户 VA 读取，逐项核对 `argc`、argv/envp 结束位、16 字节对齐、原三对或 PIE 九对、AT_RANDOM 对应最后一次熵调用。使用有效范围内的大 argv/envp 覆盖容量计费。
- [ ] 运行 `make test-host`，确认新传播及 PIE auxv 断言复现缺口。
- [ ] 在两个加载调用点保留原有 staged 清理顺序和引用所有权，直接传播 loader 状态。让容量计算和建栈共用同一个最大对数常量 9。为 PIE 追加 `AT_PHDR, AT_PHENT=56, AT_PHNUM, AT_ENTRY, AT_BASE=0, AT_PAGESZ=4096`，ET_EXEC 保留原三对，AT_NULL 为最后一对。保留当前 ABI 标记判定，x86_64 crt0 删除二次清 BSS，继续保存入口 rsp 并以原参数顺序调用 `__libc_start_main`。
- [ ] 运行 `make test-host`、`make KERNEL_SELFTEST=1 test-kernel-selftest`，确认真实生命周期回归和 AT_RANDOM/AT_PLATFORM 自测通过。对 x86 sysroot 的 `usr/lib/crt0.o` 运行 `llvm-readelf -r`，确认 `__bss_start/__bss_end` 的引用均已消失。提交 exec 和启动 ABI 改动。

## Task 3 — 用户产物及 BusyBox 迁移

**生产文件** `libc/arch/x86_64/user-pie.ld`（新）、`libc/{Makefile,arch/x86_64/make.config}`、`user/{Makefile,doom/engine.mk}`、`libgfx/Makefile`、`libgui/Makefile`、`mk/profiles/x86_64-clang.mk`、`mk/components/{sysroot,user,run}.mk`。**测试和审计** `qemutests/mm_header_boundary.py`、`tools/tests/test_user_build.py`、`tools/tests/audit_user_pie.py`（新）。旧 `libc/arch/x86_64/user.ld` 仍用于 Task 4 的 ET_EXEC。

- [ ] 将 `test_user_build.py` fixture 配齐新 linker script，移除命令行的 `-fno-pie` 覆盖，使 fixture 使用 x86 真实编译配置。在测试中加入 x86 发布清单的格式审计断言、BusyBox 中间命令与最终命令隔离断言、首轮迁移和无输入变化两次构建的缓存断言。新增 `audit_user_pie.py`，从 ELF 文件头和 PHDR/DYNAMIC 检查与 spec 一致的类型、页权限和动态/RELA 格式，含合法零表、固定 ET_EXEC、带解释器、坏目标等自测夹具。由顶层 `make test-user-pie` 将真实 `USER_ARTIFACTS`、runtime probe 和 BusyBox 的路径作为参数，不用目录 glob。
- [ ] 运行 `make PROFILE=x86_64-clang test-user-pie`。预期现有默认 ET_EXEC 产物被拒。
- [ ] 在 x86 用户编译配置统一 `-fPIE -mcmodel=small`，保留各自红区和栈保护约束。覆盖普通和目录 app、runtime probe、用户 builtins、Doom engine、libc/libm、libgfx、libgui/LVGL，并按实际消费补齐遗漏。将 `LIBK_CFLAGS` 的基线从用户专用 flags 中拆出，保留 libk 的原有有效参数。核对 user 分支的两处 CFLAGS 定义和回填的测试宏。
- [ ] 在 `user-pie.ld` 用 `FILEHDR PHDRS`、`SIZEOF_HEADERS`、页隔离 RX 和 RW/NX、映射在 RW/NX 的 PT_DYNAMIC。显式接纳 GOT、`.rela.dyn`、`.dynamic`、动态 hash/string/symbol 表和数据/BSS，保留 `_start`。x86 以 LLD `-static -pie --no-dynamic-linker -z max-page-size=4096 -z text -z norelro --pack-dyn-relocs=none` 生成产物。链接器 script 是所有链接规则的依赖。只有 x86 用户路径选新脚本。
- [ ] 在 x86 BusyBox adapter 保留私有树及 lease。编译 flags 加 `-fPIE`；在传给最终 trylink 的 `CFLAGS_busybox` 传 `-static-pie -nostdlib`、sysroot libdir、CRT 对象和 LLD 的等价 linker 参数，在 x86 私有 `.config` 写 `CONFIG_EXTRA_LDLIBS="c"`，保证 libc 出现在 trylink 每轮的最终库组中。x86 的中间构建配置去掉共用 `CONFIG_EXTRA_LDFLAGS` 中的最终链接选项，确保中间仍仅做 `-r`。模板保留产品使用的 `CONFIG_STATIC=y`，x86 通过命令行赋值覆盖 Kbuild 的 `CFLAGS_busybox`，保持 `CONFIG_PIE` 未启用，由 adapter 控制两个阶段。fresh 和 incremental 分支都从已取得 lease 的 `$$genabs/usr/include` 和 `$$genabs/usr/lib` 解析头、库目录及 crt0，旧 receipt 以同一 generation 的字段判断。避免在子 make 中再次解析可变 sysroot symlink。在 x86 的 CC/LD 入口钉住 `-fuse-ld=lld`，验证实际 argv 和中间 ELF 的 ET_REL。aarch64 沿用现有 adapter 模式。
- [ ] 把 PIE 模式、有效编译/链接标志和工具指纹加入所属缓存键。更新 libc/libm、libgfx、libgui 的 staging 输入键，以及 libc compile key、Doom engine key、user/runtime 变体和 BusyBox receipt。libgfx、libgui 分别新增按内容更新的 `.compile.key`，记录当前工具及编译 flags，挂到各自全部对象的普通依赖。单独验证已有旧 `.o` 和旧键时这两个组件都在同一 build dir 重新编译。BusyBox 额外纳入实际脚本、adapter 及配置输入，一致地传给 fresh/incremental 两条子 make。切换迁移前后配置后检查确实执行编译/重链；用同输入重跑比较 generation、receipt 和产物 hash/mtime，保持既有无变化发布语义。
- [ ] 分别运行 `make PROFILE=x86_64-clang test-user-pie`、`make PROFILE=x86_64-clang DOOM_TEST=1 test-user-pie`、`make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 test-user-pie`。确认三份真实发布清单通过。运行 `make PROFILE=aarch64-clang user test-aarch64-audit` 和 `make PROFILE=aarch64-clang test-user-pie`，后者按 profile 在审计工具中启用 `--expect-exec`，检查该 profile 的真实发布清单（含 runtime probe 和 BusyBox）。将 `x86_elf_pie_` 纳入 `test-aarch64-audit` 的符号排除，更新 `qemutests/mm_header_boundary.py` 的 x86 私有头和宏探针，运行 `make PROFILE=x86_64-clang test-static CASE=mm-header-boundary` 及 aarch64 同名 target。运行 `python3 tools/tests/test_user_build.py`、`make test-static`，确认构建测试和静态门通过。提交用户构建和审计改动。

## Task 4 — 镜像、syscall 和 QEMU 验收

**运行与测试文件** `user/systest/{systest.c,systest.h,test_mem.c}`、`user/systest/fixtures/aslr_b_exec.S`（新）、`user/Makefile`、`mk/components/{user,image,run}.mk`、`config/{rootfs.mk,test-scenarios.json}`、`qemutests/{run_test.py,test_gfx_runner.py,test_run_test_harness.py}`。**文档** `docs/memory/cow-mmap.md`、`docs/build/build.md`、`docs/roadmap.md`、`docs/changelog.md`。

- [ ] 在 `systest.c` 新增 `pie-probe` 自重入分支，在 `test_mem.c` 定义 `void test_aslr_pie(void)`，注册 `aslr_pie`。使用 fork/exec 和既有 pipe 通信模式传回入口、实际 PHDR/LOAD、偏移、数据指针和 auxv 检查结果。用 volatile 函数指针/对象指针强制真实重定位，比较被引用对象和值并做实际调用和读写。父进程连续 8 次执行，断言偏移在窗口且页对齐，至少 7 个不同槽。使用 LLD 首段的 ELF 头和 PHDR 检查加载偏移，独立比较 AT_ENTRY 与运行时 `&_start`，并验证 AT_PHENT、AT_PHNUM、AT_PAGESZ、AT_PLATFORM、AT_RANDOM、AT_NULL；对 AT_BASE 清 errno 后同时检查值和 errno。fork 单独核对映像地址及 COW 写隔离。更新 `test_protected_ranges`，从当前程序 PHDR 确定实际映像页，用 brk 加一页选取未提交堆地址，并验证 `/proc/self/maps` 的真实映像基址。
- [ ] `aslr_b_exec.S` 使用独立 `_start`、既有 ET_EXEC linker script，运行时检查自身入口等于 `0x400000` 后通过 `int $0x80` 以 73 退出，检查失败以 74 退出。接入正常用户构建的并行发布组与 sysroot 锁下的复制步骤，限 `OS01_SYSTEST=1` 且 x86_64，rootfs 依赖和清单同时加入 `/bin/aslr_b_exec`。不将该文件纳入 PIE 清单。测例验证真实 fork+exec 返回 73，并以 ELF 审计确认此夹具是 ET_EXEC。
- [ ] 扩展 `test_gfx(tester)` 现有 stdio shell 驱动，在提示符后执行 `/bin/busybox printf '%s%s\n' OS01-PIE- APPLET-OK`，用独立整行匹配接受 LF/CRLF。测试 FakeProcessSession 回显不通过、真实输出通过、缺行失败，原 GFX/Tetris 步骤继续运行。将新增宿主和来宾场景登记到 catalog，跨层复用写明独立风险。保留两类 gfx 现有 runner 夹具的 replay/失败/晚发 panic 判据。
- [ ] 运行 `make regen-test-catalog`、`make check-test-catalog`、`make test-harness`、`make OS01_SYSTEST=1 test-syscall`。确认完整 syscall 门通过，旧 ASLR-A、brk、ET_EXEC 都有独立通过证据且 PIE 随机采样达标。
- [ ] 运行 `make test-qemu SUITE=phase-0`、`make test-qemu SUITE=terminal-present`、`make test-qemu SUITE=gfx`。确认 PID 1、ash 提示、terminal runner 和独立 BusyBox applet 输出行，并核对对应镜像使用 Task 3 审计过的产物。全部门禁通过后登记文档和 changelog，移出 roadmap 的 ASLR-B 并保留栈随机化后续范围。提交来宾测试与验收文档。

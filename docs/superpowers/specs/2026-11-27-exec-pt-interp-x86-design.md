# x86_64 exec PT_INTERP 通路(T1)设计

2026-11-27

## 目标与范围

roadmap P5「动态链接器」的第一片:内核 exec 通路识别 `PT_INTERP`,装载解释器镜像并交接控制流。仅 x86_64;aarch64 留给 T4。不含符号重定位(GLOB_DAT/JUMP_SLOT)、DT_NEEDED 依赖解析、共享 libc——它们是 T2/T3。

交付:带 PT_INTERP 的 ET_DYN 主程序能 exec 成功,内核把 `/lib/ld.so`(最小骨架)装入同一地址空间独立 base,入口跳 ld.so,ld.so 经 auxv(AT_ENTRY)跳回主程序入口,运行到 exit;整链路有 systest 运行证据与宿主形状契约测试。

## 现状

- 全部用户程序 static PIE(`--no-dynamic-linker`);x86_64 静态 PIE 装载器(`elf_pie.c`)拒绝 PT_INTERP/PT_GNU_RELRO,只接受 R_X86_64_RELATIVE;ET_EXEC 路径忽略非 PT_LOAD。
- auxv 已发布 AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY/AT_PAGESZ/AT_PLATFORM/AT_RANDOM,AT_BASE 已写入但恒为 0(`exec.c:266`)。ASLR-B bias 采样、静态 TLS(Variant II)已闭环。
- `elf_load` 单镜像:静态 PIE 走 `x86_elf_pie_prepare` + 内核自应用 RELATIVE 重定位。

## 方案

| 方案 | 取舍 |
| --- | --- |
| 内核装载 interp + 交接(本设计) | 复用既有 PIE 装载/重定位/回滚机制,增量最小;T2 只需把符号重定位移交 ld.so。采用。 |
| 内核一步到位解析 PT_DYNAMIC 符号 | 把 T2 的工作塞进内核,静态 PIE 之外再造一条重定位器,T1 无法收敛。 |
| 先宿主搭 ld.so 再动内核 | ld.so 骨架本身只是验证载体,没有 exec 通路就没有端到端证据。 |

## 语义(T1 契约)

1. `elf_load` 检测到 `PT_INTERP`(仅 ET_DYN;ET_EXEC 带 PT_INTERP 由原装载路径的静默忽略改为**显式拒绝**,避免无声跳过解释器)时:读 interp 路径(≤256 B 含 NUL,须以 `/` 开头,截断或超长 → `-ENOEXEC`),VFS 打开,验证其满足现有静态 PIE 形约束(允许自身无 PT_INTERP),以独立 ASLR-B bias 装载为第二镜像。装载失败(含与主镜像页冲突重采样 16 次仍失败)→ `-ENOMEM` 整体回滚,exec 失败。
2. 主镜像 RELATIVE 重定位仍由内核装载期自应用(T1 不移交);测试主程序以 `-nostdlib` 全局部符号链接,保证零符号重定位。
3. auxv:`AT_BASE` = interp 装载 bias(无 interp 保持 0,静态 PIE auxv 字节不变);`AT_ENTRY`/`AT_PHDR`/`AT_PHNUM` 仍指主程序。入口:`regs->rip` = interp 入口(无 interp 维持主入口)。
4. `x86_elf_pie_prepare` 增加 allow-interp 形态:仅放行 `PT_INTERP` 段(记录 p_offset/p_filesz),DT tag 白名单、`DF_1_PIE` 等约束不变(实测 lld `--dynamic-linker` 链接产物后以宿主契约测试钉住,若 `DF_1_PIE` 缺失再放行「有 PT_INTERP 时不强制」,在 plan 中裁决)。
5. PT_TLS:interp 声明 PT_TLS → 拒绝(T1 骨架无 TLS);主程序静态 TLS 流程不变。
6. 已知限制(记录,不处理):interp bias 落在堆增长窗内时,brk 顶到 interp 已占用页返回 `-ENOMEM`(malloc 失败而非破坏);mmap 窗口放置留 T2。

## ld.so 骨架与测试主程序

- `ld.so`:`-nostdlib` 自带 `_start`,初 rsp 整体传 C;解析 auxv 取 AT_BASE/AT_ENTRY,`write(1)` marker,恢复原 rsp 后 `jmp *AT_ENTRY`。不搬栈、不解析符号、不映射任何段。
- 测试主程序 `dyn_exec`:同样 `-nostdlib`,直写 syscall;入口后校验自身 auxv(AT_BASE≠0、AT_PHDR 指自身 phdr、AT_ENTRY=自身入口),写第二个 marker,exit(0)。systest fork/exec/waitpid 断言 rc=0 且串口见两 marker 顺序出现。

## 验收与证据

| 层 | 通过条件 |
| --- | --- |
| 宿主形状契约 | hosttests 解析 `ld.so`/`dyn_exec` ELF:前者为可被静态 PIE 路径接受的 ET_DYN;后者 PT_INTERP=/lib/ld.so、RELATIVE-only 重定位、DT tags 全在白名单。 |
| 内核回归 | `make test-host`、`make test-static` 全绿;auxv/AT_RANDOM 自测不受影响(AT_BASE=0 路径字节不变)。 |
| QEMU 运行证据 | systest 新用例:fork+exec `dyn_exec` → waitpid rc=0,串口先后出现 ld.so marker 与主程序 marker。 |

roadmap T1 行与 changelog 在闭环时同步。

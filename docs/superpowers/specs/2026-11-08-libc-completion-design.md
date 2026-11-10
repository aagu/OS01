# libc 补全与语义修复设计草案

日期：2026-11-08。修订：R3（独立评审第二轮后）。状态：供审阅；本轮只写设计和实施计划，未授权开始实施，也不声称方案已获批准。

## 目标和范围

落实本次 libc 扫描答复中除 glob 外的改造：时间、环境变量、socket shutdown、stdio、scanf、路径、数值转换、分配、目录、regex、exec 包装，以及 strtol、calloc、fopen 的既有语义问题。每组可单独交付和回归；整体完成以对应计划的覆盖表全部通过为准。

用户最初要求覆盖上述全部改造，随后明确 glob 暂不覆盖；glob/globfree 和仅为其增加的 fnmatch 改造排除在本轮之外。下列技术选择是作者提出的默认方案，不记作用户已经同意的决定。

## 方案比较

1. **在现有 libc 内分组补全（推荐）**：保留 syscall ABI、分配器、格式化器和构建体系；对正则和浮点解析等复杂算法局部移植成熟代码。改动能逐组验收，但须显式处理现有 FILE/头文件/BusyBox 兼容问题。
2. 整体移植成熟 libc：覆盖广，但启动代码、syscall ABI、TLS、locale、线程和 libk 分层都会扩大，超出此次缺口修复的合理范围。
3. 给每个调用者添加 fallback：短期局部生效，但 libc 继续暴露空壳和未定义接口，不能满足本次全部补全目标。

本草案采用方案 1，实施前允许用户修订。

## 全局约束

- syscall 编号、boot_context、内核/用户共享结构不变；允许为真实 append 增加内核私有 VFS 回调并全量重编消费者；不新增线程、TLS 或动态链接模型。
- x86_64 用户态单线程；SMP 内核不能作为用户态 pthread 已完整支持的依据。
- aarch64 当前只构建 libk 子集，不承诺此次建立 aarch64 用户态 libc；所有现有 aarch64 libk 消费者必须仍能构建。
- libc 公开头留在 `libc/include/`，私有头放 `libc/include/<模块>/`；新增内核源/公开头必须对称。第三方内部头可以保留上游目录布局并限于私有 include path。
- 新接口使用 C/POSIX 真实签名，声明在 include guard 和 C++ linkage 内；不以无条件成功掩盖未实现能力。
- 不把静态 host 测试中宿主 libc 的实现误当作 OS01 实现；生产对象须使用符号前缀或隔离驱动链接，oracle 单独进程运行。
- 所有产物只写 profile build 目录；只经仓库根 make 入口；不得改写第三方子模块来适配 BusyBox。
- syscall E2E 使用 `make OS01_SYSTEST=1 test-syscall`，不混入 `KERNEL_SELFTEST=1`；如涉及新共享布局另跑 `test-kernel-layout`。
- 所有测试/审计的成功声明必须有实际输出；当前工作区其它启动/fixture 改动不属于本计划。

## 模块契约

### 分配与环境

用户 libc 的 `calloc` 和 `reallocarray` 在乘法溢出时返回 NULL、errno=ENOMEM；libk 同样拒绝溢出并返回 NULL，但不引用用户态 errno（libk 不定义该全局）。失败的 realloc 不改变原块。malloc 自身的取整和 header 加法也必须检查溢出。零大小沿用现有可返回 NULL 的策略并写入测试契约。

`posix_memalign` 要求 alignment 为 sizeof(void*) 的倍数且为 2 的幂，失败返回 EINVAL/ENOMEM、不改 *memptr、不通过 errno 报错；`aligned_alloc` 额外要求 size 为 alignment 的倍数。成功结果可直接 free/realloc，不能只返回普通块内部地址。调整分配器时以现有块元数据分割实现对齐，不给所有 free 路径引入不受验证的“前置指针”。posix_memalign/aligned_alloc 本轮仅在用户 libc 定义（源码以 !__is_libk 包围）；公开头标注用户态能力，并在 __is_libk 模式隐藏这两个新增声明。libk 保留原有 malloc/calloc/free/realloc 的 slab 路径，reallocarray 可复用溢出检查；不添加对齐分配 stub，也不承诺扩展 slab。API 审计分用户全量集合与 libk 既有子集/安全共享集合，libk archive 不应定义这两个用户态对齐符号。

`environ` 使用一个统一表。继承的字符串不释放；setenv 拷贝并拥有字符串，putenv 借用调用者字符串。首次修改复制指针表并维护逐项 ownership；clearenv/unsetenv 只释放 libc 拥有的项。失败保持旧环境。空 name、含 '=' 的 name 拒绝；putenv 无 '=' 按 unsetenv(name) 处理。

### 时间

默认时区 UTC；支持 POSIX `TZ` 字符串（固定偏移，以及显式 DST 名称/偏移/规则）。解析 `std offset [dst [offset],start[/time],end[/time]]`，名称支持字母或 `<...>`，规则支持 Jn/n/Mm.w.d；带 DST 名但没有规则时采用固定默认 `M3.2.0/2,M11.1.0/2` 并在文档公开。未设置/空 TZ 为 UTC；非法 TZ 回退 UTC，不能访问越界或保留上次状态。不引入 IANA zoneinfo 文件、locale 数据库或闰秒表。

`tzset` 更新缓存；localtime/mktime 检查当前 TZ 值，直接修改 putenv 借用字符串后也不能一直使用旧缓存。gmtime 不依赖 TZ；负时间、闰年、跨世纪、字段归一化和 time_t 溢出均测试。mktime 成功也可返回 (time_t)-1，调用者以预清 errno 后的 EOVERFLOW 区分失败；南半球跨年 DST 同样支持。mktime 尊重 tm_isdst，春季缺失小时向前归一化；秋季重复小时在 tm_isdst<0 时固定选标准时。

`strftime` 支持 C locale 的标准转换（a/A/b/B/c/C/d/D/e/F/g/G/h/H/I/j/m/M/n/p/r/R/S/t/T/u/U/V/w/W/x/X/y/Y/z/Z/%%），E/O 修饰按 C locale 基础形式处理；未知转换返回 0。输出不足返回 0；成功长度不含 NUL。asctime/ctime 系列使用规定的 26 字节布局，无法表示时失败。新增 difftime/timegm 作为共享转换能力。

`clock()` 当前依赖的 SYS_times 也是内核空壳，不以墙钟冒充进程 CPU 时间；此次将其改为明确不可用 `(clock_t)-1` 并记录限制，不扩展调度器记账项目。它不属于上轮答复承诺补齐的接口组。

### stdio

公开 `FILE` 仍是不透明类型，定义移入私有头；移除公开 mini_file_t。stdin/stdout/stderr 宏值先保留现有 sentinel ABI，通过统一 resolver 映射到三个真实内部状态对象；普通流保持注册表校验。现有 32 普通流上限不在本次扩容。

流记录 fd、读写能力、EOF/error、一个 ungetc 字节和最近 I/O 方向。普通/`_unlocked` 接口共享状态；单线程模型不新增伪锁。此轮保持无缓冲，setvbuf 不属于范围。EOF 粘滞；读取到 EOF 才置 EOF，I/O 错误置 error，ungetc 成功清 EOF，clearerr 清两标记。

fopen/fdopen/freopen 本轮支持 r/w/a 与 '+'、'b' 的合法组合；e/x 扩展及非法组合明确报 EINVAL，不承诺当前内核缺少的 CLOEXEC/O_EXCL。fdopen 不截断原 fd 并校验访问模式；a 设置 O_APPEND，fdopen(a) 通过 F_SETFL 保留其它状态并增加 O_APPEND。fcntl 的 F_GETFL/F_GETFD 不读取缺省第三参数；F_SETFL/F_SETFD/F_DUPFD/F_DUPFD_CLOEXEC 读取 int 参数，负 syscall 结果映射 -1/errno。fclose 关闭标准流对应 fd、更新状态，传播 close 错误；普通流无论 close 成败都失效并释放。freopen 本计划要求 path 非 NULL；NULL 路径扩展报 EINVAL，不冒充已实现。打开替代流失败时旧流关闭并失效，遵循标准重开语义。

**真实 append 内核依赖：**现有 fd_write 不消费 O_APPEND，不能在 libc 用 lseek+write 模拟原子追加。新增 VFS 私有 append 回调，在 tmpfs/ext2/FAT 各自既有 mount mutex 内完成“读取真实 EOF → 写入 → 更新元数据”，与普通 write/truncate 使用同一串行化边界，跨独立 fd/独立 node wrapper 仍生效。append 回调接收 struct file *（open-file-description），在同一 FS mount mutex 内完成 EOF 选择、写入和 f->offset 更新到提交末尾；再释放锁并返回实际提交字节。fd_write 返回后不再写回 offset，防止 dup/fork 共享 file_t 的迟返回者覆盖较新的位置。单次零提交错误不修改 offset，部分提交只计真实字节。没有 append 回调的只读 FS 返回 EROFS；其它不支持的普通文件返回 EOPNOTSUPP（补 errno 常量并与内核一致）；字符设备/pipe/socket 仍走原写路径，不把 O_APPEND 当可 seek。

一次 append syscall 最多先暂存 UACCESS_BOUNCE_SIZE 字节，再以单次 append 回调提交；超过该量合法返回短写，由 libc 继续写。用户内存访问和分配在 mount mutex 外完成，回调内只访问 kernel bounce；锁顺序为 FS mount mutex → active_lock，不在 active_lock/自旋锁内获取 mutex。禁止调用公开 write/truncate 回调导致重入同一 mutex，须复用内部 locked helper。单次 syscall 返回的成功字节区间连续、与其它追加不重叠；不承诺多 syscall 的整个 fwrite 不交错。

fread/fwrite 检查 size==0、nmemb==0、乘法溢出；完整项计数、短读写、EINTR、EOF/error 均正确。字符/字符串输出必须传播写失败。fseek/ftell 与 off_t 版本共用 lseek；成功 seek 清 EOF/回退状态；rewind 还清 error。ftell 在有回退字节时返回逻辑位置；fseek(SEEK_CUR) 同样先扣除回退字节，lseek 包装本身须将负 syscall 映射为 -1/errno。fflush(NULL) 遍历流；无输出缓冲时输出 flush 为 no-op，有回退输入的 seekable 流按逻辑位置协调，非 seekable 流不丢回退字节。

getline/getdelim 由 libc 提供，支持嵌入 NUL、动态扩容、最后一行无换行、ENOMEM、SSIZE_MAX。asprintf/vasprintf、dprintf/vdprintf 共用现有 vformatter/write_all；明确 va_copy 和计数边界，不重写 printf 内核。

### 数值与 scanf

整数转换统一 unsigned 累积核，支持 base=0 或 2..36；非法 base 报 EINVAL；无转换时 endptr 指回输入，不强制改变 errno；溢出饱和并继续消耗全部合法数字。无后续十六进制数字的 `0x` 不错误吞掉 x；无符号负号按模算术处理。新增 atoll、strtoimax/strtoumax；现有 strtoll/strtoull 不再靠隐含 long 等宽假设。

strtod/strtof/strtold 使用共享成熟转换算法，支持十进制/十六进制、指数、inf/nan、负零、endptr、正确舍入和 ERANGE。保持目标 long double ABI；不能简单把 strtod 强转成 strtold。补全 float.h 的 FLT/DBL/LDBL 编译器对应常量；主机和 QEMU 检查 x87/long double 状态可用性，不为该任务静默修改目标 ABI。

sscanf/vsscanf 支持 d/i/o/u/x/X、a/A/e/E/f/F/g/G、s/c/[、p/n/%%，宽度、赋值抑制、hh/h/l/ll/j/z/t/L 和输入失败/匹配失败区分。字符串宽度限制写入，%c 不跳空白，%n 不计入返回数量，长度修饰决定目标类型。fscanf/vfscanf/scanf/vscanf 暂不在此次范围，不能宣称整个 scanf 家族完成。

### 路径、目录、匹配和进程

realpath 支持绝对/相对路径、`.`/`..`、逐组件符号链接、最多 40 次链接展开、PATH_MAX/ENOMEM/ENOENT/ENOTDIR。公开 PATH_MAX=256 与现有 VFS_NAME_MAX 一致（含 NUL），SSIZE_MAX=(__PTRDIFF_MAX__) 与 ssize_t 一致；本轮不扩大内核路径容量。resolved==NULL 分配结果；失败不交付半截路径。不是纯字符串规范化替代实现。

目录补 dirfd/fdopendir/scandir/alphasort；opendir 校验目录，readdir EOF 不改 errno、失败正确置 errno。fdopendir 成功接管 fd，失败不关闭；scandir 拷贝条目、过滤、排序、失败全部回收。alphasort 使用 C locale strcmp，不引入未实现的 locale 子系统。


regex 提供真实 regex_t/regmatch_t、regcomp/regexec/regerror/regfree，支持 C locale POSIX BRE/ERE、捕获、BRE 反向引用、REG_EXTENDED/ICASE/NOSUB/NEWLINE 与 NOTBOL/NOTEOL。不提供 PCRE、GNU re_* 或 Unicode locale。正则和浮点解析拟从 musl 1.2.5 的相应算法局部移植，源版本为可复现基线而非“最新版本”声明；实施前审查后续相关修复、记录适用补丁、许可证与实际文件哈希。构建必须离线可重现，不链接宿主 libc，不引入整个 musl。

execl/execlp/execle 收集 NULL 终止参数，execle 在终止 NULL 后读取 envp；共享 execve/execvp。execvp 支持空 PATH 段表示当前目录、保留 EACCES 优先级、ENOEXEC 时交给 /bin/sh；分配失败立即报 ENOMEM。

## 验证原则与交付

每任务以生产对象行为测试先 RED 后 GREEN；每个新增符号必须有公开头编译、真实 archive 链接和行为验证，三者缺一不可。BusyBox 的 HAVE_GETLINE/fallback 和 unlocked 宏在私有构建目录处理，不能产生重复符号。

目标行为测试固定扩展 user/systest.c 现有用例表，按模块加入 required=1 的 libc 用例并由现有 systest protocol v1 验收；shutdown 则由 user/nettest.c + 受控宿主服务的 network suite 验收；桌面走既有 gui-desktop suite，不另起可选探针或依赖外网。

核心负例：分配失败保持旧资源、流状态在标准流上同样有效、宽度边界不越界、time_t 边界不回绕、无匹配不伪成功。最终验收覆盖 host、static、x86 syscall/网络/桌面及 aarch64 libk 构建；记录基线已有失败和第三方来源。

实施步骤见 [完整计划](../plans/2026-11-08-libc-completion.md)。上游参考：[musl regex 行为说明](https://wiki.musl-libc.org/functional-differences-from-glibc.html)、[1.2.5 发布公告](https://www.openwall.com/lists/musl/2024/03/01/2)。这些链接只支持所选算法来源，未用来声称移植已经完成。

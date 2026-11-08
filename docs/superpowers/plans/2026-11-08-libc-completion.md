# libc Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 覆盖本次扫描答复中除 glob 外的 libc 补全和语义修复，消除已声明无定义、假成功和调用方绕过代码，并提供真实生产对象与 QEMU 证据。

**Architecture:** 保留 OS01 libc、syscall ABI 和双架构 libk 构建边界。先建立分配、环境、FILE 和数值转换公共能力，再增加薄包装和目录/匹配算法；复杂浮点和正则算法局部移植，逐组验收。

**Tech Stack:** freestanding C、Clang/LLVM、现有 libc.a/libk.a、C hosttests、Python harness、QEMU/UEFI、现有 make profile；局部移植 musl 算法。

**Spec:** [libc 补全与语义修复设计草案](../specs/2026-11-08-libc-completion-design.md)。修订 R3：第二轮独立评审后；状态：待审阅，所有 checkbox 都是未来实施步骤，本轮不执行。技术默认值为作者建议，不声称已获用户批准。

## Global Constraints

- syscall 编号、boot_context、内核/用户共享结构不变；允许为真实 append 增加内核私有 VFS 回调并全量重编消费者；不新增线程、TLS 或动态链接模型。
- x86_64 用户态单线程；aarch64 当前只构建 libk 子集，不承诺此次建立 aarch64 用户态 libc。
- libc 公开头留在 `libc/include/`，私有头放 `libc/include/<模块>/`；新增内核源/公开头必须对称。
- 新接口使用 C/POSIX 真实签名，声明在 include guard 和 C++ linkage 内；不以无条件成功掩盖未实现能力。
- host 测试显式链接生产对象，替换宿主冲突符号；oracle 单独进程运行，不允许意外落到宿主 libc。
- 所有产物只写 profile build 目录；只经仓库根 make 入口；不得改写第三方子模块来适配 BusyBox。
- syscall E2E 使用 `make OS01_SYSTEST=1 test-syscall`，不混入 `KERNEL_SELFTEST=1`；如涉及新共享布局另跑 `test-kernel-layout`。
- 不覆盖已有工作区改动；计划不包含任何自动 merge、push、部署或第三方消息动作。

## Review Focus

1. ENOMEM/溢出不能破坏原 realloc 块、环境表或 scandir 结果；T2/T3/T14/T15 注入每个分配失败点。
2. stdin/stdout/stderr 的 EOF/error/ungetc/关闭行为须与普通流一致；T5/T7 测 sentinel 和真实 fd 0/1/2 注册流。
3. 64 位范围、负号、无转换和格式宽度不得截断或越界；T10/T11/T12 测 endptr、guard bytes 和真实指针目标类型。
4. 时区缓存失效、负时间、DST 重复/缺失小时不得给出不稳定结果；T13 在同一进程切换 TZ 并覆盖这些边界。
5. 编译或 host oracle 通过不能替代目标运行；T1/T17 审计 archive 符号、BusyBox fallback、真实 QEMU 执行和 ARM libk。

## 范围覆盖与依赖

按用户最新要求，glob/globfree 及仅为其增加的 fnmatch 改造暂不覆盖，不进入本轮 API 清单、测试选择器或完成条件。

| 上轮建议 | 任务 | 完成条件 |
|---|---|---|
| calloc 溢出、malloc 元数据边界、reallocarray、posix_memalign、aligned_alloc | T2 | 旧块不丢、对齐结果可 free/realloc、libk 无退化 |
| setenv/unsetenv/putenv/getenv/clearenv | T3 | 单一环境表、继承/ownership/失败原子性 |
| shutdown | T4 | 真实 SYS_shutdown、errno、TCP 半关闭 |
| O_APPEND 真实追加 | T6 | 跨独立 fd/节点的 EOF 选择与写入原子提交 |
| FILE 状态、feof/ferror/clearerr/ungetc、fopen 模式、fread/fwrite/fclose | T5/T7 | 完整普通流与标准流语义 |
| fgets/fgetc/getc/putc/fileno、getline/getdelim | T8 | 标准接口 + 动态行读取，BusyBox 无符号冲突 |
| dprintf/vdprintf、asprintf、fseek/fseeko/ftell/ftello/rewind/freopen | T9 | 复用已有核心，输出和重定位错误可见 |
| strtol/strtoul/strtoll/strtoull、atoll、strtoimax/strtoumax | T10 | 范围/负号/endptr/base 正确 |
| strtod/strtof/strtold | T11 | 各目标精度、舍入/特殊值/范围错误正确 |
| sscanf/vsscanf | T12 | 宽度、长度、转换和 EOF/计数 |
| gmtime/localtime 及 _r、mktime、strftime、ctime/asctime 及 _r | T13 | UTC/POSIX TZ、C locale 格式、桌面调用正常 |
| realpath | T14 | 逐组件解析，符号链接和错误路径 |
| scandir/alphasort/dirfd/fdopendir | T15 | fd 所有权、条目复制、排序、清理 |
| regcomp/regexec/regerror/regfree | T16 | C locale BRE/ERE，禁止永远匹配成功 |
| execl/execlp/execle、execvp 空 PATH 段等 | T14 | 参数/环境传递和搜索错误正确 |
| 头文件、构建、BusyBox、host/QEMU/ARM 回归、文档 | T1/T17 及各任务 | 符号、行为、消费者三层验收 |

执行顺序：T1 → T2 → T3；T4 可紧接 T1；T5 → T6 → T7 → T8/T9；T10 → T11 → T12；T13 依赖 T3；T14 依赖 T2/T3；T15 依赖 T2；T16 依赖 T2/T11（分配与第三方来源清单）；最后 T17。这是依赖说明，不授权自动派生并行 agent。每个任务是一个可单独审阅的交付，T2/T13/T16 可在任务内拆提交但不能跳过验收。

## 文件与构建责任

- 公共声明：修改 `libc/include/{stdio,stdlib,string,unistd,time,inttypes,float,malloc,dirent,regex,limits,errno}.h`；只改本次接口，保留 ABI 需要的类型。
- 内部公共状态：新增 `libc/include/stdio/file.h`、`libc/include/stdlib/strtox.h`、`libc/include/time/calendar.h`；现有 `libc/stdio/stdio_internal.h` 迁至 `libc/include/stdio/internal.h`，同步引用，不保留两份真相。
- 各模块新增/修改文件在对应任务列出。新目录 `libc/regex/` 需在 `libc/Makefile` x86 C_SOURCES 中显式收集，不把 regex/浮点用户态依赖带入 ARM libk。
- 测试统一注册于 `hosttests/mk/libc.mk`，生产对象编译规则在 `hosttests/mk/common.mk` 或模块本地规则；不要给所有旧 hosttests 强行链接新分配器。
- 根 focused 入口新增 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=<case>`，于 `mk/components/run.mk` dispatch 到 hosttests；默认 all，非法 case 非零失败。case 固定为 `api,alloc,env,shutdown,stream,append,io,line,format,integer,float,scan,time,path,dir,regex,all`。
- `api` 随阶段只审计已交付组；最终 all 必须审计覆盖表全部符号。新增 host binary 均注册 libc group 并进入 TEST_BINS；新命令不是既有 bucket 的别名。

## 统一任务验证约定

每任务测试按以下步骤运行，下面不会把未存在的入口冒充已有命令：T1 负责创建上述根入口和 case 路由；后续任务在实现时填入自己的 case。目标 `test_libc_<模块>.elf` 和其对象的所有路径均由 HOST_TEST_BUILD_DIR 派生。

每个测试组必须：先写测试并运行得到目标行为 RED（缺符号编译/链接 RED 可另记，但不能代替已有空壳的行为 RED）；再实现；再运行相同命令得到 GREEN；最后只 stage 本任务实际改动文件并提交指定消息。不得 `git add .`。后续任务未完成时不要求全量 api 检查不存在的接口。

### T1：固定基线、公开接口和生产对象测试入口

**Files:** 修改 `hosttests/mk/libc.mk`、`hosttests/mk/common.mk`、`mk/components/run.mk`；新增 `hosttests/cases/test_libc_api.c`、`hosttests/mock/libc_completion_runtime.h`、`scripts/audit_libc_api.py`；修改本阶段受影响公开头及 `docs/build/build.md`。

**Interfaces:** 产生上述 focused root 入口；API 清单按组保存于 audit 脚本，按已交付组启用；用户 libc 检查全量用户接口，libk 只审计既有已支持子集及明确共享的 reallocarray，不把用户对齐分配/regex/浮点扫描接口列为 libk 必需。生产测试符号统一 `os01_<函数>`，测试 TU 同步重命名，syscall/read/write/allocator 仅替换边界。oracle 二进制独立使用宿主头，不把不同 FILE/regex_t 类型混传。

- [ ] 记录 HEAD、工作区 diff、现有 host libc 测试/phase-0/网络结果；收集 libc.a/libk.a 的现有导出、BusyBox fallback 和当前消费者；失败分类为基线或新增，不隐去。
- [ ] 写 `api_header_self_contained`、`api_two_translation_units`、`api_cpp_linkage` 和 focused selector 正/负例；本阶段不将 regex.h 纳入上述通用测试；另用现有 regex.h 单 TU probe 验证非匹配输入当前仍返回 0，记录行为 RED，T16 在迁移时重跑。C++ probe 只验证声明和 extern C，可用 clang++ 编译并由目标工具链接。
- [ ] 建立 focused 路由、组内 binary 注册和 source-prefix 编译约定；修复本阶段目标头的 guard/重复声明；regex.h 的类型/头内实现暂不修改，独立双 TU 和 C++ 验收在 T16 一并启用；fseeko 的 off_t、clearerr 的 void 签名等修正在拥有行为的任务中完成。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=api`，确认测试实际链接 os01 符号；`nm` 检查测试未调用宿主同名实现。根非法 LIBC_CASE 返回非零。
- [ ] 提交 `test(libc): establish completion API and production-object harness`。

### T2：分配溢出检查和对齐分配

**Files:** 修改 `libc/stdlib/{malloc,calloc}.c`、`libc/include/{stdlib,malloc}.h`；新增 `libc/stdlib/reallocarray.c`、`hosttests/cases/test_libc_alloc.c`、`hosttests/mock/libc_heap_runtime.h`。

**Interfaces:** `void *reallocarray(void *, size_t, size_t)`；`int posix_memalign(void **, size_t, size_t)`；`void *aligned_alloc(size_t, size_t)`。对齐实现留在 malloc.c 的 !__is_libk 分支内访问现有 block_header_t；posix_memalign/aligned_alloc 仅用户 libc 定义，公开头在 __is_libk 模式不声明它们；reallocarray 不访问私有布局，可共享给 libk。

- [ ] 写 `calloc_product_wrap`：`calloc(SIZE_MAX/2+1,2)==NULL && errno==ENOMEM`；`malloc_roundup_wrap`：SIZE_MAX 请求失败；`reallocarray_failure_preserves_bytes`：失败后原 32 字节 0xA5 保持、可 free。
- [ ] 写 `aligned_block_lifecycle`：16/64/4096 对齐、分割前后块、不同 free 顺序、realloc 内容保持；`posix_memalign_bad_alignment`：3 和 sizeof(void*)/2 返回 EINVAL 且输出/errno 不变；`aligned_alloc_bad_multiple` 报 EINVAL。运行 alloc case 保存 RED。
- [ ] 加所有取整/元数据加法和乘法检查；在同一个块管理协议下实现对齐分割，验证最小前后块、合并和 brk 失败。不得更改普通块释放 ABI 而遗漏 realloc/libk 分支。libk 的 calloc/reallocarray 在溢出时返回 NULL，不引用 errno；保留 kmalloc/kfree/ksize 路径，不添加对齐 stub。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=alloc`；用目标用户程序重复分配/对齐/释放，另构建双架构 libk 验证普通 malloc/calloc/free/realloc 路径与溢出检查；nm 确认 libk 不新增 aligned_alloc/posix_memalign 定义或 errno 未定义引用。保留 ENOMEM 故障注入结果。
- [ ] 提交 `fix(libc): make allocation overflow-safe and add aligned allocation`。

### T3：统一环境变量表与所有权

**Files:** 修改 `libc/stdlib/environ.c`、`libc/include/stdlib.h`；新增 `hosttests/cases/test_libc_environ.c`。

**Interfaces:** 保留 `extern char **environ`；标准 getenv/setenv/unsetenv/putenv/clearenv；内部维护指针表和 ownership，无新增用户 ABI。

- [ ] 写 `setenv_visible`、`overwrite_and_remove`：设置/覆盖/删除可立即由 getenv 观察；`preserve_inherited`：初始 `A=1,B=2` 经 putenv 后 B 仍存在；`putenv_borrowed`：修改调用者字符串内容后 getenv 变化。
- [ ] 写 invalid-name、重复继承 key 的全部删除、40 项与 1024 字节值、clearenv 后再设置、逐分配失败保持 environ 和旧值；运行 env case 确认空壳 RED。
- [ ] 实现事务式扩容和字符串拥有关系；overwrite=0 不分配；clearenv 不释放继承/借用内存，putenv 替换自有项时释放旧项。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=env`；在 syscall E2E 中检查 fork/exec 后继承与覆盖，clearenv 不导致 crt0 继承区非法 free。
- [ ] 提交 `fix(libc): unify environment mutation and ownership`。

### T4：接通 shutdown syscall

**Files:** 修改 `libc/unistd/net_stubs.c`；新增 `libc/unistd/shutdown.c`、`hosttests/cases/test_libc_shutdown.c`；扩展 `user/nettest.c`、`qemutests/run_test.py`、`qemutests/test_run_test_harness.py`，沿用 config/inittab.nettest。

**Interfaces:** `int shutdown(int sockfd, int how)` → 已存在 SYS_shutdown=64；无内核 ABI 变化。

- [ ] 写 `shutdown_forwards_args`、`shutdown_errno`：验证 fd/how/调用次数，返回 0；内核 -EBADF 映射 -1/EBADF；运行 shutdown case 得到现有 ENOSYS RED。
- [ ] 移除旧定义、新文件实现真实 syscall；libk 路径明确失败，不执行用户 syscall。保留其它 net_stubs 接口，避免附带扩大范围。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=shutdown`；运行 `make PROFILE=x86_64-clang test-qemu SUITE=network`；NetworkServices 新增受控 10003 TCP 服务，读到 EOF 后才返回固定半关闭确认字节；user/nettest.c 增加 shutdown case，验证 SHUT_WR 后仍可收响应，设置服务端读取期限，不能用公网 socktest 代替。runner 显式要求本次启动的 shutdown PASS 和汇总 failed=0；缺 case、假旧日志、服务端超时均失败，并扩展对应 harness 负例。非法 how 验证内核现状：若内核接受非法值，在包装层先 EINVAL 拒绝，单测固定该契约。
- [ ] 提交 `fix(libc): connect shutdown to the existing kernel syscall`。

### T5：FILE 状态基础与标准流生命周期

**Files:** 修改 `libc/include/stdio.h`、`libc/stdio/{stdio_file,stdio_extras,printf,putchar,puts,getchar_unlocked}.c`；新增 `libc/include/stdio/file.h`；迁移私有 formatter 头到 `libc/include/stdio/internal.h`；新增 `hosttests/cases/test_libc_stream_state.c`；更新现有 stdio host fixtures。

**Interfaces:** `struct __FILE` 私有化；内部 `FILE *__stdio_resolve(FILE *)` 和 `int __stdio_fd(FILE *)`，resolver 识别 sentinel、注册表和关闭状态；替换旧 file_to_fd 所有调用。`int feof(FILE *)`、`int ferror(FILE *)`、`void clearerr(FILE *)`、unlocked 对应接口、`int ungetc(int, FILE *)`。

- [ ] 写 `eof_is_sticky`、`read_error_is_not_eof`、`ungetc_one_byte`、`clearerr_both_flags`；每项在普通流/标准流各测，EOF 不可作为有效回退字符，成功 ungetc 清 EOF；保存 stream case RED。
- [ ] 增加 `closed_standard_stream`、注册流底层 fd=0/1/2、无效/已关闭句柄、不超过/超过 32 普通流、槽位复用的测试；把旧 sentinel fclose no-op 预期改为真实 close。
- [ ] 集中状态和 resolver，普通/标准流均使用真实状态对象；迁移所有直接 syscall 字符路径，确保 error/errno 可观察；更新头签名，不向调用方公开结构。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=stream` 及现有 stdio registry/fflush/fread-fwrite tests；检查全部生产 FILE 消费者不再使用 mini_file_t/旧 resolver。
- [ ] 提交 `refactor(libc): centralize stream state and standard stream lifecycle`。

### T6：内核真实原子 append 支撑

**Files:** 修改 `kernel/fs/{file,vfs,tmpfs,ext2,fat}.c`、`kernel/include/fs/vfs.h`；新增 `hosttests/cases/test_vfs_append.c`、`hosttests/mock/vfs_append_runtime.h`；修改 `hosttests/mk/fs.mk` 和 focused libc append dispatch；在 `user/systest.c` 增加 required append case。

**Interfaces:** 新增私有 `int (*append)(struct file *, uint64_t size, const void *buffer)` 与 `int vfs_append(struct file *,uint64_t,const void *)`。file->node 标识底层对象；回调在同一 FS mount mutex 内选择 EOF、提交字节并更新对应 file->offset，再释放锁返回提交字节；零提交错误不改 offset。共享 syscall ABI 不变；复用并审计既有 EOPNOTSUPP=95，不重复定义。

- [ ] 写两个独立 open/node wrapper 对同一底层文件追加的生产 FS 测试：初始内容保留、每次成功区间不重叠、每个 syscall 的 token 连续；覆盖 tmpfs/ext2/FAT，普通 write/truncate 与 append 使用同一 mount mutex。新增 dup/fork 共享同一 file_t 的交错：A 完成锁内提交/offset更新后在返回点暂停，B 提交并返回，再让 A 返回，最终 f->offset 必须等于最后提交末尾，不能回退；测试断言内容、提交区间和共享位置三者一致。运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=append`，现有 fd_write 覆盖文件开头必须 RED。
- [ ] 测 size 大于 UACCESS_BOUNCE_SIZE 返回合法短写、坏用户 buffer 首段 EFAULT 不提交、ENOMEM 不提交、FS 部分写更新准确 offset；不带 append 的行为不变，设备/pipe/socket 不被错误送入 VFS append。
- [ ] 在各 FS 内提取已持锁 write helper，append 接收 file_t，在既有 mount mutex 内读取真实 EOF、调用 helper 并按实际成功字节更新 file->offset 后才解锁；不在 fd_write 返回路径再次赋值 offset，遵循 mount mutex → active_lock；append 不通过会重复加锁的公开 write 入口。fd_write 在锁外分配/拷贝最多一个 bounce，调用一次 append，然后返回；不在文件锁下接触用户内存。只读 FS EROFS、无回调的其它普通文件 EOPNOTSUPP，保留每个错误的 errno 数值。
- [ ] 运行 append host case 和现有 FS/锁顺序回归；`make OS01_SYSTEST=1 test-syscall` 在 2/8 CPU（`SMP=2`、`SMP=8`）执行多进程独立 open、以及 fork/dup 共享 fd + 固定长度记录追加，核验记录数量/校验和/无覆盖，以及等待所有子进程后共享 fd 的 SEEK_CUR 位置与实际 EOF 一致，分别走 tmpfs/ext2/FAT 可写测试目录。测试直接用 open/write，避免尚未完成的 fopen。
- [ ] 所有 VFS ops 消费者全量重编、designated initializer 检查，运行 `make test-kernel-layout`；提交 `fix(fs): implement atomic append under filesystem serialization`。

### T7：打开模式、完整项读写和关闭错误

**Files:** 修改 `libc/stdio/{stdio_file,stdio_extras}.c`、`libc/include/stdio/file.h`、`libc/unistd/fcntl.c`、`libc/include/fcntl.h`；新增 `hosttests/cases/test_libc_stdio_io.c`；扩展既有 `test_libc_fread_fwrite_validate.c`。

**Interfaces:** 标准签名 fopen/fdopen/fclose/fread/fwrite；私有 `int __stdio_parse_mode(const char *, int *open_flags, unsigned *access)` 供 T9 freopen 复用。

- [ ] 测 r/w/a/r+/w+/a+、b 的合法组合；a 不截断、w 截断；e/x、空/重复/非法 mode 为 EINVAL，不承诺 CLOEXEC/O_EXCL；fdopen 不截断/失败不关闭调用方 fd。运行 io case 得到 RED。
- [ ] 测 `fread(p,0,10,f)==0` 不 syscall，乘法溢出失败、1 字节分批读取/写入、EINTR 重试、部分项 EOF、写失败设 error、close 失败返回 EOF 且释放普通流。
- [ ] 先修真实 fcntl 包装：F_GETFL/F_GETFD 不 va_arg，F_SETFL/F_SETFD/F_DUPFD/F_DUPFD_CLOEXEC 用 va_arg(int)，不支持的 cmd 报 EINVAL，负结果映射 -1/errno；公开头声明 int fcntl(int,int,...)。直接测两参数调用和 -EBADF 映射，再测流，不能绕过 wrapper。实现共享 mode parser 和 access 校验，使用 O_APPEND/O_RDWR；fdopen(a) 的 F_SETFL 保留已有标志，依赖 T6 真实 append。读写循环保持完整项返回数及部分字节副作用，成功字节不丢失。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=io`；目标临时文件验证 append/update 与 EINTR 情况，并回归现有 fflush/registry 测试。
- [ ] 提交 `fix(libc): implement stream modes and reliable item IO`。

### T8：标准字符接口与动态行读取

**Files:** 修改 `libc/stdio/stdio_extras.c`、`libc/include/{stdio,limits}.h`；新增 `libc/stdio/getdelim.c`、`hosttests/cases/test_libc_line.c`；修改 `mk/components/user.mk` 中 BusyBox 私有构建配置生成逻辑。

**Interfaces:** `int fgetc(FILE *)/getc(FILE *)/putc(int,FILE *)/fileno(FILE *)`；`char *fgets(char *,int,FILE *)`；`ssize_t getdelim(char **,size_t *,int,FILE *)`、getline；unlocked 版本与 T5 共享状态。

- [ ] 写行含换行、空行、最后一行无换行、初始 EOF、嵌入 NUL、分隔符不是换行、已有 buffer 扩容、n=1 fgets 不消费输入；验证返回长度和终止 NUL、guard bytes。运行 line case 得 RED。
- [ ] 注入扩容 ENOMEM、读错误/EOF 区分、长度加法溢出、超过 SSIZE_MAX 的内部边界；失败后调用方持有的 buffer 仍可释放并保留一致的 capacity。
- [ ] 公开 SSIZE_MAX=(__PTRDIFF_MAX__) 并静态断言与 sizeof(ssize_t) 一致；实现 wrappers/getdelim 核心，getline 只选择 '\n'；BusyBox 私有构建定义 HAVE_GETLINE=1，使 fallback 不再参与，使用实际编译命令证明宏生效，源子模块保持不变。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=line`；重建 BusyBox 验证没有 getline 重定义，运行 head/tail/cat，图形测试用 fgets 替换原单次 read 绕过代码。
- [ ] 提交 `feat(libc): add standard character and delimited line IO`。

### T9：格式化输出包装和定位/重开

**Files:** 新增 `libc/stdio/{dprintf,asprintf,stdio_seek}.c`、`hosttests/cases/test_libc_stdio_format.c`；修改 `libc/stdio/{stdio_extras,stdio_file}.c`、公开/私有 stdio 头、`libc/unistd/lseek.c`、`libc/include/sys/stat.h`。

**Interfaces:** `int vdprintf(int,const char *,va_list)`、dprintf、`int asprintf(char **,const char *,...)`；`int fseek(FILE *,long,int)`、`int fseeko(FILE *,off_t,int)`、`long ftell(FILE *)`、`off_t ftello(FILE *)`、`void rewind(FILE *)`、`FILE *freopen(const char *,const char *,FILE *)`。

- [ ] 写 `dprintf_real_fd`、两遍 va_list、输出截断计数/ENOMEM/INT_MAX、short write/EINTR；失败设置 *strp=NULL，复用现有 vasprintf 测试。运行 format case 显示 dprintf 空壳 RED。
- [ ] 写直接 lseek 错误返回 -1 且 errno=ESPIPE/EINVAL、成功 offset 不变；写 seek 后清 EOF/ungetc、ftello 逻辑位置、ungetc 后 fseek(0,SEEK_CUR) 回到逻辑位置、rewind 清 error、pipe ESPIPE、append reopen、标准流 reopen、打开新路径失败旧流失效、NULL path EINVAL。
- [ ] 先修 lseek 的负 errno 映射并直接测试真实 wrapper；复用 vformatter/vasprintf/write_all 和 T7 mode parser；删除重复 dprintf 空壳；seek/reopen 通过 T5 resolver 保持注册表与状态一致，传播 lseek/close/open 错误。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=format` 和现有 printf/fflush 测试；api 组启用全部已交付 stdio 接口，目标文件读写后定位校验内容。
- [ ] 提交 `feat(libc): complete formatted fd IO and stream positioning`。

### T10：整数转换核与 intmax 接口

**Files:** 修改 `libc/stdlib/{strtol,strtoll}.c`、`libc/include/{stdlib,inttypes}.h`；新增 `libc/include/stdlib/strtox.h`、`hosttests/cases/test_libc_integer.c`。

**Interfaces:** strtol/strtoul/strtoll/strtoull/atoi/atol/atoll；`intmax_t strtoimax(const char *,char **,int)`、`uintmax_t strtoumax(const char *,char **,int)`；私有整数核以 uintmax_t 和目标上下界参数处理，endptr/errno 统一。

- [ ] 测 LONG_MIN/MAX、LLONG_MIN/MAX、UINTMAX_MAX、每个边界再多一位；`strtoul("-1",&e,10)==ULONG_MAX`；非法 base=1/37 不除零；空串/纯空白/纯正负号 endptr 为原始输入。运行 integer case 得 RED。
- [ ] 测 `0x`、`0xg`、`08`、base36、溢出后仍扫描到尾部字母、errno 在成功/无转换时保持；对保存的原指针断言精确消费长度，不能只测返回值。
- [ ] 使用 unsigned 算术避免 LONG_MIN 取负 UB；统一符号/进制/溢出逻辑，新增 wrapper 和 inttypes 声明，既有 PRI 宏类型匹配另作编译检查。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=integer`，与独立宿主 oracle 对确定语义的表格比较；目标 syscall 测试读取 uint64 token 不截断。
- [ ] 提交 `fix(libc): complete integer conversion boundaries and intmax APIs`。

### T11：浮点解析、精度与目标 ABI

**Files:** 修改 `libc/stdlib/strtod.c`、`libc/include/{stdlib,float}.h`、`libc/Makefile`；新增 `libc/stdlib/floatscan.c`、`libc/include/stdlib/floatscan.h`、`hosttests/cases/test_libc_float_parse.c`、`thirdpart/musl-libc-algorithms.manifest`、`thirdpart/musl-libc-algorithms/COPYRIGHT`。

**Interfaces:** `double strtod(const char *,char **)`、`float strtof(const char *,char **)`、`long double strtold(const char *,char **)`、atof；共享内部扫描器参数指定目标精度，不能经 double 中转损失 long double 精度。

- [ ] 测 `"1e+"` 只消费 1、无数字输入 endptr 不移动；十六进制 `0x1.8p+1==3`、INF/NAN、负零、FLT/DBL/LDBL 最小/最大/次正规、上溢 ERANGE、精确舍入临界值。运行 float case 保存 RED。
- [ ] 固定 musl 1.2.5 源档来源和 SHA256，审查该模块后续相关补丁并在 manifest 写清包含/不适用原因；保留逐文件许可证和局部适配 diff。移植 floatscan 必须枚举并替换其私有 stdio/shgetc/math/locale 依赖，不能导入整个 FILE 实现或遗漏编译单元。
- [ ] 实现内存字符串输入 adapter、精度参数和 errno/endptr；float.h 使用编译器内建常量补全 FLT/DBL/LDBL，不修改 long double ABI。Makefile 防止新增浮点源进入 ARM -mgeneral-regs-only 子集。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=float`；独立 oracle 比较 bit pattern/消费长度；QEMU 验证 strtold 比 double 多出的有效位能保存、系统调用/调度后不损坏；若目标浮点状态支持不足，报告该项未完成，不能降精度冒充 GREEN。
- [ ] 提交 `feat(libc): implement precision-aware floating point parsing`。

### T12：完整内存 scanf 核

**Files:** 修改 `libc/stdio/sscanf.c`、`libc/include/stdio.h`；新增 `libc/stdio/vsscanf.c`、`hosttests/cases/test_libc_scan.c`；修改 `user/systest.c` 的 proc maps 解析。

**Interfaces:** `int vsscanf(const char *,const char *,va_list)`；sscanf 只做 va_start/va_end；消费 T10/T11 转换能力但字段宽度必须限制其扫描输入。

- [ ] 测 `%lx-%lx %4s` 解析 64 位地址且 guard 不变，`%hhd/%hd/%lld/%jd/%zu/%td/%Lf` 写入正确类型；`%n` 存已消费长度、不增加计数，抑制转换不取 va_arg。运行 scan case 得 RED。
- [ ] 测空输入 EOF 与首字符不匹配返回 0、宽度整数/浮点、%c 不跳空白、`%3c` 不补 NUL、scanset 反选/首位 ']'、literal %%、%p、格式末尾空白；覆盖格式不支持时明确终止，不能跳过继续消费错误参数。
- [ ] 建立格式解析/输入游标/typed store 三部分；width 不含终止 NUL，%s/%[ 正确留 NUL；所有 length modifier 组合验证并拒绝非法组合。恢复 systest 的 `%lx/%4s`，删除 32 位绕过代码。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=scan`；执行 `make OS01_SYSTEST=1 test-syscall` 验证目标 proc maps 解析及已有调用。
- [ ] 提交 `feat(libc): implement width-aware and typed vsscanf conversions`。

### T13：日历、POSIX TZ 和格式化

**Files:** 修改 `libc/time/{time,time_r,strptime}.c`、`libc/include/time.h`；新增 `libc/time/{calendar,tz,strftime}.c`、`libc/include/time/calendar.h`、`hosttests/cases/test_libc_time.c`；扩展桌面 clock/date 测试。

**Interfaces:** gmtime/localtime/_r、mktime、strftime、asctime/ctime/_r；新增 `void tzset(void)`、`time_t timegm(struct tm *)`、`double difftime(time_t,time_t)`；内部按 days/seconds 整数核做转换，不依赖浮点。

- [ ] 测 epoch=1970-01-01 Thu、-1=1969-12-31 23:59:59、2000 闰年/2100 非闰年、2038 后、time_t 极值返回 EOVERFLOW、mktime 字段归一化、成功返回 -1 时 errno 不设 EOVERFLOW；运行 time case 得空壳 RED。
- [ ] 测 UTC0、CST-8、EST5EDT,M3.2.0/2,M11.1.0/2，春季缺时向前和秋季重复时标准时选择、南半球跨年 DST；同进程 unset/setenv/putenv 内容修改/非法 TZ 回 UTC；Jn/n/M 规则跨年边界。tm_isdst 显式值和自动值分别验证。
- [ ] 实现 calendar 与 TZ parser/缓存，包装 non-r 函数使用静态存储但 _r 只写调用方对象；strftime 实现 spec 列出的全部转换、C locale 文本和 ISO 周年；asctime/ctime 固定布局并检查无法表示年份。
- [ ] 测全部 strftime 转换、max=0/恰好不足/恰好够、26 字节 asctime guard；补 strptime 与之往返的合法范围错误；clock 按设计明确返回不可用，不伪称已有 CPU 时间。运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=time`。
- [ ] QEMU 检查真实桌面时钟不再固定 `--:--`，内置 date 和系统启动时间可显示；固定测试时间用 host 注入，QEMU 不硬编码墙钟字符串。提交 `feat(libc): implement calendar conversion POSIX TZ and time formatting`。

### T14：realpath 与 exec 家族

**Files:** 从 `libc/stdlib/malloc.c` 移除 realpath 空壳；新增 `libc/stdlib/realpath.c`、`libc/unistd/execl.c`、`hosttests/cases/test_libc_path.c`；修改 `libc/unistd/execve.c`、`libc/include/{stdlib,unistd,limits}.h`。

**Interfaces:** `char *realpath(const char *,char *)`；execl/execlp/execle 与现有 OS01 exec 返回类型兼容，本次不顺带改变 execve ABI；私有 argv 收集函数与路径解析不共享全局 scratch buffer。

- [ ] 测相对路径/根/多重斜杠/`.`/`..`、相对/绝对 symlink、尾部斜杠要求目录、失效链接、40/41 链接、PATH_MAX 和 ENOMEM；resolved==NULL 成功可 free。运行 path case 保存 RED。
- [ ] 测 exec 参数顺序和 NULL terminator、execle 的显式 envp；PATH `:/bin:` 中空段为当前目录；EACCES 优先于全路径 ENOENT；ENOEXEC 使用 `/bin/sh`，分配失败不继续掩盖 ENOMEM。
- [ ] 公开 PATH_MAX=256（含 NUL），编译期/host audit 断言等于现有 kernel VFS_NAME_MAX；本轮不扩大内核容量。实现逐组件解析 realpath；exec variadic 包装构造 argv 后调用真实 execve/execvp，失败释放；PATH 搜索不用会跳过空段的 strtok_r。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=path`；QEMU 子进程通过 pipe 返回 argv/env 和退出码，符号链接使用真实 VFS，验证 BusyBox/terminal 启动不退化。
- [ ] 提交 `feat(libc): resolve canonical paths and complete exec wrappers`。

### T15：目录 fd、扫描和排序

**Files:** 修改 `libc/unistd/dirent.c`、`libc/include/dirent.h`；新增 `libc/stdlib/scandir.c`、`hosttests/cases/test_libc_dir.c`；桌面调用是否迁移只在等价行为可证明时进行。

**Interfaces:** `int dirfd(DIR *)`、`DIR *fdopendir(int)`；`int scandir(const char *,struct dirent ***,int (*)(const struct dirent *),int (*)(const struct dirent **,const struct dirent **))`、`int alphasort(const struct dirent **,const struct dirent **)`。

- [ ] 测 opendir 普通文件 ENOTDIR；fdopendir 成功 closedir 关闭 fd、失败保留 fd；readdir 负 syscall 置 errno，正常 EOF 保持 errno；检查内核返回 record 长度/名字范围，坏记录报 EIO 不越界。运行 dir case 得 RED。
- [ ] 测 scandir filter/null comparator/alphasort、空目录结果、返回条目是独立拷贝；每个 malloc/realloc 失败都释放已拷贝条目和 DIR，不把半成品写给调用者。
- [ ] 实现目录校验/所有权/记录验证/扫描拷贝，alphasort 使用 strcmp；d_off 从内核记录正确复制，不以自增条目数冒充 cookie。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=dir`；QEMU 在 tmpfs/ext2 上验证枚举、fd 关闭和排序；回归桌面 Files 保留旧视图的错误处理。
- [ ] 提交 `feat(libc): add directory descriptor and sorted scan APIs`。

### T16：真实 POSIX 正则实现

**Files:** 新增 `libc/regex/{regcomp,regexec,regerror,regfree,tre-mem}.c` 和实际需要的上游内部单元；新增 `libc/include/regex/{tre,compat}.h`；修改 `libc/include/regex.h`、`libc/Makefile`、T11 的第三方 manifest/许可；新增 `hosttests/cases/test_libc_regex.c`。

**Interfaces:** `int regcomp(regex_t *,const char *,int)`、`int regexec(const regex_t *,const char *,size_t,regmatch_t[],int)`、`size_t regerror(int,const regex_t *,char *,size_t)`、`void regfree(regex_t *)`。regex_t 为可栈分配、包含 re_nsub 和私有指针的真实结构；regmatch_t 含有符号宽度的 rm_so/rm_eo，-1 表示未参与捕获；结构与移植实现匹配并全量重编消费者。

- [ ] 写 ERE `^(ab|cd)+([0-9]{2})$` 的成功/失败与捕获偏移，BRE `\(ab\)\1` 的反向引用，空匹配、未参与子组 -1、REG_NOSUB 不写 pmatch。在修改 regex.h 前跑旧单 TU probe 的无条件成功行为 RED（引用 T1 基线并重跑），随后引入真实类型/声明和实现；双 TU/缺符号 RED 与旧行为 RED 分别保存。
- [ ] 测 ICASE/NEWLINE/NOTBOL/NOTEOL、所有支持 POSIX 字符类、坏括号/非法区间/坏反向引用、regerror 长度包含 NUL 且小 buffer 截断、OOM 编译失败释放；host timeout 约束恶意长模式测试不得无限执行。
- [ ] 按 T11 固定来源规则移植成熟引擎；逐个列出依赖的 wchar/wctype/locale/allocator 接口，在私有 compat 实现真实 C locale 语义，不添加公共假 wchar 接口。保留上游整数/状态数溢出和资源失败检查，返回 REG_ESPACE 等真实错误。
- [ ] 在 x86 source inventory 加 regex/*.c；无头内函数定义、无宿主符号依赖、两 TU 链接正常；运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=regex`，与独立 oracle 比较 POSIX 确定的左最长捕获用例，QEMU 直接调用 regex API。
- [ ] 提交 `feat(libc): provide real C locale POSIX regular expressions`。

### T17：端到端验收、消费者清理和文档

**Files:** 扩展 `user/systest.c`、`user/nettest.c`、现有桌面测试、`qemutests/run_test.py`、`qemutests/test_run_test_harness.py`；修改 `hosttests/mk/libc.mk`、`mk/components/{user,run}.mk`、`config/test-scenarios.json`、`docs/build/build.md`；新增 `docs/libc/libc.md`、`docs/superpowers/reports/2026-11-08-libc-completion-validation.md`。固定复用 systest，不创建独立 probe。

**Interfaces:** 最终 api 审计启用覆盖表全部组；在 user/systest.c 的 tests[] 增加 required=1、quick=0 的用例 55_libc_alloc、56_libc_env、57_libc_stdio、58_libc_integer、59_libc_float、60_libc_scan、61_libc_time、62_libc_path_exec、63_libc_dir、64_libc_regex、65_libc_append（实施时先查 ID 未占用）；全部复用现有 START/SELECT/BEGIN/PASS/FAIL/END protocol v1。每模块任务在自身 GREEN 阶段填入所属 case，本任务聚合；CASE 选择不能通过过滤遗漏必测用例。

- [ ] 完成 `api` 清单：全部声明、archive 定义恰一份、真实目标静态链接、无宿主/未满足内部符号；检查 BusyBox HAVE_GETLINE 生效及无私有源改动；所有头通过独立 C/C++ TU 编译。
- [ ] 移除已被修复的 gfx fgets、systest scanf 绕过代码；桌面时钟/date/Files 以及 BusyBox 文件/网络路径必须调用真实库能力。grep/sed 只有当前 config 启用时才作为附加消费者，不能靠临时启用 applet 代替直接 API 测试。
- [ ] 运行 `make PROFILE=x86_64-clang test-libc-host LIBC_CASE=all`、`make PROFILE=x86_64-clang test-host`、`make PROFILE=x86_64-clang test-static`；保留每任务新增 host binary 的执行而非仅编译证据。
- [ ] 运行 `make OS01_SYSTEST=1 test-syscall`、`make PROFILE=x86_64-clang test-qemu SUITE=network`、`make PROFILE=x86_64-clang test-qemu SUITE=phase-0` 和 `make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop`；网络半关闭由 network case 验收；其它模块 case 都必须出现在 systest --list、当前 SELECT 和终止结果中。可用 `make OS01_SYSTEST=1 test-qemu SUITE=systest CASE=59_libc_float` 等单例定位，但最终必须完整运行 `make OS01_SYSTEST=1 test-syscall`。不要将 systest 镜像当普通交互镜像。
- [ ] 运行 `make PROFILE=aarch64-clang aarch64-uefi` 证明 ARM libk 未引入 syscall/浮点/regex 依赖；复用现有静态布局/符号审计，若改动影响共享布局另跑 `make test-kernel-layout`。运行 `make test-harness` 验证新增/修改 runner 的负例。
- [ ] 在报告逐任务列出命令、退出码、实际摘要、源码来源和适配补丁、基线失败；文档列出支持接口、TZ/locale/32 流上限、尚未支持的 clock CPU 时间等限制。所有覆盖行有证据才宣称全部完成。
- [ ] 提交 `test(libc): validate completion across userspace and libk consumers`。不自动 merge/push；按用户后续指示处理集成。

## 计划自检

- 覆盖：上轮建议除 glob 外全部映射到覆盖表；glob 及其 fnmatch 支撑改造按用户要求暂不覆盖；附带修复只限必要依赖和错误语义。
- 类型：FILE 统一私有化、clearerr 为 void、fseeko/ftello 使用 off_t、regex 捕获偏移可表示 -1；exec 保持当前 OS01 返回类型兼容。
- 依赖：环境先于 TZ/PATH，流状态先于字符/定位接口，整数/浮点先于 scanf；无循环。
- 证据：host 重命名生产符号 + 独立 oracle + archive link + 目标行为；没有把宿主或 shell 自带实现算成 libc 完成。
- 未执行：本轮没有产品代码、测试或第三方导入；上述命令均是实施阶段验收步骤。

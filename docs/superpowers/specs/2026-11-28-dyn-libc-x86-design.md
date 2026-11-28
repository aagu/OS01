# 共享 libc(T3)设计:MAP_PRIVATE file-backed 装库 + libc.so + 动态构建开关

日期:2026-11-28 · 依赖:T1(09863697)与 T2(237f0b55)已合并 master
范围:x86_64(aarch64 属 T4);方案 C2+B2 已由用户裁决

## 目标与范围

roadmap P5 依赖链 `ELF loader ✅ → 动态链接器 ✅(T1/T2) → **共享 libc(T3)** → Alpine apk/musl`。

**T3 交付**:
1. **内核 MAP_PRIVATE file-backed 映射成为受测通路**——ld.so 装库从「anon mmap + read 灌段 + 预触 + mprotect」切换为「per-segment file-backed mmap + mprotect」(方案 C2;清偿 T2 遗留债务「段间空洞页 anon RW」)。
2. **libc.so 进 sysroot 与 rootfs**——OS01 libc 源码单独 `-fPIC` pass 产出 `/usr/lib/libc.so`(链接期)/`/lib/libc.so`(运行期),SONAME `/lib/libc.so`;libc.a 与静态链接保持不变、仍是默认。
3. **动态构建开关**——systest 夹具 `dyn_libc`(经 libc.so 调 printf/malloc/snprintf 等)+ 一个真实小程序按 knob(`OS01_USER_DYNLIBC=1`)动态构建并入镜像(方案 B2)。
4. **清偿 T2 记账的两项债务**:SHN_ABS 公式自动化覆盖;解释器 ABI 选择裁决落档。

**不含**(后续片):musl/Alpine 兼容、`dlopen`/`dlsym`、GNU hash(库仍 `--hash-style=sysv`)、.so 内 TLS、DT_INIT/FINI 与 init_array、lazy PLT、aarch64(T4)、多程序全量动态化。

## 现状(master 237f0b55,2026-11-28 实测锚点)

**内核 MAP_PRIVATE file-backed 通路已存在但零测试覆盖**——这是本片内核侧的主发现:

- `do_mmap`(`kernel/arch/x86_64/memory/mmap.c:529`)对 `MAP_PRIVATE`+文件 fd 无任何拦截:`map_flags_to_vm`(`:110-113`)只给 `MAP_SHARED` 置 `VMA_SHARED`、`MAP_ANONYMOUS` 置 `VMA_ANON`,private+file 的 VMA 只带 prot 位、`vm_file` 挂 node、`vm_shared_backing` 为 NULL;`fd==-1` 仅在 `MAP_ANONYMOUS` 分支强制(`:555`)。O_RDONLY + `PROT_WRITE` + MAP_PRIVATE 合法(Linux 同语义:写落私有副本)。
- **demand-fill 已通**:`trap.c:714-737` 对 `vm_file && !VMA_IO` 的 P=0 fault 分配页、按页粒度 `vfs_read`(`file_off = (cr2页基 - vm_start) + vm_pgoff*4096`,含 VMA split 后 pgoff 推导)、EOF 短读零尾填充、按 `x86_vma_pte_flags(vm_flags)` 映射。脏页永不写回文件。
- **mprotect 全量重写驻留 PTE**(`mmap.c:823-1010`):VMA 边界 split + `vm_pgoff` 右段推导(`:781` 起)、R→W 升级与 W→R 收紧双向、`PROT_NONE` 用 `PAGE_PROTNONE` 暂存 PA(`:982`);对 file-backed VMA 无 shared_backing 分支副作用。
- **fork 语义正确**(`sched/fork.c:176-217`):private-file 脏页(可写 VMA + PAGE_WRITE)→ COW 共享(pass 2 双 refcount + R/O);干净页 → 子进程整页拷贝(非共享,语义等价、RSS 翻倍可接受);`fork_vma_copy` 字段级复制带 `vm_file` 引用(`vma.c:108-116`),子进程未驻留页按文件重新 demand-fill。
- 缺口:**private-file 路径已有部分覆盖但语义面不全**——`test_mem.c:864` `test_fixed_file_replace`(MAP_PRIVATE 文件映射 fill、munmap 挖洞后 pgoff 递推、MAP_FIXED anon 替换、邻页完好)与 `test_mem.c:735` `test_mprotect_subrange_cow`(子域收缩+COW,anon)已注册(`systest.c:485,489`);**未覆盖**:EOF 短读零尾(现用例文件恰 4 整页)、写私有性(脏页不落文件)、fork 隔离、mprotect R→W 升级重写驻留页、整窗 PROT_NONE。另 `test_mem.c:273` 只读 128 MiB anon 大映射与 MAP_SHARED 用例与本片无关。

**ld.so(T2)**:`user/ldso/ldso.c` 已实现 DT_NEEDED 装载(anon mmap+read+用户态逐页预触)、SYSV hash、GLOB_DAT/JUMP_SLOT/RELATIVE、scope=main→libs、exit 91/95-99。其「预触」是绕内核 read 不做 demand-fill 的 workaround——file-backed 映射下用户态自访触发 fault-fill,**预触整段消失**。

**libc**:无 TLS、无 constructor/destructor、无 `.init_array`(全树 grep 零命中)——天然适合做 .so。`csu/csu.c`(`__libc_start_main`,定义 `__libc_auxv`/`__libc_stack_end`)在 `LIBC_C_SOURCES`(`wildcard csu/*.c`)→ 已在 libc.a,同样进 libc.so;`environ` 定义在 `stdlib/environ.c:42`。`arch/x86_64/crt0.S` 的 `_start` `call __libc_start_main`(动态 main 下为 JUMP_SLOT 进 libc.so);栈 canary `__stack_chk_guard` 由 csu.c 经 getrandom 播种,fail-closed。

**内核白名单(T2)**:interp 主镜像放宽 5 tag(NEEDED≤8/JMPREL/PLTRELSZ/PLTGOT/PLTREL=RELA)、RELA 容忍符号型(addend==0)、RELACOUNT 前缀、SYMTAB/HASH 位置规则(触发条件:NEEDED≥1 ∨ 符号型 RELA ∨ 非空 JMPREL;须落无 PF_W PT_LOAD)。动态链接 libc.so 的主镜像:DT_NEEDED=1、GLOB_DAT/JUMP_SLOT 来自 libc 调用、非空 JMPREL——**全部已被 T2 契约覆盖,内核侧零改动**;`X86_ELF_PIE_MAX_RELA=65536` 充裕。GNU-note 主镜像 + PT_INTERP 仍 `-ENOEXEC`(T2 exec 门槛,libc.so 主镜像无 note 不受影响)。

## 语义(T3 契约)

### 内核侧(x86_64,预期零或近零行为改动 + 测试)

private-file 通路保持现状语义;本片内核工作是**在既有部分覆盖之上补全语义面**(`test_fixed_file_replace`/`test_mprotect_subrange_cow` 为增量基座,不重写):

1. **systest 新用例(`user/systest/test_mem.c` 追加,沿用 `test_fork_mmap_cow_isolation` 模式)**:
   - `mmap_private_file_eof_tail`:造**非页对齐尺寸**文件(如 0x1800+0x37 字节)→ MAP_PRIVATE 映射整页跨度 → 末页 `[文件尾, 页界)` 读回必须为 0(EOF 短读零尾),页内文件部分逐字节比对。
   - `mmap_private_file_write_isolated`:映射 PROT_READ|WRITE → 改写页内字节 → **文件内容不变**(read 回验)+ munmap 后全新重映射读到原始内容(脏页私有,不落文件)。
   - `mmap_private_file_fork_isolation`:父映射**先写共同基线** → fork(fork 后子进程必须看到基线值)→ **写/读次序固定(评审 P2 第五轮;若「各自写后立刻各自读」,两者错误共享同一页时也能各自通过)**:管道按步同步——① 父写 A → 通知子;② 子先确认自己视野仍是**基线**(若看到 A 即页被错误共享,此处即失败)→ 子写 B → 通知父;③ 父复查仍读到 A(未被 B 破坏)→ 双方各自 read 文件断言仍为**原始内容**(基线 A 与 B 均未落文件)。
   - `mmap_private_file_mprotect_rw_upgrade`:MAP_PRIVATE PROT_READ 映射并触一页(驻留 PTE 为 RO)→ mprotect 加 W → 原地写成功(驻留页 PTE 被重写,非仅改 VMA)→ 文件不变。
   - `mmap_private_file_prot_none_guard`:整窗 PROT_NONE 访问 SIGSEGV(经由现有 fork 崩溃用例管道模式)。
2. **若实施中发现行为缺口**(如 mprotect 对 PROT_NONE 暂存页回切 file-backed 的边界),修复须带 RED→GREEN 用例;**预期为零修复**——通路四点已代码走查确认。
3. 内核 ELF 校验器、白名单、exec 通路:T3 **零改动**(dyn_libc 主镜像形状 = T2 已验收的 dyn_link 形状 + NEEDED 指向 libc.so)。

### libc.so 构建(`libc/` + `mk/components/sysroot.mk`)

1. **新 -fPIC 编译 pass**:libc 源以 `*.pic.o` 后缀第二遍编译(`-fPIC` 替换 `-fPIE`——PIE 代码假设无跨镜像插译,.so 必须真 PIC;其余 CFLAGS 同 libc)。产物经 lld 链接为 `libc.so`:
   - **重复符号处置(评审 P1-1,宿主实验已复现)**:全量 PIC 编译 188 对象后 lld 因 `htons`/`ntohs`/`htonl`/`ntohl` 重复定义拒绝链接——`unistd/net_stubs.c:26-29`(bswap,LE 上 POSIX 正确)与 `arpa/inet.c:3-6`(恒等返回,**LE 上语义错误**)各定义一组。**裁决:`arpa/inet.c` 四个恒等实现删除,四原语唯一提供方为 `net_stubs.c`(bswap)**。`arpa/inet.c` 全文件仅此四函数(6 行),删除后文件移除、构建清单同步。连带效果:静态 libc.a 的潜在重复符号雷(同符号双成员)一并排除;**htons 语义由恒等修正为 bswap 会改变 nettest/udptest/netmodeltest 的线上端口字节序**(`user/nettest.c:67` 等)——验收显式重跑这些网络用例:两端同为 OS01 程序时字节序自洽、预期零回归;若有宿主侧固定端口对端被打破,属既有调用方补偿 bug,单列修复、不回退原语语义。
   - 链接 flags 从 T2 库配方抄:`--hash-style=sysv -z now -z norelro -z max-page-size=4096 --pack-dyn-relocs=none`,`-soname /lib/libc.so`,动态 main 链接时 `--dynamic-linker /lib/ld.so`。
   - **`crt0.S` 不进 libc.so**(主镜像入口对象,静态保留);`csu.c` 进 libc.so(`__libc_start_main` 经 JUMP_SLOT 解析)。
   - **宿主实验(spec 定稿前必须钉死)**:① `llvm-readelf` 检查 libc.so dynsym 的 UNDEF 符号集——**WEAK UNDEF 预期为零**(全 libc 源码仅 `include/sys/syscall.h:126` 一处 `__attribute__((weak))` 声明且零调用方,已 grep 证实);若实验推翻此预期,ld.so 须补「weak undef 解析为 0」(当前未解析即 exit 96)。STRONG UNDEF 列表必须全落在 ld.so 已支持面。② RELA/GLOB_DAT/JUMP_SLOT 计数与 RELATIVE 计数,确认 ld.so 现有循环无上限拦截。③ 确认无 COPY 重定位(-pie 主镜像不产,lld 已证)与 .so 内 TEXTREL。④ 段页窗互不重叠断言(见 ld.so 契约第 1 条)。
2. **libc.a 语义与增量重建(评审 P2-2,两轮修正)**:静态 pass 对象与清单不动(`.o` 后缀、编译键、发布路径、`test-libc-api-audit`),**例外一:上述 arpa/inet.c 删除**(静态动态同受益);**例外二:libc.a 归档对「对象清单」可重建**——两层机制缺一不可:① recipe 补 `rm -f $@`(同 libm.a 先例,`libc/Makefile:131-137` 现裸 `ar rcs`,对象缩短后旧成员残留——roadmap 已记缺口;评审实测删源重跑 ar rcs 旧成员仍在);② **对象清单 stamps 纳入归档依赖**——`rm -f` 只在 Make 判定 recipe 需要运行时才生效,而删除源码只是缩短 `LIBC_ALL_OBJS`,其余对象未变时归档仍判 up to date(评审实测 `up to date`、旧成员仍在)。修法仿 `ROOTFS_INPUTS_STAMP`/编译键先例:`LIBC_OBJLIST_STAMP` 文件 FORCE 重写当前对象列表 + `cmp` 判变,`$(LIBC_ARCHIVE)` 依赖它——清单增减都触发归档重建。**stamp 只做触发、不得入包(评审 P2 第四/五轮)**:现 recipe `ar rcs $@ $^` 会把 stamp 文本一起打包(评审实测 llvm-ar 收录、lld 报非 ET_REL)——归档命令改**显式传 `$(LIBC_ALL_OBJS)`**(recipe 不用 `$^`);且 stamp 必须是**普通前置**——order-only(`|`)只保证构建顺序、stamp 变新**不触发**归档 recipe 重建(评审 GNU Make 实测),机制即失效。仅动 libc.a(libk.a 不含 arpa/inet.c、其缺口仍留 roadmap 台账)。**验收**:在**带旧 inet.o 的构建目录**(先正常构建一次,再删 arpa/inet.c)不 clean 增量构建,`llvm-nm` 断言 libc.a 与 libc.so 中 htons/ntohs/htonl/ntohl 各恰一个定义(指向 net_stubs 的 bswap 实现)。libm.a 仍静态(malloc/printf 内部若调 libm 需改 —— 宿主实验确认,预期无)。
3. **sysroot 布局**:`$(SYSROOT)/usr/lib/libc.so` 随生成租赁发布;动态链接消费它,静态链接继续用 libc.a。头文件零改动(同一套头服务两种链接)。
4. **rootfs 布局**:`/lib/libc.so=$(USER_ARTIFACT_DIR)/libc.so:0755` 加入 `config/rootfs.mk` ROOTFS_FILES(与 `/lib/ld.so` 同目录);libc.so 与动态程序产物经 `USER_INTERP_RUNTIME` 组(见「dyn_libc 夹具与真实程序」第 3 条)进 `ROOTFS_MANIFEST_DEPS`——**防 staging 陈旧**(gotcha:漏列则 guest 跑旧产物)。

### ld.so(`user/ldso/ldso.c`,装库机制切换)

1. **per-segment file-backed 映射**(替换 anon span + read + 预触;评审 P1-2 宿主实验修正):
   - **页窗对齐规则**:每个 PT_LOAD 的映射窗为 `[align_down(p_vaddr), align_up(p_vaddr+p_filesz))`(VA 左界向下取整、右界向上取整),mmap 文件偏移为 `align_down(p_offset)`——**不可直传 p_offset**:内核 `do_mmap` 对非页对齐 offset 返回 `-EINVAL`(`mmap.c:552`),且 lld 实产 RX 段 `p_offset=0x62b0` 非页对齐;`p_vaddr ≡ p_offset (mod 4096)` 保证窗内 VA→文件字节映射仍逐字节正确。
   - **同页冲突契约(评审 P2-1 修正:按完整内存窗)**:重叠检查用**完整内存窗** `[align_down(p_vaddr), align_up(p_vaddr+p_memsz))`——不能沿用文件窗(filesz 覆盖域):那会漏掉匿名 BSS 尾与纯 BSS 段,后续 MAP_FIXED 仍可能替换相邻段已安装的页。任两段内存窗重叠 → exit 98 形状拒绝(不合并、不清零);形状门 checker 对 libc.so 用**同一规则**断言(pinned lld flags 下必然满足,断言兜住未来产物)。T2 的「重叠窗权限不同 → 拒」规则被本条覆盖(重叠即拒,不分权限)。
   - **每段安装序(评审 P1-1 修正:先清零后收紧权限)**——三步,顺序不可倒:
     1. **建窗**:`[align_down(p_vaddr), align_up(p_vaddr+p_filesz))` `mmap(fd, MAP_PRIVATE, offset=align_down(p_offset))`;**匿名 BSS 尾的建立条件是页窗右界严格大于文件窗右界**:`align_up(p_vaddr+p_memsz) > align_up(p_vaddr+p_filesz)` 时才 `mmap(fd=-1, MAP_ANON|MAP_PRIVATE|MAP_FIXED)` 于 `[align_up(p_vaddr+p_filesz), align_up(p_vaddr+p_memsz))`(评审 P1 第三轮:`p_memsz > p_filesz` 不足以作为条件——文件尾与内存尾同页时(评审产物 RW 段两者都 align 到 0x2c000)区间为空,mmap 长度 0 → 内核 EINVAL;长度 0 的 anon 固定窗**不建立也不需要**,页内清零由第 2 步覆盖);**`p_filesz==0` 的段整段走 anon MAP_FIXED**(`[align_down(p_vaddr), align_up(p_vaddr+p_memsz))`),不开文件窗。**初始 prot 一律 `PROT_READ|PROT_WRITE`**(承 T2 模型;内核拒纯 W,RW 合法)。
     2. **清零 BSS 页内尾**(仅当 `p_memsz > p_filesz` 且 `p_vaddr+p_filesz` 不页对齐):`memset` `[p_vaddr+p_filesz, align_up(p_vaddr+p_filesz))`——此页已 file-backed、页内含文件后续字节,必须清;此步**必须在 mprotect 之前**(初始 RW 窗可写;若先收紧到 RX 再 memset,典型 RX 段直接 SIGSEGV)。`p_memsz == p_filesz` 的段(典型 RX text)无 BSS、跳过清零;`p_vaddr+p_filesz` 页对齐时无页内尾(文件尾恰在页界,BSS 全在后续 anon 页,天然为零)。
     3. **收紧权限**:anon 尾段全部建好后,对每段完整内存窗统一 `mprotect` 到段最终权限(页对齐窗)。
   - **主控窗口法**:先 `mmap(NULL, 全 span 页对齐长度, MAP_ANON|PRIVATE)` 占住连续窗口得 map_start(承 T2 现有逻辑),再逐段按上式 `MAP_FIXED` 替换为 file-backed/anon;最后首段前窗与段间空洞 **munmap**(段间空洞不再映射——T2 债务「空洞页 anon RW」在此语义下消失:空洞访问 = 真实 SIGSEGV)。bias = map_start − align_down(min p_vaddr) 与 T2 公式一致。
   - 预触逻辑整体删除(内核 fault-fill 代劳);`copy_to_user_ft` 的 EFAULT workaround 注记一并删除。
2. **主镜像路径不变**:main 的 RELATIVE 已由内核应用,ld.so 对 main 只做 GLOB_DAT/JUMP_SLOT(T2 契约);仅库(libc.so)走上述 file-backed 装载。
3. **weak undef**:静态审查已证实 libc 源码零 weak 引用(唯一声明 `host_test_syscall` 零调用方)——预期 libc.so 无 WEAK UNDEF,ld.so 维持「未解析 → exit 96」;宿主实验仍复核 dynsym,若意外出现 weak undef 再按 glibc 语义补「weak undef → 0」。
4. 失败码沿用 91/95-99;新失败点(file-backed mmap 失败、BSS 尾 mmap 失败)归 99(内部)或 95(资源),复用现有 `[LDSO-ERR-*]` 单行打印。

### 动态 main 链接配方(`user/Makefile` T3 段,抄 T2 段骨架)

```
main.o: -fPIE -mcmodel=small -mno-red-zone(同既有)
dyn_libc.elf: crt0.o + dyn_libc.o + $(SYSROOT)/usr/lib/libc.so
  $(TARGET_LD) -m elf_x86_64 -pie --dynamic-linker /lib/ld.so \
    --hash-style=sysv -z now -z norelro -z max-page-size=4096 \
    --pack-dyn-relocs=none -e _start …
```
- **不链 libc.a**(全部 libc 符号经 libc.so);漏链入口对象 → e_entry=0 被内核拒(gotcha 已知)。
- 位置规则自动满足:主镜像带 NEEDED≥1 → lld sysv 产物 dynsym/hash 天然落首个 R LOAD(T2 已证)。

### dyn_libc 夹具与真实程序(knob)

1. **`user/systest/fixtures/dyn_libc.c`**:freestanding 风格 main(同 T2 夹具形态,但**经 libc.so**):`printf`/`snprintf` 格式化比对、`malloc`/`free`/`realloc` 往返、`memcpy`/`strlen`、**`getenv("OS01_DYNLIBC")` 断言精确回值**(评审 P2-2:父用例 exec 时显式传含 `OS01_DYNLIBC=<约定值>` 的 envp——现有 `test_interp.c:20,43` helper 传 `NULL` envp、内核构造空环境,`environ` 非空在空环境下也成立,不构成验证;夹具同时断言 `environ[0]` 即该串与 `getauxval(AT_PAGESZ)==4096`(auxv 全局在 libc.so 内,验证 csu 归属))、printf 输出经 stdout 到串口 marker `[DYNLIBC-OK]` / 失败 `[DYNLIBC-FAIL-<n>]`,正常 `_exit(0)` 或异常码 9x。
2. **端到端证明「ld.so 真的走了 file-backed 装载」(评审 P2-3)**——现有验收组合在 ld.so 退回 T2 anon+read 方案时也能全绿,必须补直接证据:
   - **`/proc/self/maps` 路径与偏移断言(并入 dyn_libc 夹具;评审 P2 第四轮修正)**:OS01 procfs maps 是 Linux 风格行,带 pathname(`procfs.c:229` vfs_resolve_path)、offset 列(`:222` `vm_pgoff<<12`)与私有标志(`:219` 非 VMA_SHARED → `'p'`)。**路径 + `p` 标志即 file-backed↔anon 判别器**(anon+read 方案的行无路径无偏移);模 4096 同余断言**废弃**(行内 VA 与 offset 都已页对齐,同余对任何非零偏移恒真、验证不了计算)。**核对方向从 ELF 出发,文件窗与匿名尾分别验收(评审 P2 第五/六轮)**:夹具运行期 `open`/read `/lib/libc.so` 自身解析 ELF phdrs(顺带多练 libc 路径),bias 从 `/lib/libc.so` 最低行起始 − `align_down(min p_vaddr)` 推得;对**每个 `p_filesz > 0` 的 PT_LOAD** 独立断言:① 存在 pathname=`/lib/libc.so` 的 maps 行完整覆盖该 LOAD 的**文件窗**(`行起始 ≤ align_down(p_vaddr)+bias` 且 `行结束 ≥ align_up(p_vaddr+p_filesz)+bias`——**不要求覆盖到 p_memsz 页尾**:跨页 BSS 的合法装载结果就是文件窗路径行 + 匿名尾行,单行盖满 memsz 与装载契约冲突,评审宿主实验已用跨页 BSS 合法共享库复现);② 该行标志列为 `p`;③ 该行 offset 列 == `align_down(该 LOAD 的 p_offset)`;④ **该 LOAD 存在跨页 BSS 时**(页窗右界严格大于文件窗右界),独立断言存在匿名行完整覆盖 `[align_up(p_vaddr+p_filesz)+bias, align_up(p_vaddr+p_memsz)+bias)`(无 pathname、offset 列为 0、标志列 `p`)。单向「从已找到的行反查」会漏掉某段回退匿名映射仍通过的情形——缺行、回退 anon、偏移错、尾段缺失任一即 `[DYNLIBC-MAPS-FAIL-<n>]`,全过打 `[DYNLIBC-MAPS-OK]`。
   - **空洞未映射负例夹具(评审 P2 第四/五轮修正:确定布局 + 实际装载地址上的全 VMA 覆盖检查)**:`-z separate-loadable-segments` **不足以为空洞制造依据**——评审实测 lld 23.1.5 下三个 PT_LOAD 页窗首尾相接、无空页(该 flag 只保证段不共享页,不保证中间有空页)。改用**固定布局**:libholes.so 以最小化定制链接脚本链接,显式在两段之间留出整页空洞(脚本须保住装载器契约:p_vaddr≡p_offset mod 4096、DT_HASH 落 R LOAD、pinned flags 不变——确切配方 plan 宿主实验钉死,已确认可行);夹具程序 `dyn_hole`(NEEDED libholes)运行期读 `/lib/libholes.so` phdrs,先**从 maps 中匹配首个文件段的行求实际装载偏移**(`bias = 行起始 − align_down(首段 p_vaddr)`;无匹配行即失败),再把链接虚址空洞区间换算到进程地址:`[前段内存窗右界+bias, 后段内存窗左界+bias)`,断言 **maps 中没有任何行(无论 pathname)覆盖该区间任一字节**——程序头算出的是 ELF 链接虚址,直接检查会让低地址范围天然无 VMA 恒过,漏掉实际装载区仍保留的匿名页;只看同路径行间距也会把残留 anon 保留映射误判为空洞。打 `[DYNHOLE-OK]` / 失败 `[DYNHOLE-FAIL-<n>]`;systest 用例 `exec_dyn_hole` rc==0。空洞访问 SIGSEGV 的崩溃式证明不做(实际地址上的全 VMA 覆盖检查已是结构性证据)。
3. **systest 用例 `exec_dyn_libc`**(test_mem.c 或 systest.c 注册):fork → exec `/bin/dyn_libc`(带 OS01_DYNLIBC envp)→ waitpid rc==0;串口窗口断言(见验收)。
3. **真实程序 knob 与运行时依赖发布(评审 P1-3)**:现状 `/lib/ld.so` 的构建、产物复制与 rootfs 安装都挂在 `OS01_SYSTEST=1` 下(`USER_INTERP_FIXTURES` 条件定义,`mk/components/user.mk:35`;`config/rootfs.mk:14`)——**只开 `OS01_USER_DYNLIBC=1` 的普通镜像若沿用该条件,动态程序链接成功但 exec 因缺解释器失败**。裁决:新增 `USER_INTERP_RUNTIME` 产物组(`ldso.elf` + `libc.so` + 动态版真实程序),构建与 staging 条件为 **`OS01_SYSTEST=1 ∨ OS01_USER_DYNLIBC=1`**;T2 夹具(dyn_link 系/libbad_dyn)与 T3 夹具(dyn_libc/dyn_hole/libholes.so)均仍 SYSTEST-only。`config/rootfs.mk` 的 `/lib/ld.so`、`/lib/libc.so`、`/bin/<prog>.dyn` 三项按该条件展开进 ROOTFS_FILES(静态原版程序无条件保留)。**验收**:① knobby 镜像构建后形状门通过、rootfs manifest 含三项;② **knob-only 普通镜像(不设 SYSTEST)在 QEMU 内实际 exec 动态版程序成功**——python harness 场景驱动(plan 定挂点,参照 usb-boot 场景的串口交互法),断言动态版输出 marker;③ SYSTEST 镜像内 `exec_dyn_libc` 与真实程序动态版用例照常。
4. **checker 参数化**:`tools/tests/check_interp_fixture.py` 增 `--dynlibc`/`--libc`/`--prog-dyn`/`--dynhole`/`--libholes` 参数;allowed_types:主镜像 {RELATIVE,GLOB_DAT,JUMP_SLOT},libc.so {RELATIVE,GLOB_DAT,JUMP_SLOT}(RELATIVE 须 ≥1——libc.so 有初始化的全局指针,同 T2 t2_lib volatile 教训);形状断言:libc.so 有 DT_SONAME=/lib/libc.so、DT_HASH(sysv)、主镜像 DT_NEEDED=[/lib/libc.so]、**libc.so 全部 PT_LOAD 按 `[align_down(p_vaddr), align_up(p_vaddr+p_memsz))` 内存窗互不重叠**(与 ld.so 运行期检查同规则)、libholes.so 断言段间确有整页空洞(固定布局链接脚本生效)。
5. **SHN_ABS 自动化覆盖(债务 2;评审 P2-3)**:宿主单测(hosttest,`test_ldso_units` 追加)以合成 dynsym/RELA 夹具钉公式:SHN_ABS GLOBAL 符号经 GLOB_DAT 引用 → 写 `S=st_value` 不加 bias,**st_value 允许为镜像窗外的绝对常量**(如 `0x12345678`——SHN_ABS 值是绝对地址/常量,加镜像窗约束会破坏 T2 已定义的 S=st_value 公式;正例就用窗外常量);普通 GLOBAL(定义在库)→ 写 `定义方 bias + st_value`。**负例落在重定位目标槽**:目标 `r_offset` 越出所属镜像可写段窗 → 拒(走 T2 既有目标规则,不用例重复造)。guest 侧不强制 lld 产 SHN_ABS(宿主实验若轻易可得 `--defsym` 路径则加形状门断言,否则记入已知未测项)。
6. **解释器 ABI 裁决(债务 3)**:落档为**维持现状**——GNU-note 主镜像 + PT_INTERP 保持 exec 两入口 `-ENOEXEC`(T2 契约不变);OS01 libc.so 动态镜像不带 GNU note、走原生 int80 ABI;GNU-note 动态二进制(未来 musl/Alpine 路线)打开时须先裁决「解释器任务级 ABI 分流」——记入 roadmap Alpine apk/musl 行前置条件,本片不做 compat 分流代码。

## 验收与证据

1. **宿主门禁**:`make test-host`(含 test_ldso_units 新 SHN_ABS/weak-undef 用例)全绿;`make test-static` 全绿(ldso.c 若改帧布局,`x86_elf_pie_prepare` 512B 审计同口径关注)。
2. **QEMU systest**(`make OS01_SYSTEST=1 test-syscall`):新增用例全绿——private-file 五用例 + `exec_dyn_libc`(rc=0,含 maps 判别断言)+ `exec_dyn_hole`(rc=0)+ 动态版真实程序用例;T1/T2 既有 138 用例零回退。
3. **串口窗口门**(`run_test.py::_check_ldso_serial_windows` 扩展):`exec_dyn_libc` 窗口含 `[LDSO-ENTER]` → `[LDSO-RELOC]` → `[DYNLIBC-MAPS-OK]` → `[DYNLIBC-OK]` 有序;`exec_dyn_hole` 窗口含 `[DYNHOLE-OK]`;任何 `[DYNLIBC-FAIL-*]`/`[DYNLIBC-MAPS-FAIL-*]`/`[DYNHOLE-FAIL-*]` 出现即用例失败;窗口按 BEGIN/同 ID terminal 定界、按 exec 分组(T2 已建机制,只扩预期组)。
4. **形状门**(`check_interp_fixture.py`):dyn_libc/libc.so/动态版真实程序/dyn_hole/libholes.so 五项新参数全断言通过(含 libc.so 内存窗互不重叠、libholes.so 段间有洞);既有九参数零回退。
5. **knob 证据**:`OS01_USER_DYNLIBC=1`(不设 SYSTEST)构建出 `USER_INTERP_RUNTIME` 三件(ldso/libc.so/动态版程序)且形状门通过、rootfs manifest 含 `/lib/ld.so` `/lib/libc.so` `/bin/<prog>.dyn`;**knob-only 普通镜像 QEMU 内实际 exec 动态版程序成功**(harness 场景断言输出);默认(不带 knob)构建零变化(产物集 diff 干净)。
6. **网络回归(htons 语义修正)**:htons/htonl 恒等→bswap 修正后,重跑 x86_64 网络用例(nettest/udptest/netmodeltest 所在 suite),预期零回归;任何回归按「既有调用方补偿 bug」单列修复并记录,不回退原语语义。
7. **staging 一致性**:`ROOTFS_MANIFEST_DEPS` 含 libc.so、dyn_hole/libholes.so 与动态程序产物(改产物后 image 重打,guest `strings` 对账——T1 教训)。
8. **文档闭环**:roadmap P5 动态链接器行更新(T3 落地边界)、三项债务在 changelog 记清偿方式;`docs/arch.md` 若有用户态布局章节则补 libc.so 段落。

## 留给实施者裁决

- libc.so -fPIC pass 的 make 结构(独立变量组 vs 复用 LIBC_OBJS 双后缀);编译键命名与增量触发口径。
- 真实程序候选终选(date vs setres vs 其他;须先读 app.mk 依赖)。
- `USER_INTERP_RUNTIME` 组的 make 落点(新顶层变量 vs 扩展既有 `USER_INTERP_FIXTURES` 条件;staging 归属与 `ROOTFS_MANIFEST_DEPS` 挂法)。
- ldso.c 拆分与否:装库函数若超帧预算 512B,拆 helper(参照 T2 `load_page_window` 先例)。
- hosttest 对 ld.so 新逻辑的宿主可测面(per-segment 窗口计算与页窗重叠检测的纯逻辑单测,test_ldso_units 模式)。
- weak-undef 分支的落点(ldso 符号解析函数最小改 vs 独立 helper)。
- libholes.so 固定布局链接脚本的确切配方(整页空洞位置、与 pinned flags/装载器契约的共存——宿主实验钉死后入 plan)。
- knob-only 镜像 QEMU 场景的 harness 挂点(run.mk scenario-catalog vs 独立 python harness;见已知风险末条退路)。

## 已知风险

- **htons 语义修正的网络回归面**:恒等→bswap 改变 nettest/udptest/netmodeltest 线上端口字节序。两端同为 OS01 程序时自洽(绑定与目标同侧换),预期零回归;宿主侧固定端口对端若存在,现网行为本就是错端口(或测试以自洽方式通过),回归则单列修复。验收第 6 条显式重跑。
- **libc.so 尺寸/RELA 数量**:libc 全量符号(几百个导出)首度走 ld.so 运行期;RELACOUNT/符号解析循环为 O(n) 无缓存——单库一次解析可接受,但若 dyn_libc 用例耗时异常须回报(spec 不预设优化)。
- **printf 的 stdout 路径**:libc stdio 经 write(1) 到串口——夹具在 fork/exec 的子任务里 stdout 是否继承 tty 由 systest 现有 exec 用例同构,T2 dyn_link 已在子任务跑过 stdio(printf marker),预判无坑;若 stdout 缓冲语义(全缓冲 vs 无缓冲)踩坑,夹具改 `_exit` 前显式 flush。
- **`--defsym` 产 SHN_ABS 与否**:宿主实验决定 guest 覆盖深度,不阻塞主链。
- **内核 private-file 通路若实测有缺口**(走查结论相反):风险局部化在 mmap.c/trap.c 各自分支,修复带用例,不外溢。
- **knob-only 镜像的 harness 场景挂点**:普通产品镜像的 QEMU 场景驱动若与 run.mk 既有 scenario-catalog 不咬合,退路是「manifest 断言 + PT_INTERP 检查」先行落 CI,QEMU 实跑场景作为本片内独立提交补上——不允许只交 manifest 断言就声明 P1-3 闭环。

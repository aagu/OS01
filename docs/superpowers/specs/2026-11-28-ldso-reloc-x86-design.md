# ld.so 符号重定位(T2)设计

2026-11-28

## 目标与范围

roadmap P5「动态链接器」第二片。T1(09863697)交付了 exec 通路:内核识别 `PT_INTERP`、装载解释器为第二镜像、发布 `AT_BASE`、入口跳解释器;ld.so 只是骨架(auxv 走查 + 握手对 + `jmp AT_ENTRY`),不重定位任何东西。T2 把符号重定位移交用户态 ld.so:

**交付**:动态链接的主程序(`PT_INTERP` + `DT_NEEDED` + `GLOB_DAT`/`JUMP_SLOT` 重定位)经 `/lib/ld.so` 装载共享库、按 SYSV hash 解析符号、完成重定位后运行到 `exit(0)`;缺库路径以非零 rc 退出。仅 x86_64(aarch64 留给 T4)。

**不含**(后续片):共享 libc.so(T3)、aarch64(T4)、lazy PLT、GNU hash、.so 内 TLS、DT_INIT/FINI 与 init_array、多库传递闭包验收、`dlopen`/`dlsym`。

## 现状(master 09863697)

- T1 内核:`elf_load_at`(elf.c:457,`interp_mode` 参数)→ interp 必须 ET_DYN、嵌套 `PT_INTERP`/`PT_TLS` 拒绝;`elf_load_interp`(:820)独立 ASLR-B bias 装载、碰撞重采样 ≤16 次;`exec.c:266` 发布 `AT_BASE`;入口 = `has_interp ? interp_entry : entry`。
- T1 夹具:`user/systest/fixtures/interp_probe.c` 双 flavour(`-DINTERP_LDSO` → `ldso.elf` staged 为 `/lib/ld.so`;`-DINTERP_MAIN` → `dyn_exec.elf`,RELATIVE-only `-nostdlib`),握手对写在 auxv `AT_NULL` 后垫(T1 spec §7);形状门 `tools/tests/check_interp_fixture.py` + hosttests `test_elf_load_4k.c` 六用例。
- 内核 DT 白名单(elf_pie.c:250-330):`DT_FLAGS(DF_BIND_NOW)`/`DT_FLAGS_1(DF_1_PIE|DF_1_NOW)`/`DT_RELA` 三件套/`DT_RELACOUNT`/`DT_STRTAB/STRSZ/SYMTAB/SYMENT`/`DT_HASH`/`DT_GNU_HASH`/`DT_DEBUG`,其余 `default: -ENOEXEC`;RELA 记录仅接受 `R_X86_64_RELATIVE`(:405-409),目标须 RW-NX 段、不与 dyn/rela 区重叠。
- mmap syscall(`kernel/arch/x86_64/memory/mmap.c:529` `do_mmap`):anon + file-backed;`addr=0` 内核选窗(`mmap_find_gap_locked`),显式地址须 ≥ `X86_USER_MIN_ADDR` 且未占用(非 fixed);`MAP_FIXED` 事务化替换;`offset` 须页对齐。uapi 位值(Linux 同构):`PROT_READ/WRITE/EXEC=1/2/4`,`MAP_PRIVATE=0x02/MAP_FIXED=0x10/MAP_ANONYMOUS=0x20`。`mprotect/munmap` 均在。原生 int80 六参 ABI:`rdi/rsi/rdx/r10/r8/r9`(libc_arch/x86_64/syscall.h)。
- **宿主实验**(lld 23,`--hash-style=sysv -z now -z norelro --pack-dyn-relocs=none`,2026-11-28)钉住产物形状:
  - 动态 main(`ld.lld -pie --dynamic-linker /lib/ld.so`):DT = NEEDED / FLAGS(BIND_NOW) / FLAGS_1(NOW|PIE) / DEBUG / RELA 三件套 / JMPREL / PLTRELSZ / PLTGOT / PLTREL(RELA) / SYMTAB / SYMENT / STRTAB / STRSZ / HASH;重定位 = GLOB_DAT(extern 数据)+ JUMP_SLOT(函数),**不产 DT_RELACOUNT**;
  - libt2.so(`ld.lld -shared -nostdlib`):DT = SONAME / RELA 三件套 / SYMTAB / SYMENT / STRTAB / STRSZ / HASH(无 FLAGS、无 NEEDED);重定位 = RELATIVE(函数指针全局)+ GLOB_DAT(对自身导出数据的引用——默认可见性走 GOT);
  - JUMP_SLOT 记录位于 `PLTGOT+3*8` 起(GOT[0..2] 保留槽,eager 绑定下不用)。

## 方案(用户已裁决 A)

| 方案 | 取舍 |
| --- | --- |
| **A 匿名 mmap + read 灌入 + mprotect(采用)** | 只依赖 anon mmap + mprotect,不碰 file-backed demand-fault 路径(其 BSS 零填/部分页正确性尚无调用方考验);与内核装载器「整读」风格一致;代价:无 demand paging(测试库为页级小镜像,可接受) |
| B file-backed mmap 真映射 | 更接近 musl/glibc 形状,T3 的 libc.so 反正要走;但把 demand-fault 正确性拉进本片爆炸半径 |

(内核代载 .so 已被 T1 设计否定:T1 spec「内核装载 interp + 交接,T2 把符号重定位移交 ld.so」。)

## 语义(T2 契约)

### 内核侧(最小增量,`elf_pie.c`)

1. **DT 白名单放宽仅限 `pie.has_interp` 的主镜像**:`DT_NEEDED`(可重复,≤8 次)、`DT_JMPREL`、`DT_PLTRELSZ`、`DT_PLTGOT`、`DT_PLTREL`(值必须为 `DT_RELA`)。静态 PIE(无 interp)路径白名单零变化,这些 tag 照旧拒绝。
2. **RELA 记录容忍符号型**:interp 主镜像的 `.rela.dyn` 记录类型 ∈ {RELATIVE, GLOB_DAT, JUMP_SLOT}。RELATIVE 照旧内核自应用(T1 契约不变;与 ld.so 重复应用亦幂等);GLOB_DAT/JUMP_SLOT **校验但不应用**(移交 ld.so):r_offset 8 对齐、目标仍须落在 RW-NX 段、不与 dyn/rela 区重叠,且**内核先行应用的 RELATIVE 须避开本轮新增的动态元数据**(`[JMPREL, JMPREL+PLTRELSZ)`、`[STRTAB, STRTAB+STRSZ)`、`PLTGOT` 存在时 GOT[0..2] 保留槽;SYMTAB/HASH 属 T1 既有白名单,内核 floor 不扩,ld.so 禁写区覆盖);应用循环须按类型过滤,只写 RELATIVE;带 NEEDED ≥1、DT_RELA 符号型记录、或非空 JMPREL(tag 存在且 PLTRELSZ>0——内核不走查该数组,符号型可藏于其中)的 interp 主镜像另须存在 DT_HASH,且 HASH 区间 `(2+nbucket+nchain)*4` 与 SYMTAB 区间 `nchain*SYMENT` 经 16 字节哈希头推导后**各自完整落在无 PF_W 的 PT_LOAD 内**——内核先于 ld.so 应用 RELATIVE 写,落在 RW 段的这两张表可在 ld.so 校验前被改写(lld sysv 产物天然满足,T1 无 NEEDED/符号记录的镜像不受影响);**符号型 addend 必须为 0**——x86-64 psABI 表 4.2 定义 GLOB_DAT/JUMP_SLOT 写入 `S`(无 addend),非 0 addend 会把错误地址写进 GOT/PLT 槽,内核层 `-ENOEXEC`、ld.so 层 exit 98。`DT_RELACOUNT` 语义(glibc `do-rel.h` 前缀约定):`count ≤ 表长` 且**前 `count` 条记录必须全部为 RELATIVE**——只对账「全表 RELATIVE 总数」会放过交错排列;前缀内非 RELATIVE 或 count 超表长 → `-ENOEXEC`。lld 在含符号型时不产 RELACOUNT,故该约束只作用于混合/纯 RELATIVE 场景。
3. **JMPREL 数组内核不走查**:只接受 tag 与 PLTRELSZ/PLTGOT 形状;记录级校验(类型=JUMP_SLOT、offset 对齐、目标段)由 ld.so 运行时执行。记录为已知边界:内核不防 .rela.plt 指向任意 RW 页的恶意镜像,ld.so 的类型检查是唯一防线,足够测试夹具范围。
4. ld.so 自身保持 static PIE(`--no-dynamic-linker`、RELATIVE-only):内核装载路径零改动。`interp_mode` 嵌套拒绝语义不变(interp 镜像 `allow_interp=false` → 带 `DT_NEEDED`/`PT_INTERP` 的镜像不能当解释器)。
5. **ABI 门槛(T2 仅 native)**:int80 入口按**任务级** `PF_LINUX_ABI` 分流(trap.c:1085,`compat_linux_dispatch` vs 原生表),该标志由 exec 从主镜像 GNU ABI note 推得,而解释器与主镜像同任务——GNU-note 主镜像会把 ld.so 的原生 open/read/mmap 路由进 Linux 兼容表(编号全错)。T2 契约:主镜像带 `PT_INTERP` 且 `elf_detect_abi` 判定 `PF_LINUX_ABI` → exec 两个入口(`sys_exec` / `spawn_user_task`)显式 `-ENOEXEC`。T2 夹具 `-nostdlib` 无 note,不受影响;T3(musl/共享 libc)需重新裁决解释器在 compat 任务中的 ABI 选择(记入 T3 输入)。

### ld.so(`user/ldso/`,staged 为 `/lib/ld.so`,**替换 T1 骨架**并保持其契约)

1. 仍 `-nostdlib` 自包含(不链 libc.a):raw int80(open/read/mmap/mprotect/close/write/exit),ELF 结构自声明(沿用夹具模式)。入口复用 `interp_entry.S` 的 rsp 传递。
2. **T1 行为保留**:auxv 走查;`[LDSO-ENTER]` / `OS01_LDSO` marker 原名保留(T1 systest 字符串不变);握手对(TAG/MAGIC 写 `AT_NULL` 后垫)无条件写;T1 的 `dyn_exec`(无 NEEDED、RELATIVE-only)在新 ld.so 下行为不变;被直接 exec(`AT_BASE==0`)时防御性 `exit(91)` 保留(T1 systest 已断言)。
3. 主镜像 dynamic 定位:`AT_PHDR` → 内存 phdr 走查 → `PT_DYNAMIC`;主镜像 bias = `AT_PHDR - phdr[PT_PHDR].p_vaddr`(内核已校验 PT_PHDR 与 e_phoff 一致,elf_pie.c:202-208)。**兼容性条款(新增限制,明确记录)**:内核侧 PT_PHDR 仍可选并照常发布 `AT_PHDR`,但自 T2 起 ld.so 的 bias 推导**要求 interp 主镜像带 PT_PHDR**,缺失 → exit 98;仓库内 lld 产物(T1 dyn_exec 起)均带 PT_PHDR,无既有镜像受影响;无 PT_PHDR 的 interp 主镜像若后续需要支持,再评估「从 `AT_PHDR` 向下扫 ELF magic 找 bias」的回退路径(本片不做)。
4. DT_NEEDED 依赖装载(代码通用循环 + 去重,验收单库)。每项必须绝对路径(与 PT_INTERP 同契约,否则 exit 98);open → 读 ehdr+phdrs → **ld.so 自校验**:EM_X86_64、ET_DYN、`DT_HASH` 必须存在(GNU-hash-only → exit 98)。**段映射规则**(RX 段 p_offset=0x240 / p_vaddr=0x1240 的实际 lld 产物无法用「offset==vaddr」假设):
   - 每段校验 `p_vaddr ≡ p_offset (mod 4096)`、`p_filesz ≤ p_memsz`、段间虚区无叠、W+X 拒(违例 → exit 98);
   - `lo = align_down(min p_vaddr)`、`hi = align_up(max(p_vaddr+p_memsz))`、`map_len = hi - lo`;`mmap(0, map_len, RW, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)` 内核选窗;**`bias = map_start - lo`**(mmap 返回映射区起点而非 vaddr 0 点——`--image-base=0x200000` 构建的库首个 PT_LOAD 从 0x200000 起,`bias = map_start` 会把首段读出预留区;lo=0 时退化为此前公式);
   - 每段按 **p_offset 定位文件字节**,完整读 `p_filesz` 至 `map_start + (p_vaddr - lo)`(BSS 由 anon 零填覆盖);
   - 每段权限窗 = `[map_start + (align_down(p_vaddr) - lo), map_start + (align_up(p_vaddr + p_memsz) - lo))` 逐段 `mprotect`(RX/RW/RO,页对齐后设置);**两段权限窗重叠且权限不同 → exit 98**(共享页无单一权限可满足);
   - 窗口外空洞页(首段窗前/段间)保持 anon RW——已知债务,T3 file-backed 映射时消失。
5. **输入校验边界(P1)**:ld.so 解引用任何 ELF 元数据前——
   - **PT_DYNAMIC 自身先有界**(在取得 `DT_*` 指针之前就要扫它):对每个镜像(main/lib),PT_DYNAMIC 的文件范围(lib:`p_offset+p_filesz ≤ 文件大小`)、内存范围(main/lib:动态区完整落在**文件支持**的可读 PT_LOAD 内)、`p_filesz % 16 == 0`、条目数 ≤ 上限(plan 定值,与内核 `X86_ELF_PIE_MAX_DYN` 同量级)且 `DT_NULL` 在上限内出现;缺失/截断/越界 → exit 98;
   - 所有 `DT_*` 表地址(JMPREL/STRTAB/SYMTAB/HASH/RELA)与长度必须落在**所属镜像**的可读段窗口内(main 经 `AT_PHDR` phdrs 推导,lib 经其自身段窗);全部长度运算防溢出;符号索引 < nchain;字符串读取限 strtab 范围且要求 NUL 终止(限长扫描,不信任文件内 NUL 位置以外的内存);hash bucket/chain 遍历步数以 nchain 为上限;重定位只允许写**所属镜像**的 RW-NX 段窗口内目标,**且不得落入禁写区**——所属镜像的 PT_DYNAMIC 区、DT_RELA/JMPREL 表区、DT_SYMTAB/STRTAB/HASH 元数据区、`DT_PLTGOT` 存在时的 GOT[0..2] 保留槽(这些区域前置校验通过后仍要读,一次重定位改写后续记录会破坏解析;ld.so 校验阶段已算出全部范围,直接并入禁写判定,不做快照)。任一违例 → exit 98,不得先缺页或死循环。
6. 符号解析:SYSV `DT_HASH`(nbucket/nchain/buckets/chains + elf_hash),scope = main → libs 按装载序。定义 = GLOBAL/WEAK 且 `st_shndx != SHN_UNDEF`;**`S` 用定义所在镜像的 bias**(`S = 定义方 bias + st_value`);**SHN_ABS 定义接受**(宿主实验确认 lld 可产 GLOBAL ABS 符号并经 GLOB_DAT 引用):`S = st_value`,不加 bias。任一符号未解析 → exit 96。
7. 重定位应用(psABI 公式,`S = bias + st_value`):
   - main:`GLOB_DAT`/`JUMP_SLOT` 写入 **S**(无 addend,addend≠0 → exit 98);RELATIVE 跳过(内核已应用,重复亦幂等);
   - libs:RELATIVE = bias + A;GLOB_DAT/JUMP_SLOT 同上写 S;
   - 未知类型 → exit 97。
8. `PLTGOT` GOT[0..2] 不碰;`PLTREL` 必须为 RELA。
9. 失败 exit 码段 95-99(95 缺库/open 失败、96 符号未解析、97 未知重定位、98 镜像形状/契约/校验违例、99 内部错误),每步串口 `[LDSO-ERR-*]` 一行;成功在跳转前输出 `[LDSO-RELOC]`。

### 夹具与测试

- 新 lib:`user/systest/fixtures/t2_lib.c` → `/lib/libt2.so`(`-shared -nostdlib --hash-style=sysv -z norelro --pack-dyn-relocs=none -soname /lib/libt2.so`);导出 `t2_value`(数据)与 `t2_add`(函数)。**RELATIVE 记录必须被成功路径实际消费**:导出的 `t2_add` 经 `static long (*add_fn)(long,long) = add_impl;`(RELATIVE 初始化的函数指针)分发到内部实现并读 `t2_value`(自引用 GLOB_DAT)——若 ld.so 跳过库内 RELATIVE,`add_fn` 保留链接期未重定位值,调用即跳到错误地址,主程序结果错误或进程死于信号,用例必失败;形状门断言 libt2.so 的 `.rela.dyn` 含 ≥1 条 RELATIVE。
- 新 main:`user/systest/fixtures/t2_main.c` → `dyn_link.elf`(`PT_INTERP=/lib/ld.so` + `DT_NEEDED=/lib/libt2.so`,复用 `interp_entry.S`):调用 `t2_add`、读 `t2_value`、校验结果、写 `[DYN-T2]` marker、`exit(0)`。链接 `ld.lld -pie --dynamic-linker /lib/ld.so …`(**去掉 T1 配方里的 `-static`**——lld 拒绝 `-static -pie` 链接动态输入)。
- 失败夹具:同源 `-DT2_MISSING_NEEDED`。**DT_NEEDED 来自链接输入库的 DT_SONAME/文件名,编译宏改不了它**——机制:再构建一份 stub 共享库(同源同接口,`-soname /lib/libabsent.so`)→ `dyn_link_missing.elf` 链接**该 stub**(链接期符号可解析),镜像打包**省略 stub**,运行期 open 失败 → rc=95;形状门精确断言 `dyn_link_missing` 的 NEEDED == `/lib/libabsent.so`(防 vacuous pass)。
- 形状门:`check_interp_fixture.py` 扩展:ld.so 仍 RELATIVE-only static PIE 形状(自身动态形状不变);dyn_link 有 PT_INTERP+NEEDED+JMPREL+GLOB_DAT/JUMP_SLOT 且无 GNU_HASH;libt2.so 有 SONAME+HASH、无 NEEDED;**dyn_link_missing 的 NEEDED 精确等于 `/lib/libabsent.so`**(防 vacuous pass)。
- ABI 门槛夹具:同源 `-DT2_GNUABI` → `dyn_link_gnuabi.elf`(内嵌 NT_GNU_ABI_TAG/OS_LINUX note 汇编段,其余同 dyn_link)。**验收断言真实错误而非笼统 127**:libc `exec()` 失败时返回负 errno(execve.c:11-14),child 改为 `int64_t r = exec(...); if (r == -ENOEXEC) _exit(126); _exit(127);` → systest 断言 rc=126(其他失败仍是 127,exec 成功是 0——note 缺失或拒绝点接错都会被区分出来);形状门另确认 GNU note 真的位于 **PT_NOTE** 内(name `GNU`、type `NT_GNU_ABI_TAG`、desc os=Linux,否则夹具根本不触发 elf_detect_abi)。
- 动态表扫描负例:构建期从第三个 stub(SONAME `/lib/libbad_dyn.so`,同接口,链接期供 `dyn_link_baddyn` 变体解析)产出**二进制补丁副本**——按 PT_DYNAMIC 声明范围覆写动态区为重复的良性已知 tag(如 DT_DEBUG/0),使声明的动态区间内**始终无 DT_NULL**;stage 的是补丁副本,原件不 stage。ld.so 扫描触及条目上限仍未见 DT_NULL → exit 98;systest 断言 rc=**98**(区分预期拒绝与缺页死亡:死于信号时 `run_user_image` 返回 -2,T1 helper 已区分);形状门解析补丁副本,断言其声明动态区内确无 DT_NULL(防补丁步骤失效的 vacuous pass)。
- systest 新用例:`fork/exec dyn_link` → rc=0 且串口 [LDSO-ENTER]/[LDSO-RELOC]/[DYN-T2] 顺序出现;`dyn_link_missing` → rc=95;`dyn_link_gnuabi` → rc=126;`dyn_link_baddyn` → rc=98;T1 用例(dyn_exec 两 marker)回归不变。
- hosttests:`test_elf_load_4k.c` 追加用例——interp-main 接受 NEEDED/JMPREL、GLOB_DAT/JUMP_SLOT 记录跳过不应用、RELATIVE 仍应用、**RELACOUNT 前缀语义**(count ≤ 表长且前 count 条全 RELATIVE;负例:总数相同但前缀错位 → -ENOEXEC)、**符号型 addend≠0 → -ENOEXEC**;静态 PIE 路径 **DT_NEEDED 仍拒**。注意:现有内核白名单本就接受单个 `DT_GNU_HASH`,静态 PIE 路径**不新增** GNU-hash 拒绝用例;GNU-hash-only 镜像的拒绝落在 **ld.so 输入校验层**(exit 98,夹具/单测覆盖),内核白名单原状。ABI 门槛在 **exec 层**(elf_load 之外),走 systest 夹具覆盖,不进本文件用例。
- 镜像分发:`mk/components/image.mk` 的 `ROOTFS_MANIFEST_DEPS` 与 `config/rootfs.mk` 安装项补齐全部新增 **staged** 产物(ldso.elf 换源、dyn_link.elf、dyn_link_missing.elf、dyn_link_gnuabi.elf、dyn_link_baddyn.elf、libt2.so、libbad_dyn.so 补丁副本;libabsent stub 与 libbad_dyn 原件只是链接期输入,**只构建不 stage**;T1 教训:漏列 → staging 陈旧,QEMU 跑旧 /lib/ld.so——含上轮漏掉的 gnuabi 夹具)。

## 验收与证据

| 层 | 通过条件 |
| --- | --- |
| 宿主形状契约 | 新 checker 断言全绿;现有 checker 不回退 |
| 内核回归 | `make test-host` / `make test-static` 全绿;T1 六用例不变 |
| QEMU 运行证据 | systest: dyn_link rc=0 且三 marker 顺序出现;missing rc=95;gnuabi rc=126(child 内断言 exec 返回 -ENOEXEC,127 只留给其他失败);baddyn rc=98(有界动态表扫描拒绝,非信号死亡) |
| 不变量 | 静态 PIE 镜像 auxv 字节不变;无 interp 路径(ET_EXEC/静态 PIE)行为零变化 |

## 留给实施者裁决

- mmap span 内段间空洞页保持 anon RW(不 PROT_NONE)——已知债务,T3 走 file-backed 映射时消失;
- ld.so 内部代码布局(user/ldso/ 单文件 vs 多文件)与 marker 细节命名;
- read 短读/EINTR 处理(当前内核无信号中断 read,预计只处理短读);
- `t2_lib.c` 是否需要额外 `-fvisibility` 微调以保证 RELATIVE 记录实际出现(宿主实验已证函数指针全局可行)。

roadmap T2 行与 changelog 在闭环时同步。

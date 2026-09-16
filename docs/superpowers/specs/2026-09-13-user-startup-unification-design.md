# 统一用户态进程启动方式（user startup unification）

日期：2026-09-13
状态：v4（plan v1 评审同步：libc envp 有界 walk 允许 128 项合法环境——终止槽在 index 128；§6.2 增补 envc=128 边界组与 getenv 精确值要求）
范围：`kernel/sched/task.c`（两处用户栈构造）、`user/crt0.S`、`libc/csu/csu.c`、`config/busybox.overlay/applets/crt0.S`、`user/systest.c`

## 1. 背景与动机

OS01 当前用户态进程启动存在三重不统一：

1. **内核双站点**：`spawn_user_task()`（task.c:1114，argv-only）与 `sys_exec()`（task.c:1328，argc+envc）各自维护一份用户栈构造代码（字符串降序拷贝、16 字节对齐、meta_pad 奇偶、argc→argv[]→envp[]→auxv 布局），两份逻辑已经漂移（一处支持 envp、一处不支持）。
2. **寄存器/栈双轨**：内核既构造完整 SysV 栈布局（argc/argv/envp/auxv），又通过 `regs->rdi/rsi/rdx` 把 argc/argv/envp 走寄存器再传一份；`crt0.S` 只消费寄存器，栈上的 auxv 布局无人解析（`_start` 处只压了 `AT_NULL`）。
3. **`__libc_start_main` 从未被调用**：`libc/csu/csu.c` 有完整签名但 `crt0.S` 直接 `call main`，`environ`（`libc/stdlib/environ.c:8` 恒为 NULL）从未从栈初始化。

**为什么现在做**：它是 P1 后续（AT_RANDOM/用户栈 canary/ASLR）与 P5 动态链接的前置——musl 的 `crt1.o` 只认栈布局不认寄存器；内核侧每个新 auxv 条目在双站点下成本 ×2。本任务完成后，AT_RANDOM 是「单站点 +10 行 + `__libc_start_main` 读 `__libc_auxv`」的量级（见 roadmap P1）。

## 2. 目标 / 非目标

**目标**：
- 内核单点构造用户栈（一处 auxv 逻辑），布局与现状**语义相同**，允许的有意偏差见 §5。
- `argv == NULL` 也是完整启动契约的一部分：helper 一律构造最小完整布局（argc=0、argv[0]=NULL、envp 终止、auxv `AT_NULL`），两个调用点**始终使用 helper 返回的 rsp**——消除现「NULL argv 直接以 `USER_STACK_TOP` 进用户态」的特殊分支（PID 1 `/bin/init` 正是这条路径，task.c:2181）。
- `crt0.S` 改为标准 SysV `_start`：从 `[rsp]` 解析 argc/argv，调用 `__libc_start_main`。
- `__libc_start_main` 真正启用：设置 `environ`，解析并保存 auxv 起点到全局 `__libc_auxv`（`uint64_t *`），保存 `stack_end` 到全局 `__libc_stack_end`（测试观测通道），再 `call main`。
- busybox overlay crt0 同步（与 `user/crt0.S` 保持逐字节相同）。

**非目标**（明确不做）：
- 不新增任何 auxv 条目（AT_RANDOM 属于后续 canary 任务）。
- 不移除内核 `regs->rdi/rsi/rdx` 寄存器设置（兼容期保留，冗余无害，后续任务再删）。
- 不改 ELF 加载、栈大小/位置（`USER_STACK_BASE 0x800000` / `USER_STACK_TOP` 不动）、信号帧、sigreturn trampoline。
- 不引入 init/fini 数组遍历（libc-free，无 `.init_array` 运行时支持——与 `SUBSYS_INITCALL()` 设计决策同理，见 `decisions.md`）。

## 3. 现状快照（引用点）

| 事实 | 位置 |
|---|---|
| spawn 站点：argv 字符串降序拷贝 → `&= ~15` → `meta_pad=(argc&1)?0:8` → auxv{0,0} → envp NULL+pad → argv[]+NULL → argc；**`argv==NULL` 时整个构造跳过，`regs->rsp = USER_STACK_TOP`**（:1283）；`regs->rdx=0` | `kernel/sched/task.c` `spawn_user_task()` 内 ~1226–1287 |
| exec 站点：argv+envp 字符串（`str_offset[128]` 共享上限）→ 同上布局 → `regs->rdx=user_env_ptr`；同样有 `argv==NULL → USER_STACK_TOP` 分支（:1508） | `kernel/sched/task.c` `sys_exec()` 内 ~1410–1513 |
| **PID 1 以 NULL argv 启动**：`spawn_user_task("/bin/init", NULL)`——旧 crt0 靠 rdi/rsi/rdx=0 工作 | `kernel/sched/task.c:2181` |
| **padding 现状**：`user_rsp -= 8 + meta_pad; *rsp=0`——pad 8 字节位于 envp-NULL **之上**、auxv **之下**，且**未显式写零**；walk 过 envp NULL 后 `ep+1` 命中的是 pad 不是 auxv（`meta_pad>0`，即 argc+envc 为偶数时） | 同上两站点布局代码 |
| **argv+envp 合计上限已在 syscall 边界强制**：`ac+ec > 128 → -E2BIG`（`deep_copy_argv` 单数组另限 MAX_ARGV=128） | `kernel/arch/x86_64/trap.c:1406–1420` |
| crt0：rbp=0 → r12/r13/r14 存 rdi/rsi/rdx → 清 BSS → `call main(rdi,rsi,rdx)` → SYS_exit | `user/crt0.S` |
| `__libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)` 存在但只 `return main(argc, argv, environ)` | `libc/csu/csu.c` |
| `environ = NULL` 默认值；`getenv` 依赖它；`execvp` 在 `PATH` 缺失时 fallback `/bin` | `libc/stdlib/environ.c`、`libc/unistd/execve.c` |
| busybox overlay crt0 与 `user/crt0.S` 逐字节相同（`diff` 验证） | `config/busybox.overlay/applets/crt0.S` |

## 4. 设计

### 4.1 内核：共享栈构造器

`kernel/sched/task.c` 新增 file-static helper（签名示意）：

```c
/* 从 USER_STACK_TOP 向下构造 SysV 初始栈。返回 0 成功（*out_rsp 指向 argc），
 * -E2BIG 当 argv+envp 合计超过 STR_OFFSET_MAX=128（防御下沉：trap.c:1406
 * 已在 syscall 边界拦截 exec 路径，此处覆盖不经 trap.c 的 spawn 调用方）。
 * argv/envp 均可为 NULL：NULL 时按空数组处理，仍构造完整布局。 */
static int setup_user_stack(uint8_t *kstack,          /* Phy_To_Virt(stack_page) */
                            char *const argv[],
                            char *const envp[],        /* 可 NULL */
                            int *out_argc,
                            uint64_t *out_argv_ptr,    /* argv[] 数组地址 */
                            uint64_t *out_envp_ptr,    /* envp[] 地址 */
                            uint64_t *out_rsp);
```

- 两个调用点替换各自的内联构造；`spawn_user_task` 传 `envp=NULL`，`sys_exec` 传真实 envp；**两处都删除 `argv==NULL → USER_STACK_TOP` 特殊分支，一律使用 `*out_rsp`**。
- **新布局（低地址 → 高地址）**：

  ```
  argc | argv[0..argc-1], NULL | envp[0..envc-1], NULL | auxv {AT_NULL,0} | align_pad(0 或 8，显式清零)
  ```

  与现状的唯一结构差异：**align_pad 从「envp-NULL 与 auxv 之间」迁移到「auxv 之上（字符串区之下）」并显式清零**——保证 envp 的终止 NULL 与 auxv 起点严格相邻，`__libc_start_main` 的 walk 才不会错位。`meta_pad` 奇偶规则本身不变（保持 rsp 16 对齐）。
- 出口断言 `ASSERT((*out_rsp & 0xF) == 0)`。

### 4.2 用户态：crt0.S 标准 SysV `_start`

```asm
_start:
    xorq %rbp, %rbp            # backtrace 终止
    # 清 BSS（保持在 call 之前）
    ...
    movq %rsp, %rax           # BSS 清零后保存入口 rsp，此前不得修改 rsp
    # __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
    # 寄存器映射（SysV）：rdi=main  rsi=argc  rdx=argv
    #                     rcx=init(0)  r8=fini(0)  r9=rtld_fini(0)
    # 第 7 参 stack_end 走栈
    movq (%rsp), %rsi          # argc  ← 从栈读（不再依赖内核寄存器）
    leaq 8(%rsp), %rdx         # argv
    leaq main(%rip), %rdi      # main
    xorl %ecx, %ecx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    subq $16, %rsp             # 对齐占位 + 第 7 参槽位
    movq %rax, (%rsp)          # 第 7 参：准确的入口 rsp
    call __libc_start_main
    addq $16, %rsp
    movl %eax, %edi            # main 返回值
    movl $2, %eax              # SYS_exit
    int $0x80
```

实现注意：入口 `%rsp` 16 对齐，`subq $16` 后仍 16 对齐，`call` 压返回地址后 `__libc_start_main` 内 `%rsp % 16 == 8`（SysV 不变式）。`stack_end` 必须在 **BSS 清零之后、任何 `rsp` 修改之前**通过 `movq %rsp, %rax` 保存，且随后直到写入参数槽不得覆盖 rax；现有 BSS 清零使用 `xorb %al, %al`，会破坏提前保存到 rax 的地址。汇编细节以「入口及 call 指令执行前 rsp%16==0 → 被调函数入口 rsp%16==8」为不变式自检。

### 4.3 libc：`__libc_start_main` 真正启用

`libc/csu/csu.c`：

```c
uint64_t *__libc_auxv;        /* auxv 起点指针（指向第一个条目而非 AT_NULL 终止项），
                                 AT_NULL 终止；后续 getauxval/AT_RANDOM 消费点 */
void    *__libc_stack_end;    /* _start 入口 rsp——测试观测通道（对齐/地址关系断言） */

int __libc_start_main(int (*main)(int, char **, char **), int argc, char **argv,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end)
{
    (void)init; (void)fini; (void)rtld_fini;
    __libc_stack_end = stack_end;
    /* argv[argc]==NULL 之后是 envp[]；其终止 NULL 之后**紧邻** auxv（见 §4.1 pad 迁移） */
    char **ep = argv + argc + 1;
    environ = ep;
    /* Bounded walk: 读 ep[0..128]——128 项 envp 合法（内核上限是 argc+envc 合计
     * 128，其终止 NULL 落在 index 128）；只有 0..128 内无终止 NULL 才是错误。 */
    int env_i;
    for (env_i = 0; env_i <= 128 && ep[env_i] != NULL; env_i++);
    if (env_i > 128) {                    /* envp 缺终止 NULL：内核布局 bug，放弃 auxv 但让进程可活 */
        __libc_auxv = NULL;
        return main(argc, argv, environ);
    }
    uint64_t *av = (uint64_t *)(ep + env_i + 1);
    __libc_auxv = av;                  /* 保存起点 */
    for (int i = 0; i < 64; i++) {     /* 保守上限，防内核布局 bug 变死循环 */
        if (av[2 * i] == 0) goto found;
    }
    __libc_auxv = NULL;                /* 64 对内未见 AT_NULL：布局异常，弃用 */
found:
    return main(argc, argv, environ);
}
```

- envp walk 与 auxv walk 均有界：无界 envp walk 最坏会越过 `USER_STACK_TOP` 进未映射页 fault，到不了 auxv 保护；超限时放弃 `__libc_auxv` 但照常进 main（environ 已不可信，测试可报告而非崩溃）。

- auxv walk 加保守上限（如 64 对）防内核布局 bug 变用户态死循环；超限时 `__libc_auxv = NULL`。
- `getauxval` 不在本任务公开（YAGNI，Task 2 按需加）。

### 4.4 busybox overlay

`config/busybox.overlay/applets/crt0.S` 同步为新 `user/crt0.S`（维持两文件 `diff` 逐字节相同的既有约定）；busybox input digest 覆盖 overlay，自动触发重编。

### 4.5 兼容期

内核 `regs->rdi/rsi/rdx` 设置保留，但**不再作为兼容承诺**——crt0 与内核在同一构建中原子更新（crt0 是每个用户程序的组成部分，同次 `make` 重链），不存在「新 crt0 + 旧内核」的可运行组合（NULL argv 启动在新旧 crt0 间行为不同：旧的读寄存器=0，新的读栈上 argc=0——旧内核下后者读到 `USER_STACK_TOP` 未初始化内存）。保留寄存器设置只是调试可观察性（GDB 断点在 `_start` 时寄存器里有 argc/argv）。

## 5. 行为变化（验证重点，均为有意变更）

| # | 变化 | 影响 |
|---|---|---|
| 1 | **NULL argv 不再走 `USER_STACK_TOP` 特殊分支**：一律构造完整最小布局（argc=0、argv[0]=NULL、envp 终止、auxv `AT_NULL`） | PID 1 `/bin/init`（task.c:2181）从 `main(0, NULL, NULL)` 变为 `main(0, {NULL}, {NULL, AT_NULL...})`——语义等价且更安全；init.c 现不消费 argv |
| 2 | **align_pad 位置迁移**：envp-NULL/auxv 之间 → auxv 之上，并显式清零 | 结构性修正：auxv walk 不再依赖 pad 恰好为零；对于原来已构造完整栈的相同参数，仅迁移 padding 不改变元数据总大小，最终 rsp 不变；auxv 起点在有 padding 时下移 8 字节（无 padding 时不变） |
| 3 | `environ` 从 NULL 变为真实 envp | `getenv` 开始返回真值；`execvp` 的 `PATH` 搜索从 fallback `/bin` 变为真实 PATH（若父进程传了）。busybox applet 验证必须覆盖 PATH 相关命令 |
| 4 | helper 内 `argv+envp > 128 → -E2BIG`（spawn 路径新增；exec 路径 trap.c:1406 已有，helper 为防御下沉） | 仅极端参数可见 |
| 5 | crt0 调 `__libc_start_main` 而非直接 `call main` | main 返回值语义不变；多一层 C 函数 |

## 6. 测试计划

观测通道：`__libc_stack_end` / `__libc_auxv` 两个新全局（csu.c 定义，systest `extern` 声明使用）。

1. **新增 systest case（RED 先行，编号顺延）**——`test_startup_layout()`，在任何环境修改前验证入口布局，供正常入口及自再入子进程复用：
   - `__libc_stack_end != NULL`，且 `((uintptr_t)__libc_stack_end & 0xF) == 0`；
   - `argv == (char **)((uint64_t *)__libc_stack_end + 1)`，且入口栈上的 argc 与 main 收到的 argc 一致；
   - 地址关系链：`argv + argc + 1 == environ`，且 `__libc_auxv == (uint64_t *)(environ 终止 NULL + 1)`；
   - 先检查 `__libc_auxv != NULL`，再检查 `(uintptr_t)__libc_auxv > (uintptr_t)__libc_stack_end`；当前仅有 AT_NULL，精确断言 `__libc_auxv[0] == 0 && __libc_auxv[1] == 0`；
   - 按本用例预期数量检查 argv/envp，验证字符串精确值及两个数组的终止 NULL，避免无界扫描；argc=0 时不读取 argv[0] 指向的字符串。
2. **fork+exec 自再入矩阵**：统一执行实际安装路径 `/bin/systest`。使用专用 argv[0] `startup-probe` 标识 argc=1/2 的探针分支；argc=2 时第二项为 `--envcheck`。argc=0 直接进入空参数探针分支。所有探针均在正常测试调度前分流，只执行入口检查并 `_exit`，不再次运行全套测试或递归 fork。父进程逐例 fork，检查 waitpid、正常退出和退出码；execve 返回即为失败，以独立非零退出码报告。

   | argc | argv 实参 | envc | envp 实参 | argc+envc 奇偶 / pad |
   |---|---|---|---|---|
   | 0 | `{NULL}` | 0 | `NULL` | 偶 / 8 |
   | 0 | `{NULL}` | 1 | `{"OS01_TEST_K=veRy42", NULL}` | 奇 / 0 |
   | 1 | `{"startup-probe", NULL}` | 0 | `NULL` | 奇 / 0 |
   | 1 | `{"startup-probe", NULL}` | 1 | `{"OS01_TEST_K=veRy42", NULL}` | 偶 / 8 |
   | 2 | `{"startup-probe", "--envcheck", NULL}` | 0 | `NULL` | 偶 / 8 |
   | 2 | `{"startup-probe", "--envcheck", NULL}` | 1 | `{"OS01_TEST_K=veRy42", NULL}` | 奇 / 0 |

   六组均执行 §6.1 的地址关系检查。envc=1 时断言 environ 恰好一项，且 `getenv("OS01_TEST_K")` 逐字节等于 `veRy42`（getenv 必须真实消费 environ，仅比对原始字符串不充分）；envc=0 时断言 environ 首项为 NULL，且 getenv 返回 NULL。探针成功时以 `10 + argc * 2 + envc` 作为退出码编码实际观测到的数量，失败统一退出 99；父进程按本组预期 argc/envc 核对退出码，避免环境丢失后被空环境分支误判为通过。子进程校验实际环境只能为上述两种形态。
   **边界组（第七组）**：argc=0 + envc=128（合计恰为内核上限）——libc 的有界 envp walk 必须接受第 128 项之后的终止槽并仍能找到 auxv（128 项 envp 合法，见 §4.3 修订）；探针退出码 138。
3. **execvp PATH 用例**：`mkdir /pathtest` + `symlink /bin/systest → /pathtest/startup-probe`，子进程设 `environ = {"PATH=/pathtest", NULL}` 后 `execvp("startup-probe", {"startup-probe","--pathcheck",NULL})`——探针被找到即证明 execvp 真实解析了传入的非默认 PATH（fallback `/bin` 下不存在该名字，只能 exit 127）；探针核验 env 恰为 `PATH=/pathtest` 且 `getenv("PATH")` 精确匹配，成功退出码 **20**。显示 PATH / 运行 which 不能作为该行为的证明。
3. **NULL argv 与 spawn 单独覆盖**：
   - 在六组矩阵之外，另执行 `execve("/bin/systest", NULL, NULL)` 和 `execve("/bin/systest", NULL, {"OS01_TEST_K=veRy42", NULL})`，复用 argc=0 探针，分别检查空环境及非空环境。NULL 指针与 `{NULL}` 数组必须分别测试。
   - 正常内核启动继续使用 `spawn_user_task("/bin/init", NULL)`，验证 PID 1 成功进入 main、完成四阶段引导并进入 supervision loop，且能启动 systest/交互终端。此项验证 spawn 调用点确实采用 helper 返回的 rsp；exec 自再入不能替代此项。精确布局断言由上述探针覆盖，PID 1 启动由 QEMU 启动日志和后续子进程运行结果验证。
4. **回归**：通过 `make OS01_SYSTEST=1 test-syscall` 运行 systest 全量（含新增用例，不设置 `KERNEL_SELFTEST=1`）；另跑正常启动、busybox 52 applet（重点 PATH/环境变量消费型：sh、env、exec 类）、sigtest、nettest。
5. hosttests：无新增（纯内核/用户态运行时行为）。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 对齐奇偶规则重构走样（meta_pad 与 argc/envc 奇偶耦合） | 单点实现 + `ASSERT(rsp&0xF==0)` 出口断言 + systest 六组奇偶矩阵（argc 0/1/2 × envc 0/1） |
| pad 迁移后 walk 仍错位 | 测试计划 §6.1/6.2 的地址关系链断言精确到 `environ NULL + 1 == __libc_auxv`，pad 任何残留都会被抓 |
| 两份 crt0.S 副本漂移 | spec 明确 diff-identical 约定；实现 task 里包含 `diff user/crt0.S config/busybox.overlay/applets/crt0.S` 验证步 |
| 7 参调用的栈对齐错误只在高优化度下炸 | crt0 手写汇编最小化；systest 第一个跑的 case 就是入口自检 |
| `environ` 变真后 busybox 行为变化 | applet 验证全过 + 显式 PATH 用例 |
| PID 1 NULL argv 新布局引入回归 | §6.3 独立 NULL argv exec 用例 + PID 1 spawn 引导验证 |

## 8. 后续（不属于本任务）

- AT_RANDOM + 用户栈 canary + `LWIP_RAND` 换熵池（P1，依赖本任务）
- 移除 `regs->rdi/rsi/rdx` 寄存器传参
- `getauxval()` 公开、`AT_PAGESZ`/`AT_PHDR`/`AT_EXECFN`（ASLR / 动态链接时）

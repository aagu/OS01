# 统一用户态进程启动方式（user startup unification）

日期：2026-09-13
状态：v2（评审后修订：NULL argv 启动契约 / padding 迁移到 auxv 之后 / auxv 起点指针 / 观测通道 / E2BIG 现状纠正）
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
    # __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
    # 寄存器映射（SysV）：rdi=main  rsi=argc  rdx=argv
    #                     rcx=init(0)  r8=fini(0)  r9=rtld_fini(0)
    # 第 7 参 stack_end 走栈
    movq (%rsp), %rsi          # argc  ← 从栈读（不再依赖内核寄存器）
    leaq 8(%rsp), %rdx         # argv
    leaq main(%rip), %rdi      # main
    xorq %ecx, %ecx
    xorq %r8d, %r8d
    xorq %r9d, %r9d
    subq $16, %rsp             # 对齐占位 + 第 7 参槽位
    movq %rax, (%rsp)          # 进入前 movq 入口_rsp, %rax 保存的 stack_end
    call __libc_start_main
    addq $16, %rsp
    movl %eax, %edi            # main 返回值
    movl $2, %eax              # SYS_exit
    int $0x80
```

实现注意：入口 `%rsp` 16 对齐，`subq $16` 后仍 16 对齐，`call` 压返回地址后 `__libc_start_main` 内 `%rsp % 16 == 8`（SysV 不变式）。`stack_end` 必须在任何 `rsp` 修改前保存（如先 `movq %rsp, %rax`）。汇编细节以「入口 rsp 16 对齐 → call 瞬间 rsp%16==8」为不变式自检。

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
    while (*ep != NULL) ep++;          /* envp 终止 */
    uint64_t *av = (uint64_t *)(ep + 1);
    __libc_auxv = av;                  /* 保存起点 */
    for (int i = 0; i < 64; i++) {     /* 保守上限，防内核布局 bug 变死循环 */
        if (av[2 * i] == 0) goto found;
    }
    __libc_auxv = NULL;                /* 64 对内未见 AT_NULL：布局异常，弃用 */
found:
    return main(argc, argv, environ);
}
```

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
| 2 | **align_pad 位置迁移**：envp-NULL/auxv 之间 → auxv 之上，并显式清零 | 结构性修正：auxv walk 不再依赖 pad 恰好为零；字节偏移与旧布局不同（同类 argc/envp 下 rsp 差 0 或 8 字节） |
| 3 | `environ` 从 NULL 变为真实 envp | `getenv` 开始返回真值；`execvp` 的 `PATH` 搜索从 fallback `/bin` 变为真实 PATH（若父进程传了）。busybox applet 验证必须覆盖 PATH 相关命令 |
| 4 | helper 内 `argv+envp > 128 → -E2BIG`（spawn 路径新增；exec 路径 trap.c:1406 已有，helper 为防御下沉） | 仅极端参数可见 |
| 5 | crt0 调 `__libc_start_main` 而非直接 `call main` | main 返回值语义不变；多一层 C 函数 |

## 6. 测试计划

观测通道：`__libc_stack_end` / `__libc_auxv` 两个新全局（csu.c 定义，systest `extern` 声明使用）。

1. **新增 systest case（RED 先行，编号顺延）**——`test_startup_layout()`，直接验证本进程入口：
   - `((uint64_t)__libc_stack_end & 0xF) == 0`（入口 rsp 对齐）；
   - 地址关系链：`argv + argc + 1 == environ`，且 `__libc_auxv == (uint64_t *)(environ 终止 NULL + 1)`（**同时覆盖 argc/envc 奇偶两种组合**，pad 存在与否都成立）；
   - `__libc_auxv != NULL`，且在 64 对内存在 `type==0`（AT_NULL）；`__libc_auxv < __libc_stack_end`；
   - argv 每项非空可读、`argv[argc]==NULL`；envp 每项含 `=`。
2. **fork+exec 自再入用例**：systest fork 后 `execve("/systest.elf", {"systest", "--envcheck", NULL}, {"OS01_TEST_K=veRy42", NULL})`；子进程 main 入口检测 `--envcheck` 走专用分支：断言 `getenv("OS01_TEST_K")` 逐字节等于 `veRy42`、environ 恰好 1 项、地址关系链同上，exit 码报告结果。覆盖 exec 站点 + 非空 envp 精确值 + envc=1（奇偶另一侧）。
3. **argc==0 用例**：spawn 路径 `argv==NULL` 的入口自检（PID 1 同款路径）——通过 nettest/systest 启动序或直接以 `execve(path, {NULL}, NULL)` 再入自检 argc==0 时布局链成立。
4. **回归**：systest 全量（含新增 case）、busybox 启动 + 52 applet（重点 PATH/环境变量消费型：sh、env、exec 类）、sigtest、nettest。
5. hosttests：无新增（纯内核/用户态运行时行为）。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 对齐奇偶规则重构走样（meta_pad 与 argc/envc 奇偶耦合） | 单点实现 + `ASSERT(rsp&0xF==0)` 出口断言 + systest 奇偶矩阵（argc 0/1/2 × envp 0/1） |
| pad 迁移后 walk 仍错位 | 测试计划 §6.1/6.2 的地址关系链断言精确到 `environ NULL + 1 == __libc_auxv`，pad 任何残留都会被抓 |
| 两份 crt0.S 副本漂移 | spec 明确 diff-identical 约定；实现 task 里包含 `diff user/crt0.S config/busybox.overlay/applets/crt0.S` 验证步 |
| 7 参调用的栈对齐错误只在高优化度下炸 | crt0 手写汇编最小化；systest 第一个跑的 case 就是入口自检 |
| `environ` 变真后 busybox 行为变化 | applet 验证全过 + 显式 PATH 用例 |
| PID 1 NULL argv 新布局引入回归 | §6.3 argc==0 用例 + init 引导 4 阶段全过 |

## 8. 后续（不属于本任务）

- AT_RANDOM + 用户栈 canary + `LWIP_RAND` 换熵池（P1，依赖本任务）
- 移除 `regs->rdi/rsi/rdx` 寄存器传参
- `getauxval()` 公开、`AT_PAGESZ`/`AT_PHDR`/`AT_EXECFN`（ASLR / 动态链接时）

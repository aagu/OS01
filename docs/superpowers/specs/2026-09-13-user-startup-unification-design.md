# 统一用户态进程启动方式（user startup unification）

日期：2026-09-13
状态：待复审
范围：`kernel/sched/task.c`（两处用户栈构造）、`user/crt0.S`、`libc/csu/csu.c`、`config/busybox.overlay/applets/crt0.S`、`user/systest.c`

## 1. 背景与动机

OS01 当前用户态进程启动存在三重不统一：

1. **内核双站点**：`spawn_user_task()`（task.c:1114，argv-only）与 `sys_exec()`（task.c:1328，argc+envc）各自维护一份用户栈构造代码（字符串降序拷贝、16 字节对齐、meta_pad 奇偶、argc→argv[]→envp[]→auxv 布局），两份逻辑已经漂移（一处支持 envp、一处不支持）。
2. **寄存器/栈双轨**：内核既构造完整 SysV 栈布局（argc/argv/envp/auxv），又通过 `regs->rdi/rsi/rdx` 把 argc/argv/envp 走寄存器再传一份；`crt0.S` 只消费寄存器，栈上的 auxv 布局无人解析（`_start` 处只压了 `AT_NULL`）。
3. **`__libc_start_main` 从未被调用**：`libc/csu/csu.c` 有完整签名但 `crt0.S` 直接 `call main`，`environ`（`libc/stdlib/environ.c:8` 恒为 NULL）从未从栈初始化。

**为什么现在做**：它是 P1 后续（AT_RANDOM/用户栈 canary/ASLR）与 P5 动态链接的前置——musl 的 `crt1.o` 只认栈布局不认寄存器；内核侧每个新 auxv 条目在双站点下成本 ×2。本任务完成后，AT_RANDOM 是「单站点 +10 行 + `__libc_start_main` 读 `__libc_auxv`」的量级（见 roadmap P1）。

## 2. 目标 / 非目标

**目标**：
- 内核单点构造用户栈（一处 auxv 逻辑），布局与现状**逐字节相同**。
- `crt0.S` 改为标准 SysV `_start`：从 `[rsp]` 解析 argc/argv，调用 `__libc_start_main`。
- `__libc_start_main` 真正启用：设置 `environ`，解析并保存 auxv 到全局 `__libc_auxv`，再 `call main`。
- busybox overlay crt0 同步（与 `user/crt0.S` 保持逐字节相同）。

**非目标**（明确不做）：
- 不新增任何 auxv 条目（AT_RANDOM 属于后续 canary 任务）。
- 不移除内核 `regs->rdi/rsi/rdx` 寄存器设置（兼容期保留，冗余无害，后续任务再删）。
- 不改 ELF 加载、栈大小/位置（`USER_STACK_BASE 0x800000` / `USER_STACK_TOP` 不动）、信号帧、sigreturn trampoline。
- 不引入 init/fini 数组遍历（libc-free，无 `.init_array` 运行时支持——与 `SUBSYS_INITCALL()` 设计决策同理，见 `decisions.md`）。

## 3. 现状快照（引用点）

| 事实 | 位置 |
|---|---|
| spawn 站点：argv 字符串降序拷贝 → `&= ~15` → `meta_pad=(argc&1)?0:8` → auxv{0,0} → envp NULL+pad → argv[]+NULL → argc；`regs->rdx=0` | `kernel/sched/task.c` `spawn_user_task()` 内 ~1226–1287 |
| exec 站点：argv+envp 字符串（`str_offset[128]` 共享上限）→ 同上布局 → `regs->rdx=user_env_ptr` | `kernel/sched/task.c` `sys_exec()` 内 ~1410–1513 |
| crt0：rbp=0 → r12/r13/r14 存 rdi/rsi/rdx → 清 BSS → `call main(rdi,rsi,rdx)` → SYS_exit | `user/crt0.S` |
| `__libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)` 存在但只 `return main(argc, argv, environ)` | `libc/csu/csu.c` |
| `environ = NULL` 默认值；`getenv` 依赖它；`execvp` 在 `PATH` 缺失时 fallback `/bin` | `libc/stdlib/environ.c`、`libc/unistd/execve.c` |
| busybox overlay crt0 与 `user/crt0.S` 逐字节相同（`diff` 验证） | `config/busybox.overlay/applets/crt0.S` |

## 4. 设计

### 4.1 内核：共享栈构造器

`kernel/sched/task.c` 新增 file-static helper（签名示意）：

```c
/* 从 USER_STACK_TOP 向下构造 SysV 初始栈。返回 0 成功（*out_rsp 指向 argc），
 * -E2BIG 当 argv+envp 合计超过 STR_OFFSET_MAX。envp==NULL 时跳过 envp 段
 * （spawn 站点现状）。布局与重构前逐字节相同。 */
static int setup_user_stack(uint8_t *kstack,          /* Phy_To_Virt(stack_page) */
                            char *const argv[],
                            char *const envp[],        /* 可 NULL */
                            int *out_argc,
                            uint64_t *out_argv_ptr,    /* argv[] 数组地址 */
                            uint64_t *out_envp_ptr,    /* envp[] 地址（envp==NULL 时 *out=0） */
                            uint64_t *out_rsp);
```

- 两个调用点替换各自的内联构造；`spawn_user_task` 传 `envp=NULL`，`sys_exec` 传真实 envp。
- 布局规则原样合并：字符串降序拷贝 → `rsp &= ~15` → `meta_pad = ((argc+envc) & 1) ? 0 : 8`（envp==NULL 时 envc=0，与 spawn 现状一致）→ auxv `AT_NULL` 一对 → envp NULL + pad → argv[] + NULL → argc。
- 新增 `ASSERT((rsp & 0xF) == 0)` 出口断言。
- `str_offset[128]` 上限语义保持：exec 站点是 argv+envp 共享 128；统一后 helper 内保持同一数组同一上限，超限行为（现状是越界写）改为返回 `-E2BIG`——这是唯一一处**有意的行为修正**（现状是静默栈溢出 bug）。

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
void **__libc_auxv;            /* 新全局：auxv 对（AT_NULL 终止），后续 getauxval/AT_RANDOM 消费点 */

int __libc_start_main(int (*main)(int, char **, char **), int argc, char **argv,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end)
{
    (void)init; (void)fini; (void)rtld_fini; (void)stack_end;
    /* argv[argc]==NULL 之后是 envp[]；走完 envp 的终止 NULL 之后是 auxv 对 */
    char **ep = argv + argc + 1;
    environ = ep;
    while (*ep != NULL) ep++;          /* envp 终止 */
    uint64_t *av = (uint64_t *)(ep + 1);
    for (int i = 0; i < 64; i++) {     /* 保守上限，防内核布局 bug 变死循环 */
        if (av[2 * i] == 0) { __libc_auxv = (void **)&av[2 * i]; goto found; }
    }
    __libc_auxv = NULL;
found:
    return main(argc, argv, environ);
}
```

- auxv walk 加保守上限（如 64 对）防内核布局 bug 变用户态死循环；超限时 `__libc_auxv = NULL`。
- `getauxval` 不在本任务公开（YAGNI，Task 2 按需加）。

### 4.4 busybox overlay

`config/busybox.overlay/applets/crt0.S` 同步为新 `user/crt0.S`（维持两文件 `diff` 逐字节相同的既有约定）；busybox input digest 覆盖 overlay，自动触发重编。

### 4.5 兼容期

内核 `regs->rdi/rsi/rdx` 设置保留。crt0 不再读它们，但保留可让「旧 crt0 + 新内核」「新 crt0 + 旧内核」都能跑，降低 bisect 半径。

## 5. 行为变化（验证重点）

| 变化 | 影响 |
|---|---|
| `environ` 从 NULL 变为真实 envp | `getenv` 开始返回真值；`execvp` 的 `PATH` 搜索从 fallback `/bin` 变为真实 PATH（若父进程传了）。busybox applet 验证必须覆盖 PATH 相关命令 |
| `str_offset` 超限：静默越界 → `-E2BIG` | 正向修正，仅极端 argv/envp（合计 >128）可见 |
| crt0 调 `__libc_start_main` 而非直接 `call main` | main 返回值语义不变；多一层 C 函数 |

## 6. 测试计划

1. **新增 systest case（RED 先行）**：入口自检——① argc>0 且与 argv 字符串数一致；② argv[0] 非空可读、argv[argc]==NULL；③ envp 以 NULL 终止且每项 `K=V` 形态；④ 走到 auxv 后能在 64 对内命中 `AT_NULL`；⑤ `__libc_auxv` 全局非 NULL 且首对 type==0。
2. **回归**：systest 268/268（含新增 case）；busybox 启动 + 52 applet（重点 PATH/环境变量消费型：sh、env、exec 类）；sigtest；nettest。
3. **验收不变式**：`_start` 入口 rsp 16 对齐（systest case 里从 `stack_end` 校验 `& 0xF == 0`）。
4. hosttests：无新增（纯内核/用户态运行时行为）。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 对齐奇偶规则重构走样（meta_pad 与 argc/envc 奇偶耦合） | 布局「逐字节相同」约束 + 两个站点共用同一段代码后天然一致；systest 全量 + rsp 对齐断言 |
| 两份 crt0.S 副本漂移 | spec 明确 diff-identical 约定；实现 task 里包含 `diff user/crt0.S config/busybox.overlay/applets/crt0.S` 验证步 |
| 7 参调用的栈对齐错误只在高优化度下炸 | crt0 手写汇编最小化；systest 第一个跑的 case 就是入口自检 |
| `environ` 变真后 busybox 行为变化 | applet 验证全过 + 显式 PATH 用例 |

## 8. 后续（不属于本任务）

- AT_RANDOM + 用户栈 canary + `LWIP_RAND` 换熵池（P1，依赖本任务）
- 移除 `regs->rdi/rsi/rdx` 寄存器传参
- `getauxval()` 公开、`AT_PAGESZ`/`AT_PHDR`/`AT_EXECFN`（ASLR / 动态链接时）

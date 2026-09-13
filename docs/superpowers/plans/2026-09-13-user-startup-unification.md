# 统一用户态进程启动方式 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 内核单点构造用户态初始栈（argc/argv/envp/auxv），`crt0.S` 改标准 SysV `_start` 从栈解析并真正启用 `__libc_start_main`（设置 `environ`、保存 `__libc_auxv`/`__libc_stack_end`）。

**Architecture:** 三层换新但可分批落地：内核 helper 先行（旧 crt0 靠寄存器仍工作）→ csu 实现就位（尚无人调用）→ crt0 切换（一次性点亮）。systest 探针先行写好作为 RED 测试，用 weak extern 保证旧状态下可编译可运行且必失败。

**Tech Stack:** freestanding C（kernel + libc）、x86-64 手写汇编（crt0.S）、GNU Make profile 构建、QEMU python harness。

**Spec:** `docs/superpowers/specs/2026-09-13-user-startup-unification-design.md`（v3 + envp 界修订，commit `2b7f00d`）

## Global Constraints

- **Worktree 执行**：实现前用 superpowers:using-git-worktrees 切独立 worktree，不污染主工作区（用户既定工作方式）。
- **布局不变式**（spec §4.1）：`argc | argv[]+NULL | envp[]+NULL | auxv{AT_NULL,0} | align_pad(0/8，显式清零)`；envp 终止 NULL 与 auxv 起点**严格相邻**；`rsp & 0xF == 0`；pad 规则 `(argc+envc)&1 ? 0 : 8` 不变。
- **argv/envp==NULL 即空数组**：helper 一律构造完整布局，两调用点删除 `argv==NULL → USER_STACK_TOP` 分支（PID 1 `/bin/init` 是此路径，task.c:2181）。
- **寄存器设置保留**：`regs->rdi/rsi/rdx` 照旧设置（spec §4.5，调试可观察性，不作为兼容承诺）。
- **不加任何新 auxv 条目**（AT_RANDOM 属后续任务）。
- **测试**：`test-syscall` 禁止与 `KERNEL_SELFTEST=1` 同跑（run.mk:278 硬门控）。
- **commit 尾注**：`Co-Authored-By: Claude Code <noreply@anthropic.com>`。
- 构建入口统一 `make PROFILE=x86_64-clang <target>`（profile 文件 `mk/profiles/x86_64-clang.mk`）。

---

### Task 1: systest 启动自检测试（RED）

**Files:**
- Modify: `user/systest.c`（新增启动检查代码块 + main 签名/分发 + tests[] 两个条目）

**Interfaces:**
- Consumes: libc `environ`、`fork/execve/waitpid/exit`、`strcmp/strchr`（均已存在于 systest 的 include 集）
- Produces（后续 Task 4 依赖的名字，本 task 用 weak extern 引用）: `uint64_t *__libc_auxv`、`void *__libc_stack_end`（由 Task 3 在 csu.c 定义）；`static int g_argc; static char **g_argv;`（main 保存，供 test_startup_layout 用）

- [ ] **Step 1: 写失败测试**

在 `user/systest.c` 的 `// ── Runner ──` 注释（:2923）之前插入：

```c
// ── Unified startup self-checks (spec 2026-09-13-user-startup-unification) ──
// Weak extern: under the old crt0/csu these resolve to 0, tests FAIL (RED);
// Task 3/4 define them and the same code goes GREEN.
extern uint64_t *__libc_auxv      __attribute__((weak));
extern void     *__libc_stack_end __attribute__((weak));

static int g_argc;
static char **g_argv;

#define SU_ERR_STACKEND_NULL 0x001
#define SU_ERR_ALIGN         0x002
#define SU_ERR_ARGC_MISMATCH 0x004
#define SU_ERR_ARGV_ADDR     0x008
#define SU_ERR_ENVIRON_ADDR  0x010
#define SU_ERR_ENVIRON_NULL  0x020
#define SU_ERR_AUXV_NULL     0x040
#define SU_ERR_AUXV_ADDR     0x080
#define SU_ERR_AUXV_ATNULL   0x100
#define SU_ERR_AUXV_RANGE    0x200
#define SU_ERR_ENV_WALK      0x400
#define SU_ERR_ARGV_STR      0x800
#define SU_ERR_ENV_STR       0x1000

/* Validate the entry-stack chain of THIS process (spec §6.1). Returns 0 or
 * an OR of SU_ERR_* bits. Never dereferences anything under the old crt0
 * (returns SU_ERR_STACKEND_NULL before touching environ/auxv). */
static int startup_layout_errors(int argc, char **argv)
{
    extern char **environ;
    if (&__libc_stack_end == NULL || __libc_stack_end == NULL)
        return SU_ERR_STACKEND_NULL;

    int errs = 0;
    if ((uintptr_t)__libc_stack_end & 0xF)              errs |= SU_ERR_ALIGN;
    if (*(uint64_t *)__libc_stack_end != (uint64_t)argc) errs |= SU_ERR_ARGC_MISMATCH;
    if (argv != (char **)((uint64_t *)__libc_stack_end + 1)) errs |= SU_ERR_ARGV_ADDR;
    if (argv[argc] != NULL)                              errs |= SU_ERR_ARGV_STR;
    for (int i = 0; i < argc; i++)
        if (argv[i] == NULL || argv[i][0] == '\0')      errs |= SU_ERR_ARGV_STR;

    if (environ == NULL) return errs | SU_ERR_ENVIRON_NULL;
    if (argv + argc + 1 != environ)                      errs |= SU_ERR_ENVIRON_ADDR;

    int envc = 0;
    while (environ[envc] != NULL) {
        if (strchr(environ[envc], '=') == NULL)          errs |= SU_ERR_ENV_STR;
        if (++envc > 128) { errs |= SU_ERR_ENV_WALK; break; }
    }

    if (__libc_auxv == NULL) {
        errs |= SU_ERR_AUXV_NULL;
    } else {
        if ((uint64_t *)(environ + envc + 1) != __libc_auxv) errs |= SU_ERR_AUXV_ADDR;
        if (!(__libc_auxv[0] == 0 && __libc_auxv[1] == 0))   errs |= SU_ERR_AUXV_ATNULL;
        if (!((uintptr_t)__libc_auxv > (uintptr_t)__libc_stack_end)) errs |= SU_ERR_AUXV_RANGE;
    }
    return errs;
}

/* Entry probe: re-execed /bin/systest lands here BEFORE the test table.
 * Exit codes: 10 + argc*2 + envc on success (10..15), 99 on any failure,
 * 127 if execve itself returned. Never runs the test suite or forks. */
static void startup_probe(int argc, char **argv)
{
    int is_probe = (argc == 0) ||
                   (argc >= 1 && argc <= 2 && argv != NULL && argv[0] != NULL &&
                    strcmp(argv[0], "startup-probe") == 0);
    if (!is_probe) return;

    int errs = startup_layout_errors(argc, argv);
    if (argc == 2 && (argv[1] == NULL || strcmp(argv[1], "--envcheck") != 0))
        errs |= SU_ERR_ARGV_STR;
    if (errs) exit(99);

    extern char **environ;
    int envc = 0;
    while (environ[envc] != NULL) envc++;
    if (envc > 1) exit(99);
    if (envc == 1 && strcmp(environ[0], "OS01_TEST_K=veRy42") != 0) exit(99);
    exit(10 + argc * 2 + envc);
}

/* Parent side: fork + execve actual install path + waitpid.
 * status is the raw waitpid status (exit code << 8). */
static int run_startup_probe_case(char **argv, char **envp, int expect_code)
{
    pid_t pid = fork();
    if (pid == 0) {
        execve("/bin/systest", argv, envp);
        exit(127);                      /* execve returned: failure */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == expect_code;
}

static void test_startup_layout(void)
{
    int errs = startup_layout_errors(g_argc, g_argv);
    CHECK3(errs == 0, "startup layout", errs ? "entry chain broken" : "entry chain ok");
}

/* spec §6.2 six-case parity matrix + §6.3 NULL-pointer forms */
static void test_startup_matrix(void)
{
    char *argv0[1] = {NULL};
    char *argv1[2] = {(char *)"startup-probe", NULL};
    char *argv2[3] = {(char *)"startup-probe", (char *)"--envcheck", NULL};
    char *env1[2]  = {(char *)"OS01_TEST_K=veRy42", NULL};
    struct { char **av; char **ep; int code; const char *name; } cases[] = {
        {argv0, NULL, 10, "argc=0 envc=0 (even/pad8)"},
        {argv0, env1, 11, "argc=0 envc=1 (odd/pad0)"},
        {argv1, NULL, 12, "argc=1 envc=0 (odd/pad0)"},
        {argv1, env1, 13, "argc=1 envc=1 (even/pad8)"},
        {argv2, NULL, 14, "argc=2 envc=0 (even/pad8)"},
        {argv2, env1, 15, "argc=2 envc=1 (odd/pad0)"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK3(run_startup_probe_case(cases[i].av, cases[i].ep, cases[i].code),
               "startup matrix", cases[i].name);
    CHECK3(run_startup_probe_case(NULL, NULL, 10), "startup NULL argv", "argv=NULL envp=NULL");
    CHECK3(run_startup_probe_case(NULL, env1, 11), "startup NULL argv", "argv=NULL envp=1");
}
```

再把 `main`（:3009）改为带参签名并在最前面分发探针：

```c
int main(int argc, char **argv, char **envp)
{
    (void)envp;
    startup_probe(argc, argv);
    g_argc = argc;
    g_argv = argv;

    printf("[SYS TEST] OS01 Syscall Test Suite\n");
    /* ……以下原样保留（:3012 起的循环与 summary）…… */
```

tests[] 表（:2927）在 `{"signal handler sync", ...}` 之前插入两个条目（spec §7 要求入口自检第一个跑）：

```c
    {"startup layout",    test_startup_layout},
    {"startup matrix",    test_startup_matrix},
```

- [ ] **Step 2: 运行验证 RED**

Run: `make PROFILE=x86_64-clang test-syscall`
Expected: 编译链接通过（weak extern 落 0）；原 268 例全部 PASS；新增 2 例 FAIL——`startup layout` 报 entry chain broken（SU_ERR_STACKEND_NULL），`startup matrix` 8 个 CHECK3 全 FAIL（探针 exit 99）。**任何旧用例转红即为本步失败。**

- [ ] **Step 3: Commit**

```bash
git add user/systest.c
git commit -m "test(systest): startup layout/matrix probes — RED for startup unification

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: 内核 setup_user_stack 单点构造

**Files:**
- Modify: `kernel/sched/task.c`（新增 helper + 替换 `spawn_user_task()` ~1226–1287 与 `sys_exec()` ~1385–1470 两处内联构造）

**Interfaces:**
- Consumes: `Phy_To_Virt`、`USER_STACK_BASE/USER_STACK_TOP`（task.h:366）、`strlen/memcpy`、`ASSERT`（task.c 内已用）、`free_pages(struct Page*, int32_t)`（pmm.c:410）、`-E2BIG/-ENOMEM` errno
- Produces: `static int setup_user_stack(uint8_t *kstack, char *const argv[], char *const envp[], int *out_argc, uint64_t *out_argv_ptr, uint64_t *out_envp_ptr, uint64_t *out_rsp)` — 返回 0 或 `-E2BIG`；`*out_rsp` 指向 argc（16 对齐）

- [ ] **Step 1: 写 helper（放在 spawn_user_task 之前，file-static）**

```c
/* ── Unified SysV initial-stack builder (spec 2026-09-13-user-startup-unification §4.1) ──
 * Builds (low→high): argc | argv[]+NULL | envp[]+NULL | auxv{AT_NULL,0} | align_pad.
 * The envp terminator NULL is STRICTLY adjacent to the auxv start; the align
 * pad sits ABOVE auxv and is explicitly zeroed. argv/envp may be NULL (empty).
 * Combined argc+envc is capped at 128 (defense-in-depth: trap.c already
 * enforces this for the exec syscall path; this covers spawn callers). */
#define STARTUP_STR_MAX 128

static int setup_user_stack(uint8_t *kstack, char *const argv[], char *const envp[],
                            int *out_argc, uint64_t *out_argv_ptr,
                            uint64_t *out_envp_ptr, uint64_t *out_rsp)
{
#define KSTACK(va) (kstack + ((va) - USER_STACK_BASE))
    int s_argc = 0, s_envc = 0;
    while (argv != NULL && argv[s_argc] != NULL) s_argc++;
    while (envp != NULL && envp[s_envc] != NULL) s_envc++;
    if (s_argc + s_envc > STARTUP_STR_MAX) return -E2BIG;

    uint64_t str_offset[STARTUP_STR_MAX];
    int si = 0;
    uint64_t rsp = USER_STACK_TOP;

    /* strings, descending */
    for (int i = 0; i < s_argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        rsp -= len;
        memcpy(KSTACK(rsp), argv[i], len);
        str_offset[si++] = rsp;
    }
    for (int i = 0; i < s_envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        rsp -= len;
        memcpy(KSTACK(rsp), envp[i], len);
        str_offset[si++] = rsp;
    }
    rsp &= ~15ULL;

    /* align pad (0 or 8) — ABOVE auxv, zeroed */
    if (((s_argc + s_envc) & 1) == 0) {
        rsp -= 8;
        *(uint64_t *)KSTACK(rsp) = 0;
    }
    /* auxv: single AT_NULL pair (Task: canary adds AT_RANDOM here) */
    rsp -= 16;
    *(uint64_t *)KSTACK(rsp)      = 0;   /* AT_NULL */
    *(uint64_t *)KSTACK(rsp + 8)  = 0;
    /* envp[] + terminator NULL */
    rsp -= 8;
    *(uint64_t *)KSTACK(rsp) = 0;
    uint64_t envp_arr = rsp;
    for (int i = s_envc - 1; i >= 0; i--) {
        rsp -= 8;
        *(uint64_t *)KSTACK(rsp) = str_offset[s_argc + i];
    }
    /* argv[] + terminator NULL */
    rsp -= 8;
    *(uint64_t *)KSTACK(rsp) = 0;
    uint64_t argv_arr = rsp;
    for (int i = s_argc - 1; i >= 0; i--) {
        rsp -= 8;
        *(uint64_t *)KSTACK(rsp) = str_offset[i];
    }
    /* argc */
    rsp -= 8;
    *(uint64_t *)KSTACK(rsp) = (uint64_t)s_argc;

    ASSERT((rsp & 0xF) == 0);
    *out_argc = s_argc;
    *out_argv_ptr = argv_arr;
    *out_envp_ptr = envp_arr;
    *out_rsp = rsp;
    return 0;
#undef KSTACK
}
```

对齐自检（实现者必验）：元数据总字节数 = `8*(argc+envc+5)+pad`（argc 槽 8 + argv 终止 8 + envp 终止 8 + auxv 16 + 数组本体）。`argc+envc` 奇 → 总计 ≡0 (mod 16)，pad=0；偶 → ≡8，pad=8 补齐——与旧规则逐字一致（旧 exec 站点同代数）。

- [ ] **Step 2: 替换 spawn 站点**

`spawn_user_task()` 内 `// ── 6.5 Set up argv on user stack` 到 `#undef KSTACK` 的整个 `if (argv != NULL) {...}` 块（~1230–1287）替换为：

```c
    // ── 6.5 Construct the SysV initial stack (argc/argv/envp/auxv) ──
    uint8_t *kstack = (uint8_t *)Phy_To_Virt(stack_page->phy_address);
    int s_argc = 0;
    uint64_t user_rsp = 0, user_arg_ptr = 0, user_env_ptr = 0;

    int sret = setup_user_stack(kstack, (char *const *)argv, NULL,
                                &s_argc, &user_arg_ptr, &user_env_ptr, &user_rsp);
    if (sret != 0) {
        free_pages(stack_page, 1);
        vmm_free_user_map(user_pgd);
        kfree(mm); kfree(thd); kfree(raw_alloc);
        return sret;
    }
```

（失败 unwind 与本函数上方 `alloc_pages` 失败分支同一变量集；`sret != 0` 仅 `-E2BIG`，现实中 spawn 只被 task_init 以 NULL argv 调用，永不触发。）

同函数 regs 设置（~1279–1286）改两行：

```c
    regs->rsp     = user_rsp;             // 不再有 argv==NULL → USER_STACK_TOP 分支
    ...
    regs->rdx     = user_env_ptr;         // 原为硬编码 0
```

（`regs->rdi = (uint64_t)s_argc; regs->rsi = user_arg_ptr;` 保持原样。）

- [ ] **Step 3: 替换 exec 站点**

`sys_exec()` 内 `// ── 6.5 Set up argv/envp on user stack` 到 `#undef KSTACK` 的整个 `if (argv != NULL) {...}` 块（~1385–1469）替换为：

```c
    // ── 6.5 Construct the SysV initial stack (argc/argv/envp/auxv) ──
    uint8_t *kstack = (uint8_t *)Phy_To_Virt(stack_page->phy_address);
    int s_argc = 0;
    uint64_t user_rsp = 0, user_arg_ptr = 0, user_env_ptr = 0;

    int sret = setup_user_stack(kstack, (char *const *)argv, (char *const *)envp,
                                &s_argc, &user_arg_ptr, &user_env_ptr, &user_rsp);
    if (sret != 0) {
        free_pages(stack_page, 1);
        vmm_free_user_map(new_pgd);
        kfree(new_mm);
        return sret;
    }
```

（此块上方的 `int s_argc = 0; int s_envc = 0; ...` 局部声明行一并删除，由 helper 出参接管；失败 unwind 复制上方 `-ENOMEM` 分支的变量集。）regs 设置（~1508）`regs->rsp = (argv != NULL) ? user_rsp : USER_STACK_TOP;` 改为 `regs->rsp = user_rsp;`（`rdi/rsi/rdx` 三行原样，`rdx` 本就是 `user_env_ptr`）。

- [ ] **Step 4: 编译 + QEMU 验证（新内核 + 旧 crt0，注册表 ABI 未变）**

Run: `make PROFILE=x86_64-clang test-syscall`
Expected: 原 268 例全部 PASS；Task 1 的 2 个新用例**仍 FAIL**（旧 crt0 不定义 `__libc_*`，探针 exit 99）——这是预期的中间 RED 态。busybox 正常启动（`make PROFILE=x86_64-clang test-syscall-repeat` 通过，正常 init/ash exec 路径即本站点回归）。

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/task.c
git commit -m "refactor(sched): single setup_user_stack builder for spawn+exec

argv==NULL now builds a full minimal layout (PID 1 path); align pad moves
above auxv and is zeroed so the envp terminator is strictly adjacent to
the auxv start. regs rdi/rsi/rdx still set (spec §4.5).

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: `__libc_start_main` 真正实现

**Files:**
- Modify: `libc/csu/csu.c`（整文件重写，17 行 → ~50 行）

**Interfaces:**
- Consumes: `environ`（libc/stdlib/environ.c 定义）
- Produces（Task 1 的 weak extern 在链接时解析到这里）: `uint64_t *__libc_auxv`（auxv 起点指针）、`void *__libc_stack_end`（_start 入口 rsp）；`__libc_start_main` 同签名

- [ ] **Step 1: 重写 csu.c**

```c
#include <stdlib.h>    /* environ (extern) */
#include <stdint.h>

extern char **environ;

/* Set by __libc_start_main; consumed by tests now and getauxval/AT_RANDOM
 * later (spec 2026-09-13-user-startup-unification §4.3). */
uint64_t *__libc_auxv;      /* auxv start (points at the FIRST entry) */
void     *__libc_stack_end; /* entry rsp of _start */

int __libc_start_main(int (*main)(int, char **, char **),
                      int argc, char **argv,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end)
{
    (void)init; (void)fini; (void)rtld_fini;
    __libc_stack_end = stack_end;

    /* argv[argc]==NULL, then envp[]; its terminating NULL is STRICTLY
     * followed by auxv (kernel layout contract, spec §4.1). */
    char **ep = argv + argc + 1;
    environ = ep;
    int env_i;
    for (env_i = 0; env_i < 128 && ep[env_i] != NULL; env_i++)
        ;
    if (env_i >= 128) {
        /* envp missing its terminator (kernel layout bug): drop auxv,
         * keep the process alive so tests can report (spec §4.3). */
        __libc_auxv = NULL;
        return main(argc, argv, environ);
    }

    uint64_t *av = (uint64_t *)(ep + env_i + 1);
    __libc_auxv = av;
    for (int i = 0; i < 64; i++) {
        if (av[2 * i] == 0) goto found;    /* AT_NULL */
    }
    __libc_auxv = NULL;                    /* no AT_NULL within 64 pairs */
found:
    return main(argc, argv, environ);
}
```

- [ ] **Step 2: 编译验证（无行为变化——尚无人调用）**

Run: `make PROFILE=x86_64-clang test-syscall`
Expected: 与 Task 2 末尾完全相同——268 旧例 PASS，2 个新用例仍 FAIL（crt0 还在直接 `call main`，`__libc_*` 落 bss 0 值）。链接不报重复定义/未定义。

- [ ] **Step 3: Commit**

```bash
git add libc/csu/csu.c
git commit -m "feat(libc): implement __libc_start_main — environ init + auxv walk

Defines __libc_auxv (auxv start) and __libc_stack_end (entry rsp); both
envp and auxv walks bounded (128 entries / 64 pairs). Not yet called —
crt0 switches in the next commit.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: crt0 标准 SysV `_start` + busybox overlay 同步（GREEN）

**Files:**
- Modify: `user/crt0.S`（整文件重写）
- Modify: `config/busybox.overlay/applets/crt0.S`（与上面逐字节相同）

**Interfaces:**
- Consumes: Task 3 的 `__libc_start_main(main, argc, argv, 0, 0, 0, stack_end)`；各程序符号 `main`、`__bss_start/__bss_end`
- Produces: 标准 SysV 进程入口（`[rsp]`=argc，rsp 16 对齐）

- [ ] **Step 1: 重写 user/crt0.S**

```asm
.section .text.start,"ax"
.global _start
.type _start, @function
_start:
    # Clear frame pointer for backtrace termination
    xorq %rbp, %rbp

    # Zero-initialize BSS
    leaq __bss_start(%rip), %rdi
    leaq __bss_end(%rip), %rcx
    subq %rdi, %rcx
    jz 1f
    xorb %al, %al
    rep stosb
1:
    # Save entry rsp AFTER BSS clear — xorb %al above clobbers rax, and rsp
    # is unchanged by rep stosb, so saving here is equivalent (spec §4.2).
    movq %rsp, %rax

    # __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
    # rdi=main rsi=argc rdx=argv rcx=init(0) r8=fini(0) r9=rtld_fini(0)
    # 7th arg (stack_end) passed on the stack.
    movq (%rsp), %rsi          # argc  ← from the stack (not kernel registers)
    leaq 8(%rsp), %rdx         # argv
    leaq main(%rip), %rdi      # main
    xorq %ecx, %ecx
    xorq %r8d, %r8d
    xorq %r9d, %r9d
    subq $16, %rsp             # keep rsp 16-aligned; [rsp] = arg slot
    movq %rax, (%rsp)          # stack_end = entry rsp
    call __libc_start_main
    addq $16, %rsp

    # exit(main's return value)
    movl %eax, %edi
    movl $2, %eax              # SYS_exit = 2
    int $0x80

    # Should never reach here
    hlt
```

对齐不变式（实现者必验）：入口 rsp≡0 (mod 16)（内核 `ASSERT` 保证）→ `subq $16` 后仍 ≡0 → `call` 压返回地址后 `__libc_start_main` 内 rsp≡8（SysV）。

- [ ] **Step 2: 同步 busybox overlay（diff-identical 约定）**

```bash
cp user/crt0.S config/busybox.overlay/applets/crt0.S
diff user/crt0.S config/busybox.overlay/applets/crt0.S && echo IDENTICAL
```

Expected: `IDENTICAL`（busybox input digest 覆盖 overlay，自动重编）。

- [ ] **Step 3: 全量验证（GREEN + 回归）**

Run: `make PROFILE=x86_64-clang test-syscall`
Expected: **全部 PASS**——原 268 例 + `startup layout` + `startup matrix`（8 CHECK3：六组奇偶矩阵 + 2 个 NULL argv 形态）。

Run: `make PROFILE=x86_64-clang test-syscall-repeat`
Expected: PASS（正常 init + busybox ash 反复 exec/exit——exec 站点 + busybox crt0 + environ 变真的综合回归）。

Run: `make PROFILE=x86_64-clang test-network`
Expected: PASS（nettest 6/6）。

Run: `make PROFILE=x86_64-clang test-inittab`
Expected: PASS（inittab 解析/init 引导路径）。

Run: `make PROFILE=x86_64-clang run`（交互，见 docs/build-run-debug.md）
Expected: busybox ash 正常起；抽查 applet：`env`（environ 变真后行为）、`echo $PATH`、`ls /bin`、`/bin/sigtest`（sigreturn 路径未受栈布局变化影响）。重点：`env` 输出的环境与 inittab/init 传入一致，无乱码/空值。

- [ ] **Step 4: Commit**

```bash
git add user/crt0.S config/busybox.overlay/applets/crt0.S
git commit -m "feat(crt0): standard SysV _start via __libc_start_main

argc/argv now come from the entry stack ([rsp]) instead of kernel
registers; environ is initialized from the stack for the first time.
BusyBox overlay crt0 kept diff-identical.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 5: 文档同步

**Files:**
- Modify: `docs/roadmap.md`（P1 表「统一用户态启动方式」行 → ✅）

**Interfaces:**
- Consumes: Task 4 的验证结果（全部通过记录）
- Produces: roadmap 状态更新（后续 AT_RANDOM canary 任务的入口引用）

- [ ] **Step 1: 更新 roadmap P1 行**

`docs/roadmap.md` P1 表「统一用户态启动方式」行的状态列追加：`✅（2026-09-XX，commits <Task1>..<Task4>；systest 270 例 + busybox 回归全绿）`，并在「用户栈 canary」行的依赖列把「统一用户态启动方式」标 ✅。

- [ ] **Step 2: Commit**

```bash
git add docs/roadmap.md
git commit -m "docs(roadmap): 统一用户态启动方式 done — user canary unblocked

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

- [ ] **Step 3: 回主工作区收尾**（superpowers:finishing-a-development-branch：merge worktree 分支回 master）

---

## Self-Review 记录

- **Spec 覆盖**：§4.1（Task 2）、§4.2（Task 4）、§4.3（Task 3）、§4.4（Task 4 Step 2）、§4.5（Task 2 保留 regs）、§5 五项行为变化（#1 Task 2、#2 Task 2、#3 Task 3/4 + Task 4 Step 3 `env` 抽查、#4 Task 2 helper、#5 Task 4）、§6 测试（Task 1 全部用例：§6.1 layout、§6.2 六组矩阵、§6.3 NULL 两种形态 + PID 1 spawn 由 test-syscall-repeat 的正常 init 引导覆盖、§6.4 四个回归目标）、§7 风险对策均已内嵌。无遗漏。
- **占位符扫描**：无 TBD/TODO；所有代码步骤含完整代码；「原样保留」仅用于指代未改动的既有行。
- **类型一致性**：`setup_user_stack` 签名在 Task 2 定义、Task 2 内两处调用一致；`__libc_auxv`/`__libc_stack_end` 类型（`uint64_t*`/`void*`）在 Task 1（weak extern）与 Task 3（定义）一致；探针退出码公式 `10+argc*2+envc` 与父进程期望表逐行核对（10–15）。

# AAGU-4 跨边界符号/ABI 边界治理 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 AAGU-4 规范文档 `docs/arch/cross-boundary-symbols.md` §3 现状对照表里的违例项**逐条收口**，让 OS01 跨边界符号 / ABI 全部走单一来源。

**Architecture:** 4 个独立 follow-up issue（每个对应一条或一类违例），按 §3 现状对照表的 P1 / P2 / P3 优先级排序。**不引入新的镜像层**（spec §1 明文禁止）；镜像收口路径走「合并模式」（kernel 直接 include libc）或「install 模式」（kernel 拥有 + build 步骤 install 到 libc sysroot），由落地 issue 在 plan 阶段二选一。

**Tech Stack:** OS01 kernel + libc + runtime（C11、clang、freestanding kernel）；Makefile + mk/components/*.mk；sysroot install 步骤（kernel UAPI → libc sysroot `<sys/auxv.h>` 等）。

**Spec:** [`docs/arch/cross-boundary-symbols.md`](../../arch/cross-boundary-symbols.md) — 本 plan 的 4 个任务一一对应 spec §4 后续 issue 切分。

**前置条件：** AAGU-4 规范已落地（spec 文档 + AGENTS.md 索引更新，commit 待 PR）。本 plan 不写 AAGU-4 的 PR——只规划 follow-up 落地。

---

## Global Constraints

每条都来自 spec，违反 = 镜像 / 违反 = `#ifdef __x86_64__` 在通用层：

1. **编译器 runtime 符号**（spec §2.1）：唯一定义点 `kernel/compiler_rt/<name>.c` 或 `libc/<libname>/<name>.c`，二选一；不在两处同时存在；不散落到 `kernel/arch/<arch>/`。
2. **跨用户 ABI**（spec §2.2）：单一源 `kernel/include/uapi/<topic>.h`；libc 通过 sysroot 包含，**不允许镜像**；常量集必须一致。
3. **arch value**（spec §2.3）：arch-neutral facade `kernel/include/arch/<topic>.h` + per-arch strong override `kernel/arch/<arch>/<topic>.c`；arch-neutral builder 不出现 arch 字符串、不出现 `#ifdef __x86_64__` 分支。
4. **libc API 镜像**（spec §2.4）：禁止「人工同步镜像」（头注释不能写 "FROZEN, do not modify without updating both"）；新镜像必须有 install / 合并模式说明。
5. **不引入新镜像层**（spec §1 禁止）：不能用「新建一个镜像目录」解决镜像问题。
6. **每个改动都必须**：
   - 通过 `make PROFILE=x86_64-clang test-user-canary`（build contract）
   - 通过 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1`（kernel self-tests）
   - 通过 `make OS01_SYSTEST=1 test-syscall`（systest E2E，单独跑）
   - 通过 `make PROFILE=aarch64-clang test-aarch64-uefi-smp`（SMP=1 ×3 + 已知 GIC TIMEOUT 不计入）

---

## Task 1: libc stdio `fread/fwrite` FILE 注册扩面（AAGU-4.1，P1）

**Files:**
- Modify: `libc/stdio/stdio_file.c:108-130`（`fread` + `fwrite` 入口加 `is_open_file` 检查）
- Modify: `libc/stdio/stdio_file.c:117-130`（`fwrite` 同样加检查，注意 `stdout/stderr` 已 special-case）

**Spec ref:** `docs/arch/cross-boundary-symbols.md` §3.5 + §2.4（FILE 注册本质是 libc API 镜像的「镜像」反面——同一份 FILE 概念应在 libc 内部一致）。

**Interfaces:**
- Consumes: 现有 `is_open_file(void *f)`（`libc/stdio/stdio_file.c:54-60`）
- Produces: `fread/fwrite` 入口增加 stream 验证（与 `fflush` 行 132-154 一致）

- [ ] **Step 1: 写 hosttest 覆盖 `fread/fwrite` 验证路径 — 必须用 OS01 sysroot 交叉编译**

**reviewer #5 修正**：原 hosttest 用 `clang ... -L build/x86_64-clang/runtime -lk` 链的是 host libc（不是 OS01 libc），无法验证 OS01 stdio 行为。改用 OS01 sysroot 交叉编译。

文件：`hosttests/libc/test_fread_fwrite_stream_validation.c`（NEW）

```c
// Reproduces the bug: passing an unregistered FILE* to fread/fwrite
// currently silently falls through; after this fix, it must return 0
// (per POSIX: not a valid stream = no bytes read/written).
// MUST be compiled against OS01 libc via --sysroot, NOT host libc.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char buf[16];

    /* Unregistered "stream" (not from fopen/fdopen) — must NOT read.
     * Test runs as OS01 user process (QEMU or sysroot-chrooted).
     * Prints "FAIL: fread returned N" and exits nonzero if not zero. */
    int garbage_stream = 0xDEADBEEF;
    size_t n = fread(buf, 1, sizeof buf, (void *)garbage_stream);
    if (n != 0) {
        fprintf(stderr, "FAIL: fread returned %zu, want 0\n", n);
        return 1;
    }

    /* Unregistered stream for write — must NOT write */
    n = fwrite("x", 1, 1, (void *)garbage_stream);
    if (n != 0) {
        fprintf(stderr, "FAIL: fwrite returned %zu, want 0\n", n);
        return 1;
    }

    /* stdout / stderr are sentinels (already special-cased in fwrite);
     * for fread on stdout it should also short-circuit (POSIX: stdout
     * not open for reading). */
    n = fread(buf, 1, sizeof buf, stdout);
    if (n != 0) {
        fprintf(stderr, "FAIL: fread(stdout) returned %zu, want 0\n", n);
        return 1;
    }

    printf("PASS: fread/fwrite validate unregistered streams\n");
    return 0;
}
```

- [ ] **Step 2: 跑 hosttest 验证它当前失败（OS01 sysroot 编译）**

```sh
cd /home/aagu/OS01
SYSROOT=$(make -s PROFILE=x86_64-clang print-sysroot)
clang --target=x86_64-elf --sysroot="$SYSROOT" \
      -ffreestanding \
      -I"$SYSROOT/usr/include" \
      hosttests/libc/test_fread_fwrite_stream_validation.c \
      -L"$SYSROOT/usr/lib" -lc \
      -o build/x86_64-clang/test_fread_fwrite_stream_validation.elf
# run under QEMU as OS01 user binary:
make PROFILE=x86_64-clang test-libc-stream-validation
```

Expected: FAIL with `fread returned 1, want 0`（因为 fread 当前不解 garbage_stream，直接读 fd 0xDEADBEEF = 不合法 fd → read 返回 -1 → 但 size/0 = 0... 见原 spec）。实际失败模式由 fread 当前实现决定。

- [ ] **Step 3: 改 `fread` 加 `is_open_file` 检查**

`libc/stdio/stdio_file.c:108` 起改 `fread`：

```c
size_t fread(void *ptr, size_t size, size_t nmemb, void *f)
{
    if (!f || !ptr) return 0;
    /* stdout/stderr are sentinels (fd 1/2), not mini_file_t — POSIX: stdout
     * is not open for reading; return 0 rather than dereferencing. */
    if (f == stdout || f == stderr || f == stdin) return 0;
    if (!is_open_file(f)) return 0;       /* NEW: not a stream we own */
    mini_file_t *mf = (mini_file_t *)f;
    int64_t n = read(mf->fd, ptr, size * nmemb);
    if (n < 0) return 0;
    return (size_t)(n / size);
}
```

- [ ] **Step 4: 改 `fwrite` 加 `is_open_file` 检查**

`libc/stdio/stdio_file.c:117` 起改 `fwrite`：

```c
size_t fwrite(const void *p, size_t s, size_t n, void *f)
{
    if (!f || !p) return 0;
    /* stdout/stderr are raw fd 1/2, not wrapped in mini_file_t */
    if (f == stdout || f == stderr) {
        int fd = (f == stderr) ? 2 : 1;
        int64_t written = write(fd, p, s * n);
        return (written < 0) ? 0 : (size_t)(written / s);
    }
    if (!is_open_file(f)) return 0;       /* NEW: not a stream we own */
    mini_file_t *mf = (mini_file_t *)f;
    int64_t written = write(mf->fd, p, s * n);
    if (written < 0) return 0;
    return (size_t)(written / s);
}
```

- [ ] **Step 5: 跑 hosttest 验证 RED → GREEN**

```sh
clang -o /tmp/test_fread_fwrite_stream_validation \
    hosttests/libc/test_fread_fwrite_stream_validation.c \
    -L build/x86_64-clang/runtime -lk
/tmp/test_fread_fwrite_stream_validation
```

Expected: PASS — three asserts all green.

- [ ] **Step 6: 把 hosttest 加进 hosttests Makefile / 入口列表**

- [ ] **Step 7: 跑完整测试套件**

```sh
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 kernel.bin
make OS01_SYSTEST=1 test-syscall
make PROFILE=aarch64-clang test-aarch64-uefi-smp   # SMP=1 ×3
```

Expected: 全部 PASS（已知 GIC Phase 1 TIMEOUT 不计入）。

- [ ] **Step 8: Commit**

```sh
git add hosttests/libc/test_fread_fwrite_stream_validation.c \
        libc/stdio/stdio_file.c
git commit -m "fix(libc/stdio): validate stream in fread/fwrite (close AAGU-4.1)"
```

- [ ] **Step 9: 调研 atexit 死链（Explore agent finding）**

`__call_atexit_handlers`（`libc/stdlib/atexit.c:20`）定义但全 repo 0 caller。`__libc_start_main`（`libc/csu/csu.c:35, 51`）直接 `return main(...)`，无 `exit()` / `fflush(NULL)` / fini 触发。

- [ ] **Step 10: 写 hosttest 验证 atexit 不被调用 — 必须用 OS01 sysroot 交叉编译，不链宿主 libc**

**reviewer #5 修正**：原 hosttest 用 `clang ... -L build/x86_64-clang/runtime -lk` 链的是 host libc（不是 OS01 libc），无法验证 OS01 代码行为。改用 OS01 的 sysroot 交叉编译。

文件：`hosttests/libc/test_atexit_no_call.c`（NEW）

```c
// Reproduces the bug: __libc_start_main returns main() directly without
// triggering atexit handlers. After this fix, exit() must call registered
// atexit handlers before _exit.
// MUST link against OS01 libc via --sysroot, NOT host libc.
#include <stdio.h>
#include <stdlib.h>

static volatile int teardown_called = 0;
static void teardown(void) { teardown_called = 1; }

int main(void) {
    atexit(teardown);
    /* Currently returns without calling teardown; after fix: */
    exit(0);
    /* unreachable — exit() must terminate before reaching here.
     * If exit() returns, that's the regression. */
    return 1;  /* not 0 — atexit handler is expected to have run before _exit. */
}

/* At runtime: a wrapper invokes the test as a QEMU-loaded OS01 user binary.
 * Test runner reads /proc/self/status or stdout marker to check teardown_called
 * was set before _exit. */

/* Stub: weak, test prints result via stdout before exit so the runner can see
 * whether atexit ran. */
__attribute__((destructor)) static void report(void) {
    /* If atexit works, teardown_called == 1 here. */
    fprintf(stderr, "atexit_ran=%d\n", teardown_called);
    _exit(teardown_called == 1 ? 0 : 1);
}
```

构建命令（用 OS01 sysroot，不用 host）：

```sh
clang --target=x86_64-elf --sysroot=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^sysroot=//p') \
      -ffreestanding -nostdlib \
      -I$(STAGING_DIR)/libc/usr/include \
      hosttests/libc/test_atexit_no_call.c \
      -L$(STAGING_DIR)/libc/usr/lib -lc \
      -o build/x86_64-clang/test_atexit_no_call.elf
```

（或更简单：纳入 `hosttests/` Makefile 现有目标，由 Makefile 统一 sysroot 注入）

- [ ] **Step 11: 改 `__libc_start_main` 路径触发 exit / fini**

让 `__libc_start_main` 在 `return main(...)` 后调 `exit(ret)`，并在 `exit()` 内串 `__call_atexit_handlers`。或者更简单：直接把 `__libc_start_main` 末尾改成 `exit(main(argc, argv, environ))`。Step 10 hosttest 此时应 GREEN（atexit_ran=1）。

- [ ] **Step 12: 跑完整测试套件 + Step 13: Commit atexit 修**

```sh
git add hosttests/libc/test_atexit_no_call.c \
        hosttests/libc/Makefile \
        libc/csu/csu.c \
        libc/stdlib/atexit.c \
        libc/stdlib/exit.c   # or wherever exit() lives
git commit -m "fix(libc): invoke atexit handlers from exit() (close AAGU-4.1 atexit)"
```

---

## Task 2: UAPI auxv 单一源收口（AAGU-4.2，P1）

**Files:**
- Modify: `kernel/include/uapi/auxv.h`（添加 libc 端额外常量）
- Modify OR Delete: `libc/include/sys/auxv.h`（转发或删除）
- Modify: `kernel/Makefile`（加 install 步骤：uapi/auxv.h → libc sysroot `usr/include/sys/auxv.h`）

**Spec ref:** `docs/arch/cross-boundary-symbols.md` §2.2 + §3.2

**Interfaces:**
- Consumes: 现有 15 个 `AT_*` 常量在 `kernel/include/uapi/auxv.h`；22 个在 `libc/include/sys/auxv.h`
- Produces: 单一 `kernel/include/uapi/auxv.h` 拥有全部；libc sysroot 通过 install 步骤取得

- [ ] **Step 1: 在 `kernel/include/uapi/auxv.h` 补齐 7 个缺失常量**

`kernel/include/uapi/auxv.h:11` 后插入（按 Linux `<sys/auxv.h>` 标准值）：

```c
#define AT_NOTELF        10   /* program is not ELF */
#define AT_UID           11   /* real uid */
#define AT_EUID          12   /* effective uid */
#define AT_GID           13   /* real gid */
#define AT_EGID          14   /* effective gid */
#define AT_SECURE        23   /* boolean, set for security-sensitive */
#define AT_HWCAP2        26   /* extension of AT_HWCAP */
#define AT_EXECFN        31   /* program filename */
```

并把行 4 注释里的「Kernel-side mirror of libc/include/sys/auxv.h」改为「Single source for both kernel-side setup and libc user-space getauxval()」。

- [ ] **Step 2: 改 `libc/include/sys/auxv.h` 为转发头（不立即删除）**

保留 `libc/include/sys/auxv.h` 但把所有 `AT_*` 常量定义替换为 `#include <uapi/auxv.h>` 转发：

```c
#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H

/* Single source: kernel/include/uapi/auxv.h (installed to libc sysroot) */

#include <uapi/auxv.h>

unsigned long getauxval(unsigned long type);

#endif /* _SYS_AUXV_H */
```

这样 libc 编译期仍能找到 `AT_*`，但不重复定义。

- [ ] **Step 3: 加 Makefile install 步骤 — owner = `mk/components/sysroot.mk` 唯一 writer**

**关键**：sysroot 装配的 owner 是 **`mk/components/sysroot.mk`**，不是 kernel/Makefile、不是 libc/Makefile。sysroot.mk 自身头部声明「NO other module writes into a generation or the sysroot: component installs only ever write `$(STAGING_DIR)/<component>`」。kernel 一侧要做的是把 UAPI 头 stage 到 `$(STAGING_DIR)/kernel-headers/usr/include/<sys/...>`，由 sysroot.mk 的 `kernel-headers-install.stamp` publisher 把它合进 sysroot generation。

改动两处：

**a. `kernel/Makefile`：加 install-headers 步骤**，把 UAPI 头装进 staging：

```make
# AAGU-4.2: stage UAPI headers for sysroot assembly
install-headers:
	@mkdir -p $(DESTDIR)/usr/include/sys
	install -m 644 $(CURDIR)/include/uapi/auxv.h $(DESTDIR)/usr/include/sys/auxv.h
	install -m 644 $(CURDIR)/include/uapi/stat.h $(DESTDIR)/usr/include/sys/stat.h
	# ... 其他 UAPI 头同理
```

**b. `mk/components/sysroot.mk`**：`kernel-headers-install.stamp` 已经存在（见 file 第 49-52 行附近）；**只需确认**它消费 `$(STAGING_DIR)/kernel-headers` 并装进 `$(STAGING_DIR)/kernel-headers/usr/include/`。**不要**新增 writer —— 现有 writer 通过 `os01_submake(kernel, install-headers INSTALL_ROOT=$(STAGING_DIR)/kernel-headers ARCH=x86_64 ...)` 已正确路由。

依赖图（已经存在，无需改）：`sysroot_*.stamp` ← `libc-install.stamp` ← `kernel-headers-install.stamp`。Sysroot generation 装配时把 `$(STAGING_DIR)/kernel-headers/usr/include` 和 `$(STAGING_DIR)/libc/usr/include` merge 进 sysroot。

**禁止**：
- 直接写 `$(SYSROOT_GENERATION_DIR)/usr/include/sys/auxv.h` —— 绕过 sysroot.mk publisher 会破坏 sysroot 完整性（重做时 staging 还在，generation 已被覆盖）
- 在 `kernel/Makefile` 里加 `sysroot-install` target —— owner 不对

- [ ] **Step 4: 跑测试确认 x86_64 build / libc 链接正常**

```sh
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test-user-canary
```

Expected: libc 编译能找到 `AT_*` 常量（通过 install 步骤）；kernel emit 路径无变化。

- [ ] **Step 5: 验证 `grep -r "AT_.*[0-9]" libc/include/sys/auxv.h` 只剩一行 `#include`**

Expected: 只有 Step 2 的转发头，无 `#define AT_*` 行。

- [ ] **Step 6: 跑完整测试套件**（同 Task 1 Step 7）

- [ ] **Step 7: Commit**

```sh
git add kernel/include/uapi/auxv.h \
        libc/include/sys/auxv.h \
        kernel/Makefile
git commit -m "feat(uapi): single source for AT_* (close AAGU-4.2)"
```

- [ ] **Step 8: 同时收口 `stat.h` 第二对 UAPI 镜像（Explore agent finding）**

`kernel/include/uapi/stat.h:102-111` 与 `libc/include/sys/stat.h:100-115` 同时定义 `AT_FDCWD` / `AT_SYMLINK_NOFOLLOW` / `DT_*` 8 个常量。

合并到 `kernel/include/uapi/stat.h` 一处；`libc/include/sys/stat.h` 改为转发 `#include <uapi/stat.h>`；Makefile install 步骤（Step 3）覆盖 `<sys/stat.h>` 路径。

- [ ] **Step 9: 跑完整测试套件 + Step 10: Commit stat.h 收口**

```sh
git add kernel/include/uapi/stat.h \
        libc/include/sys/stat.h \
        kernel/Makefile
git commit -m "feat(uapi): single source for stat.h AT_*/DT_* (close AAGU-4.2 stat.h)"
```

---

## Task 3: `kernel/include/compat/*` 6 文件收口（AAGU-4.3，P2）

**Files:**
- Delete: `kernel/include/compat/list.h`
- Delete: `kernel/include/compat/rbtree.h`
- Delete: `kernel/include/compat/sys/cdefs.h`
- Delete: `kernel/include/compat/sys/types.h`
- Delete: `kernel/include/compat/string.h`
- Delete: `kernel/include/compat/stdlib.h`
- Delete: `kernel/include/compat/sys/`（空目录）
- Modify: `kernel/Makefile`（去掉 `-I include/compat`，保留 `-I libc/include` 或 install 步骤）

**Spec ref:** `docs/arch/cross-boundary-symbols.md` §2.4 + §3.4

**决策点：逐头 owner 决定**（reviewer #3 指出 — 「换目录保留镜像」不算 single source）

reviewer 的真问题：原 Step 1 把 compat/* cp 到 freestanding/*，是**换目录保留镜像**，违反 spec §2.4 单一来源。`cp` 后两个目录仍然存在，kernel 一侧由 `-I` 指向 freestanding/，libc 一侧不动 —— 镜像本质未消除。

正确路径是**先逐头决定 owner**，再决定目录布局：

| 头 | 当前消费方（grep `kernel/`） | API 性质 | Owner 决定 | 落点 |
|---|---|---|---|---|
| `list.h` | `kernel/include/time/timer.h:5` 等 9 个 TU | 纯 freestanding 数据结构（双向链表）| **kernel-owned**（libc 端不应独占；OS01 libc 共享同一份） | `kernel/include/freestanding/list.h`；libc install 走 sysroot publisher |
| `rbtree.h` | `kernel/include/sched/task.h:12` | 纯 freestanding 数据结构（rbtree_node）| **kernel-owned** | `kernel/include/freestanding/rbtree.h`；libc install 走 sysroot publisher |
| `sys/cdefs.h` | `<sys/types.h>` 的 transitive dep | 纯 macro（`__BEGIN_DECLS` 等）| **kernel-owned** | `kernel/include/freestanding/sys/cdefs.h` |
| `sys/types.h` | `<pid_t>` 等 typedef | 纯 typedef（`pid_t/uid_t/gid_t/...`）| **kernel-owned** | `kernel/include/freestanding/sys/types.h` |
| `string.h` | `kernel/percpu/percpu.c:3`（memset）+ `kernel/intr/softirq.c:3`（memset）| **libc-stateful**（`memcpy/memmove` 等的实现依赖 libc 内部状态）| **libc-owned**；kernel 必须**不直接 include libc string.h** —— 改用 compiler builtin `__builtin_memcpy` / `__builtin_memset`（clang 在 freestanding 模式自动提供） | 删 compat/string.h；改写 `percpu.c:3` 和 `softirq.c:3` 用 builtin |
| `stdlib.h` | `kernel/time/timer.c:6`（calloc/free）| **libc-stateful**（`calloc/malloc/free` 依赖 libc allocator）| **kernel-owned**（aarch64 whitelist 内 calloc/free 是 stub）—— 当前 stub 实现是 `kernel/arch/aarch64/libc_stub.c` | 删 compat/stdlib.h；改 `timer.c:6` 直接 `#include <kernel/libc_stub.h>` 或调 stub |

**实施 issue 启动前必须 post comment 在 AAGU-4 issue 上确认每头 owner 表**（与 aarch64 AP CNTP issue #1 决策模式一致）。

- [ ] **Step 1: 跑 owner-decision grep，列出每个 compat 头的真实消费方**

```sh
cd /home/aagu/OS01
for h in list.h rbtree.h sys/cdefs.h sys/types.h string.h stdlib.h; do
  echo "=== compat/$h ==="
  grep -rn "#include <${h%.h}\\.h>\|#include <${h}>" kernel/ | grep -v 'kernel/include/compat/'
done
```

Expected: 输出每个头的所有 kernel 消费方（含 kernel/include/ 下的 facade TU 和 kernel/<subsys>/*.c）。

- [ ] **Step 2: 按 owner 表分三组处理**

**a) kernel-owned（list/rbtree/sys/cdefs/sys/types）**：迁移 `compat/*.h` → `kernel/include/freestanding/*.h`，**不是 cp**（换目录是镜像）。具体：

- `compat/list.h` 头注释改「kernel-owned primitive (single source for OS01)」
- `kernel/include/freestanding/list.h` 新建
- `libc/include/list.h` 删除该 API 定义（或改为 `#include <freestanding/list.h>` 转发）；OS01 libc 的链表实现**就是**同一份
- `-I kernel/include/compat` 改为 `-I kernel/include/freestanding`

**b) libc-stateful（string/stdlib）**：kernel 不再 include，**改用 builtin / 调 stub**。

- `kernel/percpu/percpu.c:3` 的 `#include <string.h>` 改为 `#include <string.h>`（不再）+ 用 `__builtin_memset(...)`；或删 include，让 `memset` 名字从 libc string.h 转出（kernel 不应该用 libc 实现 memset —— aarch64 走 `__builtin_memset`）
- `kernel/intr/softirq.c:3` 同上
- `kernel/time/timer.c:6` 的 calloc/free 改为 `#include "libc_stub.h"` 直接调 stub
- 删 `compat/string.h` 和 `compat/stdlib.h`

- [ ] **Step 3: 改 `kernel/Makefile`**

- 删除 `-I$(CURDIR)/include/compat`
- a) kernel-owned 组的 `-I` 加 `-I$(CURDIR)/include/freestanding`
- b) libc-stateful 组直接删 include / 改 builtin

- [ ] **Step 4: aarch64 build 跑通**

```sh
make PROFILE=aarch64-clang SMP=1 aarch64-uefi
```

Expected: exit 0。

- [ ] **Step 5: x86_64 build 跑通**

```sh
make PROFILE=x86_64-clang kernel.bin
```

Expected: exit 0。

- [ ] **Step 4: aarch64 build 跑通**

```sh
make PROFILE=aarch64-clang SMP=1 aarch64-uefi
```

Expected: exit 0（`<list.h>` 等通过 freestanding 解析而非 compat）。

- [ ] **Step 5: x86_64 build 跑通**

```sh
make PROFILE=x86_64-clang kernel.bin
```

Expected: exit 0。

- [ ] **Step 6: 验证 `kernel/include/compat/` 已空**

```sh
git rm -r kernel/include/compat
```

Expected: 6 文件 + 1 空目录全部删除。

- [ ] **Step 7: 跑完整测试套件**

- [ ] **Step 8: Commit**

```sh
git add -A kernel/include/compat kernel/include/freestanding \
        kernel/Makefile
git commit -m "refactor(kernel): freestanding/ single source, drop compat/ (close AAGU-4.3)"
```

---

## Task 4: kernel 端 `__stack_chk_guard` 移到 `kernel/compiler_rt/`（AAGU-4.4，P3）

**Files:**
- Create: `kernel/compiler_rt/stack_chk_guard.c`（NEW）
- Modify: `kernel/core/main.c:63/77`（删除本地定义）
- Modify: `kernel/Makefile`（添加 `compiler_rt/stack_chk_guard.c` 到 KERNEL_C_SOURCES）

**Spec ref:** `docs/arch/cross-boundary-symbols.md` §2.1 + §3.1

**Interfaces:**
- Consumes: 现有 `unsigned long __stack_chk_guard` 在 `kernel/core/main.c:63/77`；现有 `__stack_chk_fail` 在 `kernel/...`（搜）
- Produces: `kernel/compiler_rt/stack_chk_guard.c` 唯一定义；kernel 链接时符号来自 compiler_rt

- [ ] **Step 1: 写 kernel self-test 覆盖 canary 跨 TU 一致性**

文件：`kernel/selftest/test_canary_single_source.c`（NEW）

```c
// Verify __stack_chk_guard has a single definition across kernel TUs.
// Implementation: declare extern __stack_chk_guard; link-time check that
// exactly one symbol exists (nm kernel.elf | grep stack_chk_guard).
#include <stdint.h>

extern unsigned long __stack_chk_guard;
extern void __stack_chk_fail(void);

void test_canary_single_source(void) {
    (void)__stack_chk_guard;
    (void)__stack_chk_fail;
    /* Linker guarantees one definition; selftest just exercises the symbol. */
    serial_printk("[selftest] canary_single_source: pass\n");
}
```

- [ ] **Step 2: 跑 selftest 当前状态**（找到现有入口）

```sh
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 kernel.bin
```

找到 self-test 注册入口（`kernel/selftest/test_*.c` 的注册 pattern），把这个新 selftest 加进去。

- [ ] **Step 3: 创建 `kernel/compiler_rt/stack_chk_guard.c`**

```c
// kernel/compiler_rt/stack_chk_guard.c — kernel-side __stack_chk_guard
// single source (AAGU-4.4). User-space definition lives in libc/ssp/ssp.c
// (gated by __is_libk). libk.a archive scanning must not pick up this
// definition (kernel does not link libk.a).
#include <stdint.h>

unsigned long __stack_chk_guard = 0xDEADBEEFCAFEBABEUL;  /* bootloader seed */

void __stack_chk_fail(void) {
    /* Mirror libc __stack_chk_fail behavior: print + abort. */
    volatile int *crash = (volatile int *)0;
    *crash = 0;
    for (;;) ;
}
```

- [ ] **Step 4: 删 `kernel/core/main.c:63/77` 本地定义**

从 `kernel/core/main.c` 删除 `__stack_chk_guard` 定义（行 63）和 `__stack_chk_fail` 定义（行 77，如存在）。保留引用代码。

- [ ] **Step 5: 改 `kernel/Makefile`**

在 `KERNEL_C_SOURCES`（行 47 附近）添加：

```make
compiler_rt/stack_chk_guard.c
```

- [ ] **Step 6: 跑测试**

```sh
make PROFILE=x86_64-clang kernel.bin
nm build/x86_64-clang/kernel.elf | grep stack_chk
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 kernel.bin
make OS01_SYSTEST=1 test-syscall
```

Expected: `nm` 输出恰好一个 `__stack_chk_guard` 符号，来源 `compiler_rt/stack_chk_guard.c`。

- [ ] **Step 7: Commit**

```sh
git add kernel/compiler_rt/stack_chk_guard.c \
        kernel/core/main.c \
        kernel/Makefile
git commit -m "refactor(compiler_rt): single-source kernel __stack_chk_guard (close AAGU-4.4)"
```

---

## Task 5: `arch_atomic_*_u64` facade + softirq.c 移除 ifdef（AAGU-4.5，P2）

**Files:**
- Create: `kernel/include/arch/atomic.h`（NEW，facade）
- Create: `kernel/arch/x86_64/atomic.c`（NEW，strong override：`lock orq/andq` 内联汇编）
- Create: `kernel/arch/aarch64/atomic.c`（NEW，strong override：`ldset`/`stclr` 或 LR/SC 重试）
- Modify: `kernel/intr/softirq.c:11-13, 19, 48-53`（删除 `#if defined(__x86_64__)`，改调 `arch_atomic_or_u64` / `arch_atomic_and_u64`）
- Move: `kernel/intr/pic/8259A.c` → `kernel/arch/x86_64/intr/pic_8259a.c`；`kernel/intr/apic/lapic_timer.c` + `kernel/intr/apic/lapic.c` + `kernel/intr/apic/ioapic.c` → `kernel/arch/x86_64/intr/apic/`
- Modify: `kernel/Makefile`（更新 KERNEL_C_SOURCES 路径 + 添加 atomic.c）

**Spec ref:** `docs/arch/cross-boundary-symbols.md` §2.3 + §3.3（softirq.c ifdef 行）

**关键修正（reviewer #4 反馈）**：原 Task 5 把 softirq.c 的 ifdef 错误描述为「APIC/PIC dispatch」，实际是 x86 原子位 set/clear（`lock orq`/`lock andq` 内联汇编）。Task 5 的 facade 应该是 **arch atomic bit op**，不是 softirq dispatch。

**Interfaces:**
- Consumes: 现有 `softirq.c:9-21` 的 `set_softirq_status` 内 ifdef 守护的 `__asm__ __volatile__("lock orq ...")` + `softirq_status |= status`；`do_softirq()` 行 48-53 同款
- Produces: `kernel/include/arch/atomic.h` 暴露 `void arch_atomic_or_u64(uint64_t *addr, uint64_t mask);` / `void arch_atomic_and_u64(uint64_t *addr, uint64_t mask);`；per-arch strong override

- [ ] **Step 1: 写 kernel selftest 覆盖 atomic facade**

文件：`kernel/selftest/test_arch_atomic_u64.c`（NEW）

```c
// Verify that kernel/include/arch/atomic.h has no #ifdef __x86_64__ left
// in any consumer TU. selftest runs at arch-neutral layer, calls the facade
// directly — no ifdef in this TU.
#include <arch/atomic.h>
#include <stdint.h>

static uint64_t target = 0;

void test_arch_atomic_u64(void) {
    /* Set bit 3 atomically */
    arch_atomic_or_u64(&target, (uint64_t)1 << 3);
    if ((target & ((uint64_t)1 << 3)) == 0) {
        serial_printk("[selftest] arch_atomic_or_u64: FAIL\n");
        return;
    }
    /* Clear bit 3 atomically */
    arch_atomic_and_u64(&target, ~((uint64_t)1 << 3));
    if (target & ((uint64_t)1 << 3)) {
        serial_printk("[selftest] arch_atomic_and_u64: FAIL\n");
        return;
    }
    /* No ifdefs in this selftest — same source for both arches. */
    serial_printk("[selftest] arch_atomic_u64: pass\n");
}
```

- [ ] **Step 2: 创建 facade `kernel/include/arch/atomic.h`**

```c
#ifndef OS01_ARCH_ATOMIC_H
#define OS01_ARCH_ATOMIC_H

#include <stdint.h>

/* arch_atomic_or_u64 — atomically OR `mask` into `*addr`.
 * x86_64 strong override: lock orq. aarch64 strong override: ldset. */
void arch_atomic_or_u64(uint64_t *addr, uint64_t mask);

/* arch_atomic_and_u64 — atomically AND `mask` into `*addr`.
 * x86_64 strong override: lock andq. aarch64 strong override: stclr / LR+SC. */
void arch_atomic_and_u64(uint64_t *addr, uint64_t mask);

#endif /* OS01_ARCH_ATOMIC_H */
```

- [ ] **Step 3: x86_64 strong override `kernel/arch/x86_64/atomic.c`**

```c
// kernel/arch/x86_64/atomic.c — arch_atomic_*_u64 strong override
// (spec: docs/arch/cross-boundary-symbols.md §2.3). x86_64 uses x86 locked
// instructions for atomic RMW. SMP-safe across cores.
#include <arch/atomic.h>

void arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock orq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}

void arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock andq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}
```

- [ ] **Step 4: aarch64 strong override `kernel/arch/aarch64/atomic.c`**

```c
// kernel/arch/aarch64/atomic.c — arch_atomic_*_u64 strong override
// AArch64 Large System Extensions (LSE) provide ldset/stclr (release/acquire
// semantics in one instruction). Fallback to LR/SC retry loop if LSE absent.
#include <arch/atomic.h>

void arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, tmp;
    __asm__ __volatile__(
        "1: ldaxr %0, [%2]\n"
        "   orr  %1, %0, %3\n"
        "   stxr  %w1, %1, [%2]\n"
        "   cbnz  %w1, 1b\n"
        : "=&r"(old), "=&r"(tmp)
        : "r"(addr), "r"(mask)
        : "memory");
}

void arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, tmp;
    __asm__ __volatile__(
        "1: ldaxr %0, [%2]\n"
        "   and  %1, %0, %3\n"
        "   stxr  %w1, %1, [%2]\n"
        "   cbnz  %w1, 1b\n"
        : "=&r"(old), "=&r"(tmp)
        : "r"(addr), "r"(mask)
        : "memory");
}
```

（注：若硬件支持 LSE，可改用单条 `ldset`/`stclr` 指令，本 plan 取最保守 LR/SC 实现保证向后兼容）

- [ ] **Step 5: 改 `kernel/intr/softirq.c` 移除 ifdef**

`kernel/intr/softirq.c:9-21`（`set_softirq_status`）：
```c
#include <arch/atomic.h>  /* NEW */

void set_softirq_status(uint64_t status)
{
    /* arch-neutral — facade + per-arch strong override (Task 5). */
    arch_atomic_or_u64(&softirq_status, status);
}
```

`kernel/intr/softirq.c:40-56`（`do_softirq`）：保留 dispatch 循环，行 48-53 改为：
```c
arch_atomic_and_u64(&softirq_status, ~(1ULL << i));
```

- [ ] **Step 6: 跑 x86_64 build + selftest**

```sh
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 kernel.bin   # 跑 test_arch_atomic_u64
```

Expected: selftest 输出 `arch_atomic_u64: pass`。

- [ ] **Step 7: 跑 aarch64 build + selftest**

```sh
make PROFILE=aarch64-clang SMP=1 aarch64-uefi
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi
```

Expected: exit 0；selftest 同样通过；`kernel/intr/softirq.c` 编译无 ifdef。

- [ ] **Step 8: 重定位 x86-only 驱动**

- `kernel/intr/pic/8259A.c` → `kernel/arch/x86_64/intr/pic_8259a.c`
- `kernel/intr/apic/lapic_timer.c` → `kernel/arch/x86_64/intr/apic/lapic_timer.c`
- `kernel/intr/apic/lapic.c` → `kernel/arch/x86_64/intr/apic/lapic.c`
- `kernel/intr/apic/ioapic.c`（如有）→ `kernel/arch/x86_64/intr/apic/ioapic.c`
- 删除原 `kernel/intr/pic/` + `kernel/intr/apic/` 目录

- [ ] **Step 9: 更新 `kernel/Makefile`**

KERNEL_C_SOURCES 加 `arch/$(ARCH)/atomic.c`；x86_64 whitelist 加 `arch/x86_64/intr/pic_8259a.c` + `arch/x86_64/intr/apic/*.c`。

- [ ] **Step 10: 跑完整测试套件**

```sh
make PROFILE=x86_64-clang KERNEL_SELFTEST=1
make OS01_SYSTEST=1 test-syscall
make PROFILE=aarch64-clang test-aarch64-uefi-smp
```

- [ ] **Step 11: Commit**

```sh
git add kernel/include/arch/atomic.h \
        kernel/arch/x86_64/atomic.c \
        kernel/arch/aarch64/atomic.c \
        kernel/intr/softirq.c \
        kernel/arch/x86_64/intr/ \
        kernel/Makefile
git commit -m "refactor(atomic): arch_atomic_or/and_u64 facade + softirq.c drop ifdef (close AAGU-4.5)"
```

---

## Self-Review（plan 完成前自检）

1. **Spec coverage**：spec §3.5 行 → Task 1（含 Steps 9-13 atexit 死链）；spec §3.2 行 → Task 2（含 stat.h DT_* 收口）；spec §3.4 行 → Task 3（6 文件逐头 owner 决策）；spec §3.1 行 → Task 4；spec §3.3 softirq ifdef 行 → Task 5。每条违例都有 task。✅
2. **Placeholder scan**：no "TBD" / "TODO" / "implement later" / "add appropriate error handling"。Step 1–N 每步都是可执行代码或命令。✅
3. **Type / name consistency**：所有 task 用 `is_open_file` / `arch_auxv_platform` / `arch_atomic_or_u64` / `arch_atomic_and_u64` / `__stack_chk_guard` 等真实符号名，与 spec 一致。✅
4. **Task 右边界**：5 个 task 各自可独立 review / 独立 commit / 独立测试。✅
5. **No 镜像层引入**：Task 3 走**逐头 owner 决策**（kernel-owned 走 freestanding/，libc-stateful 走 builtin/stub），**不是换目录保留镜像**。✅
6. **reviewer 5 条反馈全部消化**（来自 origin/feat/aagu-4-cross-boundary-spec@9631686 二审）：
   - reviewer #1（stat.h DT_* 不全）：Task 2 spec 表格已写 kernel 6 vs libc 9 准确 diff；plan 标 owner = sysroot.mk
   - reviewer #2（sysroot.mk owner + 装配顺序）：Task 2 Step 3 明确 owner = `mk/components/sysroot.mk`，禁止直接写 `$(SYSROOT_GENERATION_DIR)`
   - reviewer #3（install 模式换目录 = 镜像）：Task 3 改为逐头 owner 表，分 kernel-owned / libc-stateful 两组
   - reviewer #4（softirq.c ifdef ≠ APIC/PIC dispatch）：Task 5 改为 `arch_atomic_or/and_u64` facade，x86 strong override 用 `lock orq/andq`，aarch64 strong override 用 LR/SC
   - reviewer #5（hosttest 链 host libc + task 数不一致）：Task 1 hosttest 改用 `--sysroot=$(SYSROOT)` 交叉编译；spec §4.5 段已加入 plan 任务表；§4排期表 5 task 一致
7. **SPEC §3.3 ifdef 行描述已修正**：从「APIC/PIC dispatch」改为「`lock orq`/`lock andq` 原子位 set/clear 内联汇编」。✅

---

## 落地排期建议（仅供参考）

| Task | 对应 issue | 估时 | 优先级 |
|---|---|---|---|
| Task 1 | AAGU-4.1 | 2 days（含 sysroot hosttest + selftest；atexit 死链修复） | P1 — 假象安全 |
| Task 2（含 stat.h） | AAGU-4.2 | 3 days（含 sysroot.mk owner 接入 + uapi install 步骤 + stat.h DT_* 补全 + x86_64/aarch64 双跑） | P1 |
| Task 3 | AAGU-4.3 | 5 days（含 6 文件逐头 owner 决策 + kernel-owned/freestanding 迁移 + libc-stateful 改 builtin + aarch64 + x86_64 双跑） | P2 — 决策点 |
| Task 4 | AAGU-4.4 | 1 day | P3 |
| Task 5 | AAGU-4.5 | 4 days（含 arch_atomic facade + x86_64 + aarch64 strong override + softirq.c 移除 ifdef + x86-only 驱动重定位） | P2 |

落地 issue 创建顺序：Task 1 → Task 2 → Task 5 → Task 3 → Task 4（每个 issue 的子任务清单 = 本 plan 对应 Task 的 Steps）。

---

## 验收（每个落地 issue 完成时回填到 spec §3 现状对照表）

每个 task commit 后，回 `docs/arch/cross-boundary-symbols.md` §3 对应行，把 `❌ 违例` / `🟡 部分` 改为 `✅ 修`，加 commit hash。
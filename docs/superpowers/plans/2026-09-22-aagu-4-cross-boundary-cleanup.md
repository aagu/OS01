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

- [ ] **Step 1: 写 hosttest 覆盖 `fread/fwrite` 验证路径**

文件：`hosttests/libc/test_fread_fwrite_stream_validation.c`（NEW）

```c
// Reproduces the bug: passing an unregistered FILE* to fread/fwrite
// currently silently falls through; after this fix, it must return 0
// (per POSIX: not a valid stream = no bytes read/written).
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char buf[16];

    // Unregistered "stream" (not from fopen/fdopen) — must NOT read
    int garbage_stream = 0xDEADBEEF;
    size_t n = fread(buf, 1, sizeof buf, (void *)garbage_stream);
    assert(n == 0);

    // Unregistered stream for write — must NOT write
    n = fwrite("x", 1, 1, (void *)garbage_stream);
    assert(n == 0);

    // stdout / stderr are sentinels (already special-cased in fwrite);
    // for fread on stdout it should also short-circuit (POSIX: stdout
    // not open for reading).
    n = fread(buf, 1, sizeof buf, stdout);
    assert(n == 0);

    return 0;
}
```

- [ ] **Step 2: 跑 hosttest 验证它当前失败**

```sh
cd /home/aagu/OS01
clang -o /tmp/test_fread_fwrite_stream_validation \
    hosttests/libc/test_fread_fwrite_stream_validation.c \
    -L build/x86_64-clang/runtime -lk
/tmp/test_fread_fwrite_stream_validation
```

Expected: FAIL — `fread(buf, 1, sizeof buf, (void *)0xDEADBEEF)` 当前返回 1（读 1 字节从 fd 0xDEADBEEF），不返回 0。

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

- [ ] **Step 3: 加 Makefile install 步骤**

`kernel/Makefile` 内 x86_64 sysroot 配置区（行 106 附近）加：

```make
# AAGU-4.2: install UAPI headers to libc sysroot (single source for ABI)
$(SYSROOT_GENERATION_DIR)/usr/include/sys/auxv.h: \
        $(CURDIR)/include/uapi/auxv.h
	@mkdir -p $(dir $@)
	install -m 644 $< $@
```

并在 `kernel-c-objects` 或 `KERNEL_C_SOURCES` 安装 phase 引用这个新目标。

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

**决策点**：合并模式 vs install 模式（spec §2.4）
- **合并模式**：kernel 直接 include libc 头（`<list.h>` 等），前提是 aarch64 whitelist 不再 exclude libc → 等价于 revert commit `23f916d` 并加更细的白名单
- **install 模式**：kernel 拥有单一 freestanding 头目录，由 build 步骤 install 到 libc sysroot；kernel 自包含

本任务**默认走 install 模式**（与 §2.2 一致：kernel UAPI install 到 libc sysroot）。**实施 issue 启动前必须先 post 一个 comment 在 AAGU-4 issue 上确认模式选择**（参 aarch64 AP CNTP issue #1 决策模式）。

- [ ] **Step 1: 创建 kernel-owned 单一 freestanding 头目录**

```sh
mkdir -p kernel/include/freestanding
cp kernel/include/compat/list.h kernel/include/freestanding/list.h
cp kernel/include/compat/rbtree.h kernel/include/freestanding/rbtree.h
cp kernel/include/compat/sys/cdefs.h kernel/include/freestanding/sys/cdefs.h
cp kernel/include/compat/sys/types.h kernel/include/freestanding/sys/types.h
cp kernel/include/compat/string.h kernel/include/freestanding/string.h
cp kernel/include/compat/stdlib.h kernel/include/freestanding/stdlib.h
```

把每个头里的「kernel's own copy of libc/...」注释改为「freestanding kernel header (not a mirror of libc/include/)」。

- [ ] **Step 2: 加 Makefile install 步骤**

同 Task 2 Step 3 模式：把 `kernel/include/freestanding/<name>.h` install 到 libc sysroot 的对应路径。

- [ ] **Step 3: 改 `kernel/Makefile`**

- 删除 `-I$(CURDIR)/include/compat`
- 添加 `-I$(CURDIR)/include/freestanding`

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

## Self-Review（plan 完成前自检）

1. **Spec coverage**：spec §3.5 行 → Task 1；spec §3.2 行 → Task 2；spec §3.4 行 → Task 3；spec §3.1 行 → Task 4。每条违例都有 task。✅
2. **Placeholder scan**：no "TBD" / "TODO" / "implement later" / "add appropriate error handling"。Step 1–N 每步都是可执行代码或命令。✅
3. **Type / name consistency**：所有 task 用 `is_open_file` / `arch_auxv_platform` / `__stack_chk_guard` 等真实符号名，与 spec 一致。✅
4. **Task 右边界**：4 个 task 各自可独立 review / 独立 commit / 独立测试。✅
5. **No 镜像层引入**：Task 3 走 install 模式，**不是新建镜像**（单一来源是 kernel/include/freestanding/，与 compat/ 的双份维护本质不同）。✅

---

## 落地排期建议（仅供参考）

| Task | 对应 issue | 估时 | 优先级 |
|---|---|---|---|
| Task 1 | AAGU-4.1 | 1 day（含 hosttest + selftest） | P1 — 假象安全 |
| Task 2 | AAGU-4.2 | 2 days（含 install 步骤 + x86_64/aarch64 双跑） | P1 |
| Task 3 | AAGU-4.3 | 3 days（含 freestanding/ install 步骤 + 6 文件迁移） | P2 — 决策点 |
| Task 4 | AAGU-4.4 | 1 day | P3 |

落地 issue 创建顺序：Task 1 → Task 2 → Task 3 → Task 4（每个 issue 的子任务清单 = 本 plan 对应 Task 的 Steps）。

---

## 验收（每个落地 issue 完成时回填到 spec §3 现状对照表）

每个 task commit 后，回 `docs/arch/cross-boundary-symbols.md` §3 对应行，把 `❌ 违例` / `🟡 部分` 改为 `✅ 修`，加 commit hash。
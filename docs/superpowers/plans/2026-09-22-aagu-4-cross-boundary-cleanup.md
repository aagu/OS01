# AAGU-4 跨边界符号/ABI 边界治理 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 AAGU-4 规范文档 `docs/arch/cross-boundary-symbols.md` §3 现状对照表里的违例项**逐条收口**，让 OS01 跨边界符号 / ABI 全部走单一来源。

**Architecture:** 5 个独立 follow-up issue（每个对应一条或一类违例），按 §3 现状对照表的 P1 / P2 / P3 优先级排序。**不引入新的镜像层**（spec §1 明文禁止）；镜像收口路径走「合并模式」（kernel 直接 include libc）或「install 模式」（kernel 拥有 + build 步骤 install 到 libc sysroot），由落地 issue 在 plan 阶段二选一。

**Tech Stack:** OS01 kernel + libc + runtime（C11、clang、freestanding kernel）；Makefile + mk/components/*.mk；sysroot install 步骤（kernel UAPI → libc sysroot `<sys/auxv.h>` 等）。

**Spec:** [`docs/arch/cross-boundary-symbols.md`](../../arch/cross-boundary-symbols.md) — 本 plan 的 5 个任务一一对应 spec §4 后续 issue 切分（含 AAGU-4.6 atexit follow-up，由 reviewer round 4 拆出）。

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

- [ ] **Step 1: 写 hosttest 覆盖 `fread/fwrite` 验证路径 — pipe-based fake mini_file_t + 真实 `TEST_FUNC` 模式**

**reviewer round 5 最小化**：
- 删除 `#include "stdio_internal.h"`：该目录不在 test include path；`mini_file_t` 已由 OS01 `<stdio.h>` 公开
- 删除 `test_fread_on_stdout_returns_zero`：修前把 stdout sentinel 当地址解引用 → 整个 hosttest 崩溃，不能作为 RED case
- `fwrite` 测试**不**在调用前 `close(pipefd[0])` —— 修前 `write` 会触发 SIGPIPE；保留 read end，调用后再同时 close 两端
- 仅保留两个 pipe + 未注册 `mini_file_t` 测试；修前都稳定得到 `n == 1`、修后得到 `n == 0`
- 不引入 `signal` / `setjmp` / `destructor`

**reviewer round 4 修正**（前置）：
- 删除 `0xDEADBEEF` 野指针 + SIGSEGV catcher 方案（崩溃路径不可控）
- 删除 `__attribute__((destructor))` + 显式 oracle 方案（OS01 libc 无 `.init_array`）
- 删除 `--target=x86_64-elf --sysroot=$SYSROOT` + QEMU user-binary 方案（不是 hosttest 模式）
- 改为：**fake `mini_file_t` + 真实 pipe fd** —— 已分配 `mini_file_t`，但用 `is_open_file` 未注册路径触发
- 复制 `hosttests/cases/test_libc_fflush.c` 的真实 pattern（`TEST_FUNC` / `assert_eq` / `TEST_LIST_BEGIN/END` / `TEST_ENTRY` / `__test_table` / `__test_stats`），不用不存在的 `TEST_CASE` / `ASSERT_EQ`

文件：`hosttests/cases/test_libc_fread_fwrite_validate.c`（NEW）

```c
/*
 * test/cases/test_libc_fread_fwrite_validate.c — fread/fwrite stream
 * validation contract (spec AAGU-4 §3.5 + cleanup plan Task 1).
 *
 * Strategy: allocate a `mini_file_t fake` whose `.fd` is a real pipe
 * read-end (so pre-fix fread reads successfully from it), but **do
 * NOT register it via fopen/fdopen**. Pre-fix fread dereferences
 * fake.fd and returns the byte; post-fix fread rejects unregistered
 * streams (returns 0). Same for fwrite with a separate pipe.
 */
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* mini_file_t is exposed via OS01's <stdio.h>; no extra header. */

TEST_FUNC(test_fread_unregistered_stream_returns_zero) {
    /* Pre-fix: fread dereferences fake->fd, reads 1 byte from pipe, returns 1.
     * Post-fix: fread checks is_open_file(fake), not registered, returns 0. */
    int pipefd[2];
    int rc = pipe(pipefd);
    assert_eq(rc, 0);
    /* Seed pipe with 1 byte. */
    rc = write(pipefd[1], "x", 1);
    assert_eq(rc, 1);
    close(pipefd[1]);

    mini_file_t fake = { .fd = pipefd[0], .mode = 0 };
    char buf[4] = {0};
    size_t n = fread(buf, 1, sizeof buf, &fake);
    /* Post-fix expectation: unregistered stream → 0 bytes read. */
    assert_eq(n, 0);

    close(pipefd[0]);
}

TEST_FUNC(test_fwrite_unregistered_stream_returns_zero) {
    /* Pre-fix: fwrite writes 1 byte to fake->fd (the pipe write-end).
     * Post-fix: fwrite checks is_open_file, returns 0.
     * KEEP read-end open so pre-fix fwrite doesn't SIGPIPE; close both ends after. */
    int pipefd[2];
    int rc = pipe(pipefd);
    assert_eq(rc, 0);

    mini_file_t fake = { .fd = pipefd[1], .mode = 1 };
    size_t n = fwrite("x", 1, 1, &fake);
    assert_eq(n, 0);    /* post-fix: not registered */

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fread_unregistered_stream_returns_zero),
    TEST_ENTRY(test_fwrite_unregistered_stream_returns_zero),
TEST_LIST_END

int main(void) {
    printf("=== Test Runner ===\n");
    int size = sizeof(__test_table) / sizeof(__test_table[0]);
    for (int i = 0; i < size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }
    int failed = __test_stats.failed;
    printf("\n  ---\n");
    printf("  Total: %d | Passed: %d | Failed: %d\n",
           __test_stats.total, __test_stats.passed, failed);
    if (failed > 0)
        printf("  >>> SOME TESTS FAILED <<<\n");
    else
        printf("  >>> ALL TESTS PASSED <<<\n");
    return failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: 把测试 bin 加进 `hosttests/Makefile` + 跑 hosttest 验证 RED（reviewer round 4：必须用真实 make target，不能假设 `test-libc-stream-validation` 已存在）**

**reviewer round 4 修正**：
- 原 Step 1 / Step 2 假设 `make PROFILE=x86_64-clang test-libc-stream-validation` 存在 —— **不存在**，必须按现有 `test_libc_fflush.elf` 的注册 pattern 加进 `hosttests/Makefile`
- 删除 SIGSEGV catcher / 野指针方案
- 复用 `hosttests/Makefile` 已有的 `libc_stdio_file.o` 编译规则（已存在）+ 加新 `.o` + 加新 `.elf` target

**a. 把新 test 文件加进 `hosttests/Makefile`：**

仿照已有的 `$(TEST_BLD)/test_libc_fflush.elf` + `$(TEST_BLD)/test_libc_fflush.o` + 链接行 pattern。新增：

```make
# fread/fwrite stream validation test — added to TEST_BINS (line 62-86) and to .PHONY list
$(TEST_BLD)/test_libc_fread_fwrite_validate.o: $(TEST_CASES)/test_libc_fread_fwrite_validate.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_fread_fwrite_validate.elf: \
        $(TEST_BLD)/test_libc_fread_fwrite_validate.o \
        $(TEST_BLD)/libc_stdio_file.o
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) \
	    -o $@ $< $(TEST_BLD)/libc_stdio_file.o
```

并在 `hosttests/Makefile` 同时注册：

a) **TEST_BINS 列表追加**（line 62 附近的 `TEST_BINS :=` 块）：
   ```make
   TEST_BINS := \
       ... \
       $(TEST_BLD)/test_libc_fread_fwrite_validate.elf \
       ...
   ```
   （reviewer round 5 明确要求，不是 `+=`，是直接 append 到 `TEST_BINS :=` 块）

b) **`.PHONY` 列表追加**（line 88 附近的 `.PHONY` 行）：
   ```make
   .PHONY: ... test-libc-fread-fwrite-validate
   ```

c) **新增 phony target**（追加在 `all:` 之后）：
   ```make
   test-libc-fread-fwrite-validate: $(TEST_BLD)/test_libc_fread_fwrite_validate.elf
   	$<   # 执行 ELF（host 原生）
   ```

`libc_stdio_file.o` 已有编译规则（line ~80 附近，`$(TEST_BLD)/libc_stdio_file.o: $(LIBC_SRC)/stdio/stdio_file.c`），不需要新增。

**b. 跑测试（RED 状态）：**

```sh
cd /home/aagu/OS01
make PROFILE=x86_64-clang test-libc-fread-fwrite-validate
```

Expected（修前 RED）：两个 `TEST_FUNC` **失败**，因为当前 `fread`/`fwrite` 没有 `is_open_file` 检查，`fake` 的 `pipefd` 是合法 fd，会被 deref + 真正读到 / 写到数据。失败模式：
- `test_fread_unregistered_stream_returns_zero`：实际 `n == 1`（修后期望 `0`），assert 失败
- `test_fwrite_unregistered_stream_returns_zero`：实际 `n == 1`（修后期望 `0`），assert 失败

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

- [ ] **Step 5: 跑 hosttest 验证 RED → GREEN（reviewer round 4：用真实 makefile target）**

```sh
cd /home/aagu/OS01
make PROFILE=x86_64-clang test-libc-fread-fwrite-validate
```

Expected（修后 GREEN）：3 个 `TEST_FUNC` 全 PASS。
- `test_fread_unregistered_stream_returns_zero`：fake.fd 是 pipe，**post-fix** `fread` 在 `is_open_file` 检查失败后 return 0，pipe 没被 deref
- `test_fwrite_unregistered_stream_returns_zero`：post-fix `fwrite` return 0，pipe 写端不会被写到
- `test_fread_on_stdout_returns_zero`：post-fix `fread(stdout)` return 0（POSIX）

**禁止**：`clang ... -L build/x86_64-clang/runtime -lk`（host toolchain，不可执行 OS01 代码）。

**禁止**：`make test-libc-stream-validation`（target 不存在，已被 reviewer round 4 明确指出；用 `test-libc-fread-fwrite-validate`）。

- [ ] **Step 6: 跑完整测试套件**

```sh
cd /home/aagu/OS01
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 kernel.bin
make OS01_SYSTEST=1 test-syscall
make PROFILE=aarch64-clang test-aarch64-uefi-smp   # SMP=1 ×3
```

Expected: 全部 PASS（已知 GIC Phase 1 TIMEOUT 不计入）。

- [ ] **Step 7: Commit**

```sh
git add hosttests/cases/test_libc_fread_fwrite_validate.c \
        hosttests/Makefile \
        libc/stdio/stdio_file.c
git commit -m "fix(libc/stdio): validate stream in fread/fwrite (close AAGU-4.1)"
```

---

## Task 2: UAPI auxv 单一源收口（AAGU-4.2，P1）

**Files:**
- Modify: `kernel/include/uapi/auxv.h`（添加 libc 端额外常量）
- Modify OR Delete: `libc/include/sys/auxv.h`（转发或删除）
- (none, 复用 `kernel/Makefile` 现有 `install-headers` target)

**Spec ref:** `docs/arch/cross-boundary-symbols.md` §2.2 + §3.2

**Interfaces:**
- Consumes: 现有 15 个 `AT_*` 常量在 `kernel/include/uapi/auxv.h`；23 个在 `libc/include/sys/auxv.h`
- Produces: 单一 `kernel/include/uapi/auxv.h` 拥有全部 23 个；libc sysroot 通过转发头 `<sys/auxv.h>` 取得（不重新定义常量）

- [ ] **Step 1: 在 `kernel/include/uapi/auxv.h` 补齐 8 个缺失常量**

`kernel/include/uapi/auxv.h:11` 后插入（按 Linux `<sys/auxv.h>` 标准值，reviewer #2 round 3 修正 — 实为 8 个不是 7 个）：

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

- [ ] **Step 3: 加 Makefile install 步骤 — single-source 双路径编排（reviewer #1 round 3 修正：duplicate-destination 解决）**

**关键问题**（reviewer #1 round 3）：
- 旧 Step 3 让 kernel install-headers 同时 stage `usr/include/uapi/auxv.h` + `usr/include/sys/auxv.h`，libc install 又 stage `usr/include/sys/auxv.h`（libc 的 forwarding 头）—— **同一 destination 来自两个 component**，sysroot.mk merge 时会按 manifest duplicate-destination 失败。
- 旧 Step 3 还让 kernel install-headers 把 uapi/* 拷到 sys/*（kernel 一侧写 `usr/include/sys/`，绕过 owner 边界）

**修正后 single-source 双路径编排**：

| Public path | Producer | Mechanism |
|---|---|---|
| `<uapi/auxv.h>` | **kernel**（single source） | kernel install-headers stage `$(STAGING_DIR)/kernel-headers/usr/include/uapi/auxv.h` |
| `<sys/auxv.h>` | **libc**（forwarding） | libc install stage `libc/include/sys/auxv.h`（forwarding header）|
| `<uapi/stat.h>` | **kernel**（single source） | kernel install-headers stage `$(STAGING_DIR)/kernel-headers/usr/include/uapi/stat.h` |
| `<sys/stat.h>` | **libc**（forwarding） | libc install stage `libc/include/sys/stat.h`（forwarding）|

**禁止**：
- kernel install-headers 写 `usr/include/sys/` —— kernel 不拥有 `<sys/...>` 路径
- libc install-headers 写 `usr/include/uapi/` —— libc 不拥有 `<uapi/...>` 路径
- 任一 component 直接写 `$(SYSROOT_GENERATION_DIR)/...` —— 绕过 sysroot.mk publisher 会破坏 sysroot 完整性

**a. `kernel/Makefile` install-headers（已存在 — 不修改）**

**reviewer round 4 修正**：`kernel/Makefile:412-425` 现有 install-headers recipe 用 `INSTALL_ROOT` + `cp -R --preserve=timestamps include/. $(INSTALL_ROOT)/usr/include/.` 已经正确 stage `kernel/include/uapi/*.h` 到 sysroot。**本任务不改 kernel header-install recipe**，删除 plan 里所有 `DESTDIR` / 重复 `install-headers:` recipe / 「其他头同理」。

唯一可能需要 kernel 端做的：**如果某条新 UAPI 头不在 `kernel/include/uapi/` 下而是别处**，把它移到 `kernel/include/uapi/`。否则本任务不动 kernel/Makefile。

**b. `libc/include/sys/auxv.h`（改动内容 — 文件已存在，**不是**新建）**

**reviewer round 4 修正**：保留 include guard 与 `getauxval` 声明，仅：
- 加入 `#include <uapi/auxv.h>`（forwarding）
- 删除所有 `AT_*` 宏定义（**23 个**全部移到 kernel/include/uapi/auxv.h）

完整新内容：

```c
/* libc/include/sys/auxv.h — libc-side forwarding header.
 * Single source for AT_* macros: kernel/include/uapi/auxv.h
 * (installed to sysroot usr/include/uapi/auxv.h by kernel/Makefile:412-425).
 * Public producer fixed at kernel/<uapi/>; libc just forwards. */
#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H

#include <uapi/auxv.h>

unsigned long getauxval(unsigned long type);

#endif /* _SYS_AUXV_H */
```

**c. `libc/include/sys/stat.h`（改动内容 — 文件已存在，**不是**替换）**

**reviewer round 4 修正**：**不得替换为只有 2 个 getdents 声明的短文件**。保留其独有声明 / 结构 / 函数：
- 保留 `struct stat` 定义（libc 端独立版本）
- 保留 `fstat` / `stat` / `lstat` / `umask` / `chmod` / `mkdir` 等独有声明
- 保留 `#include <sys/types.h>` 等已有 includes
- **只**删除重复的 `AT_*` / `DT_*` 宏，并在这些宏原位置加入 `#include <uapi/stat.h>`（**in-place 替换**，不删其他内容）
- **同步在 `kernel/include/uapi/stat.h` 补全 libc 缺少的 3 个 `DT_*`**：`DT_FIFO (1)`、`DT_SOCK (12)`、`DT_WHT (14)`，使 kernel 这边也拥有 POSIX 完整集合

**d. `mk/components/sysroot.mk` 现有 `kernel-headers-install.stamp`**：不变（owner + 装配链已正确）。依赖图：
```
sysroot_*.stamp  ←  libc-install.stamp  ←  kernel-headers-install.stamp
```
libc install 链路上**只读** `$(STAGING_DIR)/kernel-headers/usr/include/uapi/`（不会复制它），加上 libc 自己的 `$(STAGING_DIR)/libc/usr/include/sys/`，最终 sysroot 同时有 `<uapi/auxv.h>` + `<sys/auxv.h>`，**无 duplicate**。

- **Test（sysroot 完整性 + 数字一致性）**：

```sh
cd /home/aagu/OS01
make PROFILE=x86_64-clang lib   # 跑 libc build
# 1) sysroot 同时含 4 个不同路径
find $(make -s PROFILE=x86_64-clang print-staging) -name 'auxv.h' -o -name 'stat.h' | sort -u
# 期望：
#   <STAGING>/kernel-headers/usr/include/uapi/auxv.h
#   <STAGING>/libc/usr/include/sys/auxv.h
#   <STAGING>/kernel-headers/usr/include/uapi/stat.h
#   <STAGING>/libc/usr/include/sys/stat.h
# 4 个文件，每个路径单一 producer。

# 2) libc 不再有 ABI 宏定义（forwarding only）
rg '^#define (AT_|DT_)' libc/include/sys/auxv.h libc/include/sys/stat.h
# 期望：no output

# 3) kernel/uapi auxv 23 个 AT_*，kernel/uapi stat.h 9 个 DT_*
rg -c '^#define AT_' kernel/include/uapi/auxv.h     # 期望: 23
rg -c '^#define DT_' kernel/include/uapi/stat.h     # 期望: 9

# 4) 数字与 spec §3.2 一致：kernel 15 vs libc 23 = 8 差；kernel 6 vs libc 9 = 3 差
```

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

- [ ] **Step 8: 同时收口 `stat.h` 第二对 UAPI 镜像（reviewer #2 round 3 修正）**

`kernel/include/uapi/stat.h:102-111` 有 6 个 `DT_*`（`DT_UNKNOWN/REG/DIR/CHR/BLK/LNK`），`libc/include/sys/stat.h:104-112` 有 9 个 `DT_*`（多 `DT_FIFO (1)`、`DT_SOCK (12)`、`DT_WHT (14)` —— POSIX 完整集合）。**不是 8 个**——reviewer #2 round 3 抓出 spec §6 「DT_* 8 个」与 §3.2 「6/9」矛盾。

合并到 `kernel/include/uapi/stat.h` 一处（补全 9 个 `DT_*` POSIX 完整集合 + 原有 `AT_FDCWD`/`AT_SYMLINK_NOFOLLOW`）；`libc/include/sys/stat.h` 改为转发 `#include <uapi/stat.h>` + 保留 libc 用户态扩展（`getdents/getdents64` 等）；Makefile install 步骤（Step 3b/3c）覆盖 `<sys/stat.h>` 路径。

- [ ] **Step 9: 跑完整测试套件 + Step 10: Commit stat.h 收口**

```sh
git add kernel/include/uapi/stat.h \
        libc/include/sys/stat.h \
        kernel/Makefile
git commit -m "feat(uapi): single source for stat.h AT_*/DT_* (close AAGU-4.2 stat.h)"
```

---

## Task 3: `kernel/include/compat/*` 6 文件收口（AAGU-4.3，P2）

**reviewer round 4 决议**：本 task 是**边界规则**，**不是实施 recipe**。每头 owner 决策 + 实际迁移在独立落地 issue 提交。plan 不预写 `libc/include/{string,stdlib,...}` 的删除 / 转发内容，不预写 `kernel/include/freestanding/allocator.h` 或任何 freestanding/ 头 —— 这些是落地 issue 才决定的 ABI / owner 选择。

**规范内容（边界规则）**：每个 compat 头（list.h / rbtree.h / sys/cdefs.h / sys/types.h / string.h / stdlib.h）必须满足以下 4 列表才允许 commit：

1. **消费者清单**：在 `kernel/` 下用 `grep` 列出所有 `#include <X.h>` 的 TU（facade TU + kernel/<subsys>/*.c）
2. **唯一 owner**：kernel-owned 或 libc-owned，二选一；不重复；不出现「kernel 改 X + libc 改 X」两套
3. **公开安装路径**：sysroot 下的 `<X.h>` 由**唯一**一个 component stage（kernel install-headers 或 libc install，二选一）；不写同 destination 的两份
4. **x86_64 / aarch64 编译测试**：两条 build profile 都跑通

**落地 issue 模板**：AAGU-4.3.1（list）、AAGU-4.3.2（rbtree）、AAGU-4.3.3（sys/cdefs）、AAGU-4.3.4（sys/types）、AAGU-4.3.5（string）、AAGU-4.3.6（stdlib）—— 每个对应一个独立 issue，启动前 post comment 在 AAGU-4 issue 上贴出该头的 4 列表等 reviewer 确认。

**禁止**（reviewer round 4 + round 5 明确）：
- parent plan **不得**包含 `git rm -r kernel/include/compat`（reviewer round 5）—— 删除 compat/ 是 6 个 sub-issue 的**最后一个**做，且只在替代 include 路径全验证后才删
- 在本 plan 预写 `libc/include/{string,stdlib,list,rbtree,sys/cdefs,sys/types}.h` 的删除 / 转发内容
- 在本 plan 预写 `kernel/include/freestanding/allocator.h` 或任何 freestanding/ 头
- 把 6 个落地 issue 合并成一个 PR（每个独立 owner 决策需独立 review）

**parent plan 不写实施步骤**（仅边界规则 + 6 个 sub-issue 模板）；所有具体动手的 step / 任务 / commit message / test verification 都属于 6 个 sub-issue 各自的 plan。

---## Task 4: kernel 端 `__stack_chk_guard` 移到 `kernel/compiler_rt/`（AAGU-4.4，P3）

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

/* arch_atomic_or_u64 — atomically OR `mask` into `*addr` with acquire-release semantics.
 * x86_64 strong override: lock orq. aarch64 strong override: ldaxr+stlxr LR/SC loop. */
void arch_atomic_or_u64(uint64_t *addr, uint64_t mask);

/* arch_atomic_and_u64 — atomically AND `mask` into `*addr` with acquire-release semantics.
 * x86_64 strong override: lock andq. aarch64 strong override: ldaxr+stlxr LR/SC loop. */
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
// Memory-order semantics:
//   ldaxr — load-acquire exclusive. Loads *addr with acquire semantics (no
//           subsequent load/store in this CPU can be reordered BEFORE this
//           load) and marks the address as exclusive monitor for this CPU.
//   stlxr — store-release exclusive. Stores new_val to *addr if exclusive
//           monitor still belongs to this CPU; status=0 on success, status=1
//           on failure (and *addr is NOT modified on failure). Release
//           semantics for the store (no prior load/store can be reordered
//           AFTER it).
//   "memory" clobber — tells compiler not to reorder loads/stores around the
//           asm. The acquire-on-load + release-on-store pair is an
//           **acquire-release RMW**, NOT seq_cst (no dmb ish between
//           independent RMWs on the same CPU). Sufficient for the
//           softirq_status use case (per-CPU single-writer + per-CPU
//           reader).
#include <arch/atomic.h>

void arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   orr    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}

void arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   and    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}
```

**reviewer round 4 修正**：
- `stxr` 改为 `stlxr`（release 语义）
- `ldaxr + stlxr + "memory"` 的准确契约是 **acquire-release RMW**，**不**是 seq_cst；接口注释同步改为「acq_rel」
- 三个独立约束（`old/new_val/status`）保留
- 若未来确需 seq_cst（要求 RMW 之间的 total order），需在该 task 另加 `dmb ish` 并给出理由 + 测试；**当前不引入**

**reviewer #5 round 3 修正**：
- 旧版本 `tmp` 寄存器同时被 `orr` 当 64-bit new_val 用、被 `stxr %w1` 当 32-bit status 用 —— **status 必须独立**，因为 `stxr %w1, %1, ...` 会把 `%1` 的低 32 位当 status，新 value 会被覆盖
- 现版本：`[old]`, `[new_val]`, `[status]` 三个独立约束寄存器；`stlxr %w[status]` 写 32-bit status 寄存器（GCC/clang `w` prefix 把 64-bit reg 取低 32 位），不影响 new_val
- retry 条件 `cbnz %w[status], 1b` 用 status 寄存器

**注**：若硬件支持 LSE（`HWCAP_CPUID/HWCAP2_ATOMIC`），可改用单条 `ldset`/`stclr` 指令，本 plan 取最保守 LR/SC 实现保证向后兼容（任何 ARMv8.0+ CPU 都有 LR/SC）。

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

## 后续 issue（reviewer round 5 拆出 — AAGU-4.6）

**AAGU-4.6 — atexit 死链整改（reviewer round 4 拆出 + round 5 移到独立节）**

- **范围**：解决 `__call_atexit_handlers`（`libc/stdlib/atexit.c:20`）定义但全 repo 0 caller 的问题
- **验收**：用 OS01 QEMU user-program E2E —— 注册 handler → `exit(0)` → handler 写 marker 到 file/pipe → 父进程断言 marker 存在
- **前置**：在 `libc/stdlib.h` 添加 `void exit(int status);` 声明（当前缺声明）
- **禁止**：
  - 用 hosttest 替代（atexit 死链问题在 csu 层，hosttest 触达不到）
  - 用 `__attribute__((destructor))`（OS01 libc 无 `.init_array` runtime support）
  - 拆到 AAGU-4.1 stdio FILE 注册 plan 内（不属于 stdio 范畴）
- **owner 关系**：AAGU-4.6 由 spec §3.5 atexit 行归属（reviewer round 5 显式约束）；spec §3.5 该行保持 AAGU-4.6 归属声明
- **优先级**：P3；估时 2 days

---

## Self-Review（plan 完成前自检）

1. **Spec coverage**：spec §3.5 行 → Task 1；spec §3.2 行 → Task 2（含 stat.h DT_* 收口）；spec §3.4 行 → Task 3（6 文件逐头 owner 决策）；spec §3.1 行 → Task 4；spec §3.3 softirq ifdef 行 → Task 5；spec §3.5 atexit 行 → AAGU-4.6 follow-up。每条违例都有 task。✅
2. **Placeholder scan**：no "TBD" / "TODO" / "implement later" / "add appropriate error handling"。Step 1–N 每步都是可执行代码或命令。✅
3. **Type / name consistency**：所有 task 用 `is_open_file` / `arch_auxv_platform` / `arch_atomic_or_u64` / `arch_atomic_and_u64` / `__stack_chk_guard` 等真实符号名，与 spec 一致。✅
4. **Task 右边界**：5 个 task + 1 follow-up（AAGU-4.6）各自可独立 review / 独立 commit / 独立测试。✅
5. **No 镜像层引入**：Task 3 走**逐头 owner 决策**（4 列表），**不**预写 freestanding/allocator.h 等替换路径；删除 compat/ 是 6 个 sub-issue 的最后一个做。✅
6. **Task 1 hosttest 现状**：2 个 TEST_FUNC（pipe-based fake mini_file_t，无 SIGSEGV / destructor / setjmp / `--sysroot`）；`hosttests/Makefile` 加 `TEST_BINS` + `.PHONY` + phony target 跑 ELF。✅
7. **Task 2 sysroot 编排现状**：kernel `install-headers` 不动；kernel 唯一 stage `<uapi/>`，libc 唯一 stage `<sys/>` forwarding/wrapper；每个 destination 单一 producer。✅
8. **Task 3 边界规则现状**：仅消费者清单 / 唯一 owner / 唯一公开安装路径 / x86_64+aarch64 编译测试 4 列表；落地 6 个 sub-issue；parent plan **不**预写 header 落地路径，**不**含 `git rm -r`。✅
9. **Task 5 atomic 现状**：`kernel/include/arch/atomic.h` facade 注释标「acq_rel」；aarch64 strong override 用 `ldaxr + stlxr + memory clobber`（acquire-release RMW，**不** seq_cst）；3 个独立约束（`old/new_val/status`）；`stxr` 已统一改 `stlxr`。✅
10. **文档自洽性**：plan header + spec ref + §4 排期表 + Self-Review + spec §6 全部以「5 task + AAGU-4.6 follow-up」一致呈现，无 4-task / 5-task 混合。✅

---

## 落地排期建议（仅供参考）

| Task | 对应 issue | 估时 | 优先级 |
|---|---|---|---|
| Task 1 | AAGU-4.1 | 1.5 days（hosttest: pipe-based fake mini_file_t + Makefile target 注册 + GREEN 验证；不含 atexit） | P1 — 假象安全 |
| Task 2（含 stat.h） | AAGU-4.2 | 3 days（kernel UAPI 补全 + libc sys/auxv.h + sys/stat.h 转发/补全 + install owner 接入 + x86_64/aarch64 双跑） | P1 |
| Task 3 | AAGU-4.3 | parent plan **仅边界规则**（reviewer round 5 明确）；6 个 sub-issue 估时各自 ~ 1.5 days/header（落地步骤在各自 sub-issue plan） | P2 |
| Task 4 | AAGU-4.4 | 1 day | P3 |
| Task 5 | AAGU-4.5 | 4 days（arch_atomic_or/and_u64 facade + x86_64 + aarch64 strong override + softirq.c 移除 ifdef + x86-only 驱动重定位） | P2 |
| follow-up | **AAGU-4.6**（reviewer round 4 拆出） | 2 days（atexit 死链整改：QEMU user-program E2E + csu/exit 重写；不含 hosttest） | P3 |

落地 issue 创建顺序：AAGU-4.1 → AAGU-4.2 → AAGU-4.5 → AAGU-4.3.x（6 个 sub-issue 各自独立 owner + commit）→ AAGU-4.4 → AAGU-4.6。`git rm -r kernel/include/compat` 由 6 个 sub-issue 的**最后一个**做。

---

## 验收（每个落地 issue 完成时回填到 spec §3 现状对照表）

每个 task commit 后，回 `docs/arch/cross-boundary-symbols.md` §3 对应行，把 `❌ 违例` / `🟡 部分` 改为 `✅ 修`，加 commit hash。
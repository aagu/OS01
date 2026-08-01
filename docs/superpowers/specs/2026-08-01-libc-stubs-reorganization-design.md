# libc Stub 文件拆分重构设计

**日期**: 2026-08-01
**状态**: 待实现

## 目标

消除 `libc/unistd/` 下 5 个 `*_stubs.c` 文件，将其中约 100 个函数按标准 header 归属分目录重新组织。顺带修复发现的 bug、统一 stub 语义、清理死代码和冗余头文件内容。

## 当前状态

`libc/unistd/` 下存在 5 个混合 stub 文件：

| 文件 | 行数 | 函数/符号数 |
|------|------|------------|
| `busybox_stubs.c` | 231 | 50 |
| `uid_stubs.c` | 29 | 20 |
| `net_stubs.c` | 53 | 13 |
| `misc_stubs.c` | 8 | 4 |
| `term_stubs.c` | 4 | 2 |

### 已发现 bug（本次一并修复）

| # | 问题 | 详情 |
|---|------|------|
| 1 | `tcflush` 重复定义 | `busybox_stubs.c:82` 和 `term_stubs.c:2` 各定义一次，签名不同 |
| 2 | `sigfillset.c` 死代码 | `signal.h:87` 有宏 `#define sigfillset(set) (*(set) = ~0UL)`；`.c` 文件从不被调用 |
| 3 | 多文件缺少 errno 设置 | getsockname / getpeername / killpg 裸 `return -1`，不设 errno |
| 4 | `struct passwd/group` 本地冗余定义 | busybox_stubs.c 内重复定义，而 `pwd.h`/`grp.h` 已有正确定义 |
| 5 | `htons` 等放错归属 | 属于 `<arpa/inet.h>` / POSIX `<netinet/in.h>`，不是 `<netdb.h>` |
| 6 | `dirname`/`basename` 放错归属 | 属于 `<libgen.h>`，不是 `<string.h>` |
| 7 | `netdb.h` 守卫外有重复垃圾 | `#endif` 后整段重复两次（lines 48-69） |
| 8 | `tcgetattr`/`tcsetattr` 返回 raw syscall | 返回负 errno 不符合 POSIX，应按 kill.c 惯例转换 |

## 设计原则

1. **按标准 header 归属分目录** — 函数声明在哪个 header，实现就放在对应目录
2. **unistd 域：一函数一文件** — 延续 unistd/ 现有惯例
3. **其他域：一组一个文件** — xattr 12 个函数合并（纯样板，分开无价值）；termios/socket/sched 同理
4. **统一 stub 语义** — 失败：`errno = ENOSYS; return -1`；成功 no-op：`return 0`。例外：`getpwnam`/`getpwuid`/`getgrnam`/`getgrgid` 裸 `return NULL`（POSIX "未找到"语义，非错误）
5. **每个 `.c` 文件 include 对应 header** — 让编译器检查声明/定义一致性
6. **Makefile 用 wildcard 自动发现** — 不手工枚举文件

## 设计方案

### 删除 6 个文件

```
libc/unistd/busybox_stubs.c
libc/unistd/uid_stubs.c
libc/unistd/misc_stubs.c
libc/unistd/term_stubs.c
libc/unistd/net_stubs.c
libc/unistd/sigfillset.c   ← 死代码，不迁移，直接删除
```

### 新建 12 个目录

```
libc/termios/    libc/socket/    libc/netdb/    libc/sched/
libc/xattr/      libc/resource/  libc/pwd/       libc/grp/
libc/csu/        libc/signal/    libc/arpa/      libc/libgen/
```

### 新建文件清单

每个条目列出：路径、include 列表、来源、函数列表及行为。

---

#### `termios/termios.c`

```c
#include <termios.h>       /* struct termios, B9600, 函数声明 */
#include <sys/ioctl.h>     /* TCGETS, TCSETS */
#include <sys/syscall.h>   /* syscall() static inline, SYS_ioctl */
#include <errno.h>         /* errno */
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `tcgetattr` | 实际 ioctl 包装，按 ret<0 → errno=-ret; return -1 惯例转换 | busybox_stubs.c |
| `tcsetattr` | 同上 | busybox_stubs.c |
| `tcflow` | `return 0` | busybox_stubs.c |
| `tcflush` | `return 0`（统一用 busybox 版本，消除与 term_stubs 的重复） | busybox_stubs.c |
| `cfgetispeed` | `return B9600` | busybox_stubs.c |
| `cfgetospeed` | `return B9600` | busybox_stubs.c |
| `cfsetispeed` | `return 0` | busybox_stubs.c |
| `cfsetospeed` | `return 0` | busybox_stubs.c |

---

#### `socket/socket.c`

```c
#include <sys/socket.h>
#include <errno.h>
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `socket` | `errno = ENOSYS; return -1` | busybox_stubs.c |
| `bind` | `errno = ENOSYS; return -1` | busybox_stubs.c |
| `listen` | `errno = ENOSYS; return -1` | busybox_stubs.c |
| `sendto` | `errno = ENOSYS; return -1` | busybox_stubs.c |
| `getsockname` | `errno = ENOSYS; return -1`（行为变化：旧版无 errno） | net_stubs.c |
| `getpeername` | `errno = ENOSYS; return -1`（行为变化：旧版无 errno） | net_stubs.c |

---

#### `netdb/netdb.c`

```c
#include <netdb.h>
#include <string.h>
#include <errno.h>
```

| 符号 | 行为 | 来源 |
|------|------|------|
| `int h_errno = 0` | 全局变量 | net_stubs.c |
| `hstrerror` | `return "Unknown host"` | net_stubs.c |
| `getaddrinfo` | `return -1` | net_stubs.c |
| `freeaddrinfo` | no-op | net_stubs.c |
| `getservbyname` | `return NULL` | net_stubs.c |
| `gethostbyname` | `return NULL` | net_stubs.c |
| `getnameinfo` | `return -1` | net_stubs.c |

---

#### `arpa/inet.c`

```c
#include <arpa/inet.h>
#include <stdint.h>
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `htons` | `return n`（identity） | net_stubs.c |
| `ntohs` | `return n`（identity） | net_stubs.c |
| `htonl` | `return n`（identity） | net_stubs.c |
| `ntohl` | `return n`（identity） | net_stubs.c |

**同步头文件**：补齐 `arpa/inet.h` 和 `netinet/in.h` 的声明，从 `netdb.h` 中移除不属于它的 `htons`/`ntohs`/`htonl`/`ntohl` 声明（它们属于 `<arpa/inet.h>` 和 POSIX `<netinet/in.h>`）。

---

#### `libgen/libgen.c`

```c
#include <libgen.h>
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `dirname` | `return "/"` | busybox_stubs.c |
| `basename` | `return ""` | busybox_stubs.c |

---

#### `sched/sched.c`

```c
#include <sched.h>         /* 含 <unistd.h> → pid_t, 函数声明 */
#include <errno.h>
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `sched_getaffinity` | `errno = ENOSYS; return -1` | busybox_stubs.c |
| `sched_setaffinity` | `errno = ENOSYS; return -1` | busybox_stubs.c |
| `sched_get_priority_max` | `return 0` | busybox_stubs.c |
| `sched_get_priority_min` | `return 0` | busybox_stubs.c |
| `sched_setscheduler` | `return 0` | busybox_stubs.c |
| `sched_getscheduler` | `return SCHED_NORMAL` | busybox_stubs.c |
| `sched_yield` | `return 0` | busybox_stubs.c |

---

#### `xattr/xattr.c`

```c
#include <sys/xattr.h>
#include <errno.h>
```

12 个函数，统一 `errno = ENOSYS; return -1`（`getxattr`/`lgetxattr`/`fgetxattr`/`listxattr`/`llistxattr`/`flistxattr`/`setxattr`/`lsetxattr`/`fsetxattr`/`removexattr`/`lremovexattr`/`fremovexattr`）。来源：busybox_stubs.c。

---

#### `resource/resource.c`

```c
#include <sys/resource.h>
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `getrlimit` | 实际逻辑：`rlim->rlim_cur = rlim->rlim_max = 65536; return 0` | busybox_stubs.c |
| `setrlimit` | `return 0` | busybox_stubs.c |

---

#### `pwd/pwd.c`

```c
#include <pwd.h>
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `getpwnam` | `return NULL`（POSIX "未找到"语义，不改 errno） | busybox_stubs.c |
| `getpwuid` | `return NULL`（同上） | busybox_stubs.c |

使用 `pwd.h` 中的 `struct passwd` 定义，**不**本地定义。

---

#### `grp/grp.c`

```c
#include <grp.h>
#include <sys/types.h>    /* gid_t */

int setgroups(size_t s, const gid_t *l) { (void)s; (void)l; return 0; }
int initgroups(const char *u, int g) { (void)u; (void)g; return 0; }
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `getgrnam` | `return NULL`（POSIX "未找到"语义，不改 errno） | busybox_stubs.c |
| `getgrgid` | `return NULL`（同上） | busybox_stubs.c |
| `setgroups` | `return 0` | uid_stubs.c |
| `initgroups` | `return 0` | uid_stubs.c |

使用 `grp.h` 中的 `struct group` 定义，**不**本地定义。

**同步头文件**：`setgroups` 在整个 include 树中无声明。在 `grp.h` 中补充：
```c
int setgroups(size_t size, const gid_t *list);
int initgroups(const char *user, gid_t group);
```
（当前 `initgroups` 声明在 `unistd.h:107`，`setgroups` 无声明。）

---

#### `csu/csu.c`

```c
#include <stdlib.h>    /* environ (extern) */
```

| 函数 | 行为 | 来源 |
|------|------|------|
| `__libc_start_main` | 实际 CRT 启动：调用 `main(argc, argv, environ)` | busybox_stubs.c |

非 stub。命名贴合 glibc 惯例（`csu/` = C StartUp）。

**注意**：`environ` 声明在 `stdlib.h:48`（`extern char **environ;`），不在 `unistd.h`。

---

#### signal/ 目录（从 unistd/ 迁移 6 个文件 + 新建 1 个）

从 `unistd/` 迁移的文件（内容不变，仅路径变化）：
```
unistd/kill.c          → signal/kill.c
unistd/raise.c         → signal/raise.c
unistd/sigaction.c     → signal/sigaction.c
unistd/signal.c        → signal/signal.c
unistd/sigprocmask.c   → signal/sigprocmask.c
unistd/sigsuspend.c    → signal/sigsuspend.c
```

**注意**：`raise.c` 当前同时 include `<signal.h>` 和 `<unistd.h>`。迁入 signal/ 后 include 不变——`unistd.h` 仍需要（提供 `syscall()` static inline）。

新建文件：

**`signal/killpg.c`**

```c
#include <signal.h>
#include <errno.h>
```

`killpg` — stub: `errno = ENOSYS; return -1`（行为变化：旧版无 errno）

**不可迁移**：`sigfillset.c` 不迁移。`signal.h:87` 的宏 `#define sigfillset(set) (*(set) = ~0UL)` 使 `.c` 函数不可达。且函数签名 `int sigfillset(void *s)` 与 POSIX 不一致。直接删除。

signal/ 最终共 **7** 个文件：6 个迁入 + 1 个新建（killpg.c）。

---

### unistd/ 中新建的独立文件

从 uid_stubs.c/misc_stubs.c/term_stubs.c 拆分，每函数一个文件，延续 unistd/ 惯例。

每个文件 include `<unistd.h>`（提供类型和声明）+ 按需 `<errno.h>`。

#### uid/gid（7 个新建；`geteuid.c` 已存在无需操作）

| 文件 | 行为 | 来源 |
|------|------|------|
| `getuid.c` | `return 0` | uid_stubs.c |
| `getgid.c` | `return 0` | uid_stubs.c |
| `getegid.c` | `return 0` | uid_stubs.c |
| `setgid.c` | `return 0` | uid_stubs.c |
| `setuid.c` | `return 0` | uid_stubs.c |
| `setegid.c` | `return 0` | uid_stubs.c |
| `seteuid.c` | `return 0` | uid_stubs.c |

#### session/pgrp（7 个）

| 文件 | 行为 | 来源 |
|------|------|------|
| `vfork.c` | `errno = ENOSYS; return -1` | uid_stubs.c |
| `setsid.c` | `errno = ENOSYS; return -1` | uid_stubs.c |
| `getsid.c` | `return 0` | uid_stubs.c |
| `getpgrp.c` | `return 1` | uid_stubs.c |
| `setpgid.c` | `return 0` | uid_stubs.c |
| `tcsetpgrp.c` | `return 0` | uid_stubs.c |
| `tcgetpgrp.c` | `return 1` | uid_stubs.c |

#### chown 系列

| 文件 | 行为 | 来源 |
|------|------|------|
| `chown.c` | 启用（已存在）。含 `chown` + `fchown`，均为 `errno = ENOSYS; return -1` | 取消 filter-out |
| `lchown.c` | 新建：`errno = ENOSYS; return -1` | 旧 uid_stubs 为 `return 0`，统一为 ENOSYS |

**不新建 `fchown.c`** — `chown.c` 同时定义了 `chown` 和 `fchown`，避免重复定义。

#### 其他（4 个）

| 文件 | 行为 | 来源 |
|------|------|------|
| `fchdir.c` | `errno = ENOSYS; return -1` | misc_stubs.c |
| `chroot.c` | `errno = ENOSYS; return -1` | misc_stubs.c |
| `ttyname_r.c` | `errno = ENOSYS; return -1` | misc_stubs.c |
| `alarm.c` | `return 0` | term_stubs.c |

---

### 修改已有文件

#### `stdio/stdio_extras.c` — 追加 7 个函数

来源：busybox_stubs.c。追加 `#include <unistd.h>`（提供 `syscall()` 供 `fgets_unlocked` 使用）。

| 函数 | 行为 |
|------|------|
| `ferror_unlocked` | `return 0` |
| `clearerr` | no-op |
| `fileno_unlocked` | `return 0` |
| `fopen` | `return NULL` |
| `fclose` | `return 0` |
| `fdopen` | `return NULL` |
| `fgets_unlocked` | 实际实现（从 fd 0 逐字节读） |

#### `stdlib/environ.c` — 追加 2 个函数

来源：busybox_stubs.c。追加 `#include <string.h>`（`strchr`, `strncmp`）。

| 函数 | 行为 |
|------|------|
| `putenv` | 有实际逻辑（env_table 管理，含 `realloc`） |
| `unsetenv` | `return 0` |

#### `string/extras.c` — 不追加

`dirname`/`basename` 归属 `<libgen.h>`，放入新建的 `libgen/libgen.c`。

#### `time/time.c` — 追加 2 个函数

追加 `#include <sys/time.h>`（`settimeofday` 声明）、`#include <errno.h>`（`ENOSYS`）。

| 函数 | 行为 | 来源 |
|------|------|------|
| `strftime` | `return 0` | busybox_stubs.c |
| `settimeofday` | `errno = ENOSYS; return -1` | misc_stubs.c |

注意：当前 `settimeofday` 声明在 `unistd.h:145` 而非 `<sys/time.h>`。声明不迁移，仅实现迁移。

#### `arpa/inet.h` — 补齐声明

```c
uint16_t htons(uint16_t n);
uint16_t ntohs(uint16_t n);
uint32_t htonl(uint32_t n);
uint32_t ntohl(uint32_t n);
```

#### `netinet/in.h` — 补齐声明

同样补齐 4 个字节序函数声明（POSIX 标准位置，busybox 网络代码常用）。

#### `netdb.h` — 清理

- 删除守卫 `#endif` 后的整段重复内容（lines 48-69）
- 删除原本的 `htons`/`ntohs`/`htonl`/`ntohl` 声明（它们属于 `<arpa/inet.h>` 和 `<netinet/in.h>`）

#### `grp.h` — 补齐声明

```c
int setgroups(size_t size, const gid_t *list);
int initgroups(const char *user, gid_t group);
```

（`setgroups` 当前无任何声明；`initgroups` 当前在 `unistd.h:107`，后续可清理。）

---

### Makefile 变更

新增 12 行 wildcard：
```makefile
    $(wildcard termios/*.c) \
    $(wildcard socket/*.c) \
    $(wildcard netdb/*.c) \
    $(wildcard sched/*.c) \
    $(wildcard xattr/*.c) \
    $(wildcard resource/*.c) \
    $(wildcard pwd/*.c) \
    $(wildcard grp/*.c) \
    $(wildcard csu/*.c) \
    $(wildcard signal/*.c) \
    $(wildcard arpa/*.c) \
    $(wildcard libgen/*.c) \
```

删除 filter-out 中 `unistd/chown.c`：
```makefile
# 旧
C_SOURCES := $(filter-out stdlib/free.c unistd/chown.c, $(C_SOURCES))
# 新
C_SOURCES := $(filter-out stdlib/free.c, $(C_SOURCES))
```

同时删除死 wildcard `$(wildcard dirent/*.c)`（`libc/dirent/` 目录不存在）。

---

## 行为变化清单

| 函数 | 旧行为 | 新行为 | 影响评估 |
|------|--------|--------|----------|
| `chown` | `return 0`（恒成功） | `errno=ENOSYS; return -1` | busybox `chown` / `cp -p` 从成功变为报错 |
| `fchown` | `return 0`（恒成功） | `errno=ENOSYS; return -1` | 同上 |
| `lchown` | `return 0`（恒成功） | `errno=ENOSYS; return -1` | 统一语义 |
| `tcflush` | `return -1`（term_stubs 版本，实际被 busybox 版本覆盖） | `return 0`（统一为已验证版本） | 消除重复 |
| `tcgetattr` | 返回 raw syscall（负 errno） | `errno=-ret; return -1` 惯例转换 | 修复 POSIX 合规性 |
| `tcsetattr` | 返回 raw syscall（负 errno） | `errno=-ret; return -1` 惯例转换 | 同上 |
| `getsockname` | `return -1`（无 errno） | `errno=ENOSYS; return -1` | 统一 stub 语义 |
| `getpeername` | `return -1`（无 errno） | `errno=ENOSYS; return -1` | 统一 stub 语义 |
| `killpg` | `return -1`（无 errno） | `errno=ENOSYS; return -1` | 统一 stub 语义 |
| `sigfillset` | 死代码 `.c` 文件 | 删除，宏生效 | 无影响 |

**不在清单中的函数**：`getpwnam`/`getpwuid`/`getgrnam`/`getgrgid` 保持 `return NULL`（不改 errno），语义不变。

---

## 验证方案

1. **重复符号检查**：`nm libc.a | sort | uniq -d` — 确认零重复
2. **归档构建**：`make -C libc clean && make -C libc` — 确认 `.a` 生成无警告
3. **完整链接**：`make clean && make` 走 root Makefile 的 `busybox.elf` 流程 — 确认无 undefined symbol

---

## 排除在外的内容

- `stdlib/free.c` 与 `malloc.c` 的冲突排除（保留现有 filter-out）
- `settimeofday` 声明从 `unistd.h` 迁移到 `<sys/time.h>`（声明迁移，不属本次范围）
- `initgroups` 声明从 `unistd.h` 迁移到 `<grp.h>`（不属本次范围；本次仅补齐 `grp.h` 中缺失的声明）
- 非 stub 函数的实现改进（如 `fgets_unlocked` 的实际逻辑）

---

## 重构后文件结构

```
libc/
  arpa/inet.c           (~12行)
  termios/termios.c     (~30行)
  socket/socket.c       (~20行)
  netdb/netdb.c         (~45行)
  sched/sched.c         (~25行)
  xattr/xattr.c         (~45行)
  resource/resource.c   (~15行)
  pwd/pwd.c             (~8行)
  grp/grp.c             (~15行)
  csu/csu.c             (~10行)
  libgen/libgen.c       (~10行)
  signal/               (7 文件: killpg + 6 个自 unistd/ 迁入)
  unistd/               (原有 ~60 文件, -5 *_stubs.c, -1 sigfillset.c,
                         -6 迁出信号文件, +19 新建独立 stub,
                         chown.c 启用)
  stdio/stdio_extras.c  (+7 函数)
  stdlib/environ.c      (+2 函数: putenv, unsetenv)
  time/time.c           (+2 函数: strftime, settimeofday)

头文件修改:
  arpa/inet.h           (+4 字节序声明)
  netinet/in.h          (+4 字节序声明)
  netdb.h               (-4 字节序声明移出, -守卫外重复块)
  grp.h                 (+2 声明: setgroups, initgroups)
```

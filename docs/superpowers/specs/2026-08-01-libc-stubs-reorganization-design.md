# libc Stub 文件拆分重构设计

**日期**: 2026-08-01
**状态**: 待实现

## 目标

消除 `libc/unistd/` 下 5 个 `*_stubs.c` 文件，将其中约 100 个函数按 GNU libc 风格（按标准 header 归属分目录，每组 stub 一个 `.c` 文件）重新组织。

## 当前状态

`libc/unistd/` 下存在 5 个混合 stub 文件：

| 文件 | 行数 | 内容 |
|------|------|------|
| `busybox_stubs.c` | 232 | stdio、rlimit、termios、socket、sched、xattr、putenv、`__libc_start_main` 等 |
| `uid_stubs.c` | 29 | uid/gid、vfork、setsid、进程组、chown、killpg |
| `net_stubs.c` | 54 | netdb 函数、字节序转换、socket 地址函数 |
| `misc_stubs.c` | 8 | fchdir、chroot、ttyname_r、settimeofday |
| `term_stubs.c` | 4 | tcflush（与 busybox_stubs.c 重复）、alarm |

### 已发现 bug

- `tcflush` 在 `busybox_stubs.c:82` 和 `term_stubs.c:2` 重复定义，返回值和签名不同（`return 0` vs `return -1; int` vs `void`）

## 设计方案

### 原则

1. 按标准 header 归属分目录（GNU libc 风格）
2. 每个功能域一组 stub 一个 `.c` 文件
3. 每个 syscall wrapper 或独立概念的 stub 单独成一个文件
4. Makefile 使用 wildcard 自动发现，无需手工枚举

### 删除 5 个文件

```
libc/unistd/busybox_stubs.c
libc/unistd/uid_stubs.c
libc/unistd/misc_stubs.c
libc/unistd/term_stubs.c
libc/unistd/net_stubs.c
```

### 新建 11 个目录

```
libc/termios/    libc/socket/    libc/netdb/    libc/sched/
libc/xattr/      libc/resource/  libc/pwd/       libc/grp/
libc/csu/        libc/signal/
```

### 新建文件

#### termios/ (10 个函数)

**`termios/termios.c`** — 来源：busybox_stubs.c
- `tcgetattr` — 实际 ioctl 包装（非纯 stub，调用 `syscall(SYS_ioctl, ..., TCGETS, ...)`）
- `tcsetattr` — 实际 ioctl 包装（同上，过 TCGETS）
- `tcflow` — stub: `return 0`
- `tcflush` — stub: `return 0`（统一用 busybox 版本，消除重复）
- `cfgetispeed` — stub: `return B9600`
- `cfgetospeed` — stub: `return B9600`
- `cfsetispeed` — stub: `return 0`
- `cfsetospeed` — stub: `return 0`

#### socket/ (6 个函数)

**`socket/socket.c`** — 来源：busybox_stubs.c
- `socket`, `bind`, `listen`, `sendto` — stub: `errno = ENOSYS; return -1`
- `getsockname`, `getpeername` — 来源：net_stubs.c，stub: `return -1`

#### netdb/ (12 个符号)

**`netdb/netdb.c`** — 来源：net_stubs.c
- `int h_errno = 0` — 全局变量
- `hstrerror` — stub: `return "Unknown host"`
- `getaddrinfo`, `freeaddrinfo`, `getservbyname`, `gethostbyname`, `getnameinfo` — stub
- `htons`, `ntohs`, `htonl`, `ntohl` — 直通（identity）

注意：net_stubs.c 中的 `getsockname`, `getpeername` 移至 `socket/socket.c`（属于 `<sys/socket.h>`）。

#### sched/ (7 个函数)

**`sched/sched.c`** — 来源：busybox_stubs.c
- `sched_getaffinity`, `sched_setaffinity`, `sched_get_priority_max`, `sched_get_priority_min`, `sched_setscheduler`, `sched_getscheduler` — stub
- `sched_yield` — stub: `return 0`

#### xattr/ (12 个函数)

**`xattr/xattr.c`** — 来源：busybox_stubs.c
- `getxattr`, `lgetxattr`, `fgetxattr`, `listxattr`, `llistxattr`, `flistxattr`, `setxattr`, `lsetxattr`, `fsetxattr`, `removexattr`, `lremovexattr`, `fremovexattr` — 全部 `errno = ENOSYS; return -1`

#### resource/ (2 个函数)

**`resource/resource.c`** — 来源：busybox_stubs.c
- `getrlimit` — 实际逻辑：`rlim->rlim_cur = rlim->rlim_max = 65536; return 0`
- `setrlimit` — stub: `return 0`

#### pwd/ (2 个函数)

**`pwd/pwd.c`** — 来源：busybox_stubs.c
- `getpwnam` — stub: `return NULL`
- `getpwuid` — stub: `return NULL`

注意：需本地定义 `struct passwd`（当前在 busybox_stubs.c 中本地定义）。

#### grp/ (4 个函数)

**`grp/grp.c`** — 来源：busybox_stubs.c + uid_stubs.c
- `getgrnam`, `getgrgid` — stub: `return NULL`
- `setgroups`, `initgroups` — stub: `return 0`

注意：需本地定义 `struct group`（当前在 busybox_stubs.c 中本地定义）。

#### csu/ (1 个函数)

**`csu/csu.c`** — 来源：busybox_stubs.c
- `__libc_start_main` — 非 stub，实际 CRT 启动逻辑：调用 `main(argc, argv, environ)`

#### signal/ (从 unistd/ 迁移 6 个 + 新建 1 个)

从 `unistd/` 迁移至 `signal/`：
```
unistd/raise.c        → signal/raise.c
unistd/sigaction.c    → signal/sigaction.c
unistd/sigfillset.c   → signal/sigfillset.c
unistd/signal.c       → signal/signal.c
unistd/sigprocmask.c  → signal/sigprocmask.c
unistd/sigsuspend.c   → signal/sigsuspend.c
```

新建：
**`signal/killpg.c`** — 来源：uid_stubs.c，stub: `return -1`

### unistd/ 中新建的独立文件（共 16 个）

从 uid_stubs.c/misc_stubs.c/term_stubs.c 拆分出，每个函数一个文件：

**uid/gid 系列（8 个）**：
```
unistd/getuid.c       — return 0
unistd/getgid.c       — return 0
unistd/getegid.c      — return 0
unistd/setgid.c       — return 0
unistd/setuid.c       — return 0
unistd/setegid.c      — return 0
unistd/seteuid.c      — return 0
unistd/geteuid.c      — return 0（已有，无需操作）
```

**session/pgrp 系列（7 个）**：
```
unistd/vfork.c        — errno = ENOSYS; return -1
unistd/setsid.c       — errno = ENOSYS; return -1
unistd/getsid.c       — return 0
unistd/getpgrp.c      — return 1
unistd/setpgid.c      — return 0
unistd/tcsetpgrp.c    — return 0
unistd/tcgetpgrp.c    — return 1
```

**chown 系列（2 个新建，1 个启用）**：
```
unistd/fchown.c       — 新建：return 0
unistd/lchown.c       — 新建：return 0
unistd/chown.c         — 已存在（当前被 Makefile filter-out），启用
```

**其他（4 个）** — 来源：misc_stubs.c + term_stubs.c：
```
unistd/fchdir.c       — errno = ENOSYS; return -1
unistd/chroot.c       — errno = ENOSYS; return -1
unistd/ttyname_r.c    — errno = ENOSYS; return -1
unistd/alarm.c        — return 0
```

### 修改已有文件

**`stdio/stdio_extras.c`** — 追加 7 个 stdio stub（来源：busybox_stubs.c）
- `ferror_unlocked`, `clearerr`, `fileno_unlocked`, `fopen`, `fclose`, `fdopen`, `fgets_unlocked`

**`stdlib/environ.c`** — 追加 2 个环境变量函数（来源：busybox_stubs.c）
- `putenv` — 有实际逻辑（env_table 管理）
- `unsetenv` — stub: `return 0`

**`string/extras.c`** — 追加 2 个路径函数（来源：busybox_stubs.c）
- `dirname` — stub: `return "/"`
- `basename` — stub: `return ""`

**`time/time.c`** — 追加 2 个时间函数
- `strftime` — 来源：busybox_stubs.c，stub: `return 0`
- `settimeofday` — 来源：misc_stubs.c，stub: `errno = ENOSYS; return -1`

### Makefile 变更

新增 11 行 wildcard：
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
```

删除 filter-out 行：
```makefile
# 删除
C_SOURCES := $(filter-out stdlib/free.c unistd/chown.c, $(C_SOURCES))
# 改为
C_SOURCES := $(filter-out stdlib/free.c, $(C_SOURCES))
```

原因：`chown.c` 冲突的来源是 `uid_stubs.c` 中定义了 `chown`——该文件被删除后冲突自然消除。`free.c` 与 `malloc.c` 的冲突保留处理。

### 修复重复定义

`tcflush` 同时存在于 `busybox_stubs.c:82` 和 `term_stubs.c:2`：
- `busybox_stubs.c`: `int tcflush(int fd, int q) { (void)fd; (void)q; return 0; }`
- `term_stubs.c`: `int tcflush(int fd, int q) { (void)fd; (void)q; return -1; }`

采用 busybox 版本（`return 0`），放入 `termios/termios.c`。删除 `term_stubs.c` 后冲突消除。

## 排除在外的内容

- `libc/unistd/chown.c` 当前被 Makefile filter-out 排除，`uid_stubs.c` 删除后取消排除，启用该文件
- `libc/stdlib/free.c` 继续被 filter-out（与 `malloc.c` 冲突）

## 文件结构对比

### 重构前
```
libc/
  unistd/
    busybox_stubs.c   (232行, ~46 函数, 7+ 域)
    uid_stubs.c       (29行, ~16 函数)
    net_stubs.c       (54行, ~14 符号)
    misc_stubs.c      (8行, 4 函数)
    term_stubs.c      (4行, 2 函数, 含重复)
  ... (其他 60+ unistd 文件)
```

### 重构后
```
libc/
  termios/termios.c   (~25行)
  socket/socket.c     (~20行)
  netdb/netdb.c       (~50行)
  sched/sched.c       (~25行)
  xattr/xattr.c       (~45行)
  resource/resource.c (~15行)
  pwd/pwd.c           (~10行)
  grp/grp.c           (~15行)
  csu/csu.c           (~10行)
  signal/             (7 文件: killpg + 从 unistd 迁入的 6 个)
  unistd/             (60+ 原有文件 + 16 新建独立 stub 文件,
                       -5 *_stubs.c, -6 signal 文件,
                       chown.c 启用)
  stdio/stdio_extras.c (+7 函数)
  stdlib/environ.c    (+2 函数)
  string/extras.c     (+2 函数)
  time/time.c         (+2 函数)
```

## 风险

- 低风险：纯文件移动和拆分，函数实现不改变
- `__libc_start_main` 中的 `struct passwd` / `struct group` 本地定义需随函数移动到新文件
- Makefile wildcard 自动发现新目录，无需手动维护文件列表
- 构建验证：`make -C libc` 成功后链接 busybox 确认无未定义符号

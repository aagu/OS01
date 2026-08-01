# libc Stub 文件拆分重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate 5 `*_stubs.c` files in `libc/unistd/`, redistributing ~100 functions into new directories organized by standard header, fixing bugs and unifying stub semantics.

**Architecture:** New directories follow GNU libc convention — one directory per standard-header domain. Each domain gets a single `.c` file. `unistd/` functions remain one-per-file. 6 signal files migrate from `unistd/` to `signal/`. Makefile uses wildcard auto-discovery.

**Spec:** `docs/superpowers/specs/2026-08-01-libc-stubs-reorganization-design.md`

## Global Constraints

- Every `.c` file must `#include` its corresponding header
- Stub failure: `errno = ENOSYS; return -1` (int) or `return NULL` (pointer). Exception: `getpwnam`/`getpwuid`/`getgrnam`/`getgrgid` return `NULL` without errno
- `tcgetattr`/`tcsetattr` must convert raw syscall: `if (ret < 0) { errno = (int)(-ret); return -1; }` pattern
- Build verified at each major phase with `make -C libc clean && make -C libc`

---

### Task 1: Fix headers (4 files)

**Files:**
- Modify: `libc/include/netinet/in.h`
- Modify: `libc/include/netdb.h`
- Modify: `libc/include/grp.h`
- Modify: `libc/include/unistd.h`

**Interfaces:**
- Produces: `htons`, `ntohs`, `htonl`, `ntohl` declared in `netinet/in.h`; removed from `netdb.h`; `setgroups`, `initgroups(gid_t)` in `grp.h`; `initgroups` in `unistd.h` corrected to `gid_t`

- [ ] **Step 1: Add byte-order declarations to `netinet/in.h`**

Add after the `IPPROTO_RAW` line (before the `sockaddr_in6` struct):

```c
/* Byte-order functions (POSIX) */
uint16_t htons(uint16_t n);
uint16_t ntohs(uint16_t n);
uint32_t htonl(uint32_t n);
uint32_t ntohl(uint32_t n);
```

- [ ] **Step 2: Clean `netdb.h` — remove duplicate garbage and htons declarations**

Delete lines 40-43 (the `htons`/`ntohs`/`htonl`/`ntohl` declarations — they belong in `netinet/in.h`). Delete lines 48-68 (the entire duplicated block outside the `#endif` guard). Keep lines 1-47, then end file. The result should end with:

```c
#ifdef __cplusplus
}
#endif

#endif /* _NETDB_H */
```

- [ ] **Step 3: Add `setgroups`/`initgroups` to `grp.h`**

After the `getgrgid` declaration line, add:

```c
int setgroups(size_t size, const gid_t *list);
int initgroups(const char *user, gid_t group);
```

- [ ] **Step 4: Fix `initgroups` signature in `unistd.h`**

On line 107, change:
```c
int initgroups(const char *user, int group);
```
to:
```c
int initgroups(const char *user, gid_t group);
```

- [ ] **Step 5: Verify headers compile cleanly**

Run:
```bash
cd libc && clang -target x86_64-unknown-none -ffreestanding -Iinclude -fsyntax-only include/netinet/in.h include/netdb.h include/grp.h include/unistd.h
```
Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add libc/include/netinet/in.h libc/include/netdb.h libc/include/grp.h libc/include/unistd.h
git commit -m "fix(libc): add byte-order/grp declarations, clean netdb.h, fix initgroups type

- netinet/in.h: add htons/ntohs/htonl/ntohl declarations
- netdb.h: remove htons declarations (belong in netinet/in.h),
  delete duplicated block outside include guard (lines 48-68)
- grp.h: add setgroups() and initgroups() declarations
- unistd.h: fix initgroups parameter type int -> gid_t

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Create new domain directories and .c files (11 files)

**Files:**
- Create: `libc/termios/termios.c`
- Create: `libc/socket/socket.c`
- Create: `libc/netdb/netdb.c`
- Create: `libc/sched/sched.c`
- Create: `libc/xattr/xattr.c`
- Create: `libc/resource/resource.c`
- Create: `libc/pwd/pwd.c`
- Create: `libc/grp/grp.c`
- Create: `libc/csu/csu.c`
- Create: `libc/arpa/inet.c`
- Create: `libc/libgen/libgen.c`

**Interfaces:**
- Produces: 11 `.o` files compiled into `libc.a`, providing implementations for functions previously in `*_stubs.c`
- Note: builds will fail at this stage (duplicate symbols with old stubs). This is expected until Task 5.

- [ ] **Step 1: Create all 12 directories**

```bash
mkdir -p libc/{termios,socket,netdb,sched,xattr,resource,pwd,grp,csu,arpa,libgen,signal}
```

- [ ] **Step 2: Write `libc/termios/termios.c`**

```c
#include <termios.h>       /* struct termios, B9600, function declarations */
#include <sys/ioctl.h>     /* TCGETS, TCSETS */
#include <sys/syscall.h>   /* syscall(), SYS_ioctl */
#include <errno.h>         /* errno */

int tcgetattr(int fd, struct termios *tio)
{
    int64_t ret = syscall(SYS_ioctl, (uint64_t)fd, (uint64_t)TCGETS, (uint64_t)tio);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int tcsetattr(int fd, int actions, const struct termios *tio)
{
    (void)actions;
    int64_t ret = syscall(SYS_ioctl, (uint64_t)fd, (uint64_t)TCSETS, (uint64_t)tio);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int tcflow(int fd, int action)       { (void)fd; (void)action; return 0; }
int tcflush(int fd, int q)           { (void)fd; (void)q; return 0; }

speed_t cfgetispeed(const struct termios *tio) { (void)tio; return B9600; }
speed_t cfgetospeed(const struct termios *tio) { (void)tio; return B9600; }

int cfsetispeed(struct termios *tio, speed_t s) { (void)tio; (void)s; return 0; }
int cfsetospeed(struct termios *tio, speed_t s) { (void)tio; (void)s; return 0; }
```

- [ ] **Step 3: Write `libc/socket/socket.c`**

```c
#include <sys/socket.h>
#include <errno.h>

int socket(int domain, int type, int protocol)
    { (void)domain; (void)type; (void)protocol; errno = ENOSYS; return -1; }

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
    { (void)sockfd; (void)addr; (void)addrlen; errno = ENOSYS; return -1; }

int listen(int sockfd, int backlog)
    { (void)sockfd; (void)backlog; errno = ENOSYS; return -1; }

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest, socklen_t addrlen)
    { (void)sockfd; (void)buf; (void)len; (void)flags;
      (void)dest; (void)addrlen; errno = ENOSYS; return -1; }

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    { (void)sockfd; (void)addr; (void)addrlen; errno = ENOSYS; return -1; }

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    { (void)sockfd; (void)addr; (void)addrlen; errno = ENOSYS; return -1; }
```

- [ ] **Step 4: Write `libc/netdb/netdb.c`**

```c
#include <netdb.h>
#include <string.h>

int h_errno = 0;

const char *hstrerror(int err) { (void)err; return "Unknown host"; }

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
    { (void)node; (void)service; (void)hints; (void)res; return -1; }

void freeaddrinfo(struct addrinfo *res) { (void)res; }

struct servent *getservbyname(const char *name, const char *proto)
    { (void)name; (void)proto; return NULL; }

struct hostent *gethostbyname(const char *name)
    { (void)name; return NULL; }

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
    { (void)sa; (void)salen; (void)host; (void)hostlen;
      (void)serv; (void)servlen; (void)flags; return -1; }
```

- [ ] **Step 5: Write `libc/sched/sched.c`**

```c
#include <sched.h>         /* includes <unistd.h> -> pid_t, function declarations */
#include <errno.h>

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
    { (void)pid; (void)cpusetsize; (void)mask; errno = ENOSYS; return -1; }

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask)
    { (void)pid; (void)cpusetsize; (void)mask; errno = ENOSYS; return -1; }

int sched_get_priority_max(int policy) { (void)policy; return 0; }
int sched_get_priority_min(int policy) { (void)policy; return 0; }

int sched_setscheduler(pid_t pid, int policy, const void *param)
    { (void)pid; (void)policy; (void)param; return 0; }

int sched_getscheduler(pid_t pid) { (void)pid; return SCHED_NORMAL; }
int sched_yield(void) { return 0; }
```

- [ ] **Step 6: Write `libc/xattr/xattr.c`**

```c
#include <sys/xattr.h>
#include <errno.h>

ssize_t getxattr(const char *p, const char *n, void *v, size_t s)
    { (void)p; (void)n; (void)v; (void)s; errno = ENOSYS; return -1; }
ssize_t lgetxattr(const char *p, const char *n, void *v, size_t s)
    { (void)p; (void)n; (void)v; (void)s; errno = ENOSYS; return -1; }
ssize_t fgetxattr(int fd, const char *n, void *v, size_t s)
    { (void)fd; (void)n; (void)v; (void)s; errno = ENOSYS; return -1; }
ssize_t listxattr(const char *p, char *l, size_t s)
    { (void)p; (void)l; (void)s; errno = ENOSYS; return -1; }
ssize_t llistxattr(const char *p, char *l, size_t s)
    { (void)p; (void)l; (void)s; errno = ENOSYS; return -1; }
ssize_t flistxattr(int fd, char *l, size_t s)
    { (void)fd; (void)l; (void)s; errno = ENOSYS; return -1; }
int setxattr(const char *p, const char *n, const void *v, size_t s, int f)
    { (void)p; (void)n; (void)v; (void)s; (void)f; errno = ENOSYS; return -1; }
int lsetxattr(const char *p, const char *n, const void *v, size_t s, int f)
    { (void)p; (void)n; (void)v; (void)s; (void)f; errno = ENOSYS; return -1; }
int fsetxattr(int fd, const char *n, const void *v, size_t s, int f)
    { (void)fd; (void)n; (void)v; (void)s; (void)f; errno = ENOSYS; return -1; }
int removexattr(const char *p, const char *n)
    { (void)p; (void)n; errno = ENOSYS; return -1; }
int lremovexattr(const char *p, const char *n)
    { (void)p; (void)n; errno = ENOSYS; return -1; }
int fremovexattr(int fd, const char *n)
    { (void)fd; (void)n; errno = ENOSYS; return -1; }
```

- [ ] **Step 7: Write `libc/resource/resource.c`**

```c
#include <sys/resource.h>

int getrlimit(int resource, struct rlimit *rlim)
{
    (void)resource;
    if (rlim) { rlim->rlim_cur = 65536; rlim->rlim_max = 65536; }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
    { (void)resource; (void)rlim; return 0; }
```

- [ ] **Step 8: Write `libc/pwd/pwd.c`**

```c
#include <pwd.h>

struct passwd *getpwnam(const char *name) { (void)name; return NULL; }
struct passwd *getpwuid(uid_t uid)       { (void)uid; return NULL; }
```

- [ ] **Step 9: Write `libc/grp/grp.c`**

```c
#include <grp.h>
#include <sys/types.h>    /* gid_t */

struct group *getgrnam(const char *name) { (void)name; return NULL; }
struct group *getgrgid(gid_t gid)        { (void)gid; return NULL; }

int setgroups(size_t s, const gid_t *l)  { (void)s; (void)l; return 0; }
int initgroups(const char *u, gid_t g)  { (void)u; (void)g; return 0; }
```

- [ ] **Step 10: Write `libc/csu/csu.c`**

```c
#include <stdlib.h>    /* environ (extern) */

extern char **environ;

int __libc_start_main(int (*main)(int, char **, char **),
                       int argc, char **argv,
                       void (*init)(void), void (*fini)(void),
                       void (*rtld_fini)(void), void *stack_end)
{
    (void)init; (void)fini; (void)rtld_fini; (void)stack_end;
    return main(argc, argv, environ);
}
```

- [ ] **Step 11: Write `libc/arpa/inet.c`**

```c
#include <arpa/inet.h>

uint16_t htons(uint16_t n)  { return n; }
uint16_t ntohs(uint16_t n)  { return n; }
uint32_t htonl(uint32_t n)  { return n; }
uint32_t ntohl(uint32_t n)  { return n; }
```

- [ ] **Step 12: Write `libc/libgen/libgen.c`**

```c
#include <libgen.h>

char *dirname(char *path)  { (void)path; return "/"; }
char *basename(char *path) { (void)path; return ""; }
```

- [ ] **Step 13: Commit**

```bash
git add libc/termios/ libc/socket/ libc/netdb/ libc/sched/ libc/xattr/ \
        libc/resource/ libc/pwd/ libc/grp/ libc/csu/ libc/arpa/ libc/libgen/
git commit -m "feat(libc): add new domain directories with stub implementations

Create 11 new directories following GNU libc convention:
  termios/ socket/ netdb/ sched/ xattr/ resource/
  pwd/ grp/ csu/ arpa/ libgen/

Each directory has a single .c file implementing stubs previously
scattered across unistd/*_stubs.c.

Notable fixes:
- tcgetattr/tcsetattr: convert raw syscall return to POSIX errno convention
- pwd/grp: use standard headers (pwd.h/grp.h) instead of local struct defs
- getsockname/getpeername: add errno=ENOSYS (was bare return -1)
- All xattr/socket stubs unified to errno=ENOSYS; return -1

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Create new unistd/ single-function stub files (19 files)

**Files:**
- Create: `libc/unistd/getuid.c`, `libc/unistd/getgid.c`, `libc/unistd/getegid.c`
- Create: `libc/unistd/setgid.c`, `libc/unistd/setuid.c`, `libc/unistd/setegid.c`, `libc/unistd/seteuid.c`
- Create: `libc/unistd/vfork.c`, `libc/unistd/setsid.c`, `libc/unistd/getsid.c`
- Create: `libc/unistd/getpgrp.c`, `libc/unistd/setpgid.c`, `libc/unistd/tcsetpgrp.c`, `libc/unistd/tcgetpgrp.c`
- Create: `libc/unistd/lchown.c`
- Create: `libc/unistd/fchdir.c`, `libc/unistd/chroot.c`, `libc/unistd/ttyname_r.c`, `libc/unistd/alarm.c`

**Interfaces:**
- Produces: 19 `.o` files with implementations previously in `uid_stubs.c`, `misc_stubs.c`, `term_stubs.c`
- Note: `chown.c` already exists (contains `chown` + `fchown`) but is filtered out by Makefile — will be enabled in Task 5

- [ ] **Step 1: Write uid/gid stubs (7 files)**

`libc/unistd/getuid.c`:
```c
#include <unistd.h>
int getuid(void) { return 0; }
```

`libc/unistd/getgid.c`:
```c
#include <unistd.h>
int getgid(void) { return 0; }
```

`libc/unistd/getegid.c`:
```c
#include <unistd.h>
int getegid(void) { return 0; }
```

`libc/unistd/setgid.c`:
```c
#include <unistd.h>
int setgid(gid_t g) { (void)g; return 0; }
```

`libc/unistd/setuid.c`:
```c
#include <unistd.h>
int setuid(uid_t u) { (void)u; return 0; }
```

`libc/unistd/setegid.c`:
```c
#include <unistd.h>
int setegid(gid_t g) { (void)g; return 0; }
```

`libc/unistd/seteuid.c`:
```c
#include <unistd.h>
int seteuid(uid_t u) { (void)u; return 0; }
```

Note: `geteuid.c` already exists, skip.

- [ ] **Step 2: Write session/pgrp stubs (7 files)**

`libc/unistd/vfork.c`:
```c
#include <unistd.h>
#include <errno.h>
int vfork(void) { errno = ENOSYS; return -1; }
```

`libc/unistd/setsid.c`:
```c
#include <unistd.h>
#include <errno.h>
int setsid(void) { errno = ENOSYS; return -1; }
```

`libc/unistd/getsid.c`:
```c
#include <unistd.h>
pid_t getsid(pid_t pid) { (void)pid; return 0; }
```

`libc/unistd/getpgrp.c`:
```c
#include <unistd.h>
pid_t getpgrp(void) { return 1; }
```

`libc/unistd/setpgid.c`:
```c
#include <unistd.h>
int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
```

`libc/unistd/tcsetpgrp.c`:
```c
#include <unistd.h>
int tcsetpgrp(int fd, pid_t pgrp) { (void)fd; (void)pgrp; return 0; }
```

`libc/unistd/tcgetpgrp.c`:
```c
#include <unistd.h>
pid_t tcgetpgrp(int fd) { (void)fd; return 1; }
```

- [ ] **Step 3: Write lchown stub**

`libc/unistd/lchown.c`:
```c
#include <unistd.h>
#include <errno.h>
int lchown(const char *path, uid_t owner, gid_t group)
    { (void)path; (void)owner; (void)group; errno = ENOSYS; return -1; }
```

Note: `chown.c` already exists (contains `chown` + `fchown`, both `ENOSYS; return -1`). It will be enabled in Task 5. Do NOT create `fchown.c`.

- [ ] **Step 4: Write misc stubs (4 files)**

`libc/unistd/fchdir.c`:
```c
#include <unistd.h>
#include <errno.h>
int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }
```

`libc/unistd/chroot.c`:
```c
#include <unistd.h>
#include <errno.h>
int chroot(const char *p) { (void)p; errno = ENOSYS; return -1; }
```

`libc/unistd/ttyname_r.c`:
```c
#include <unistd.h>
#include <errno.h>
int ttyname_r(int fd, char *buf, size_t buflen)
    { (void)fd; (void)buf; (void)buflen; errno = ENOSYS; return -1; }
```

`libc/unistd/alarm.c`:
```c
#include <unistd.h>
unsigned int alarm(unsigned int s) { (void)s; return 0; }
```

- [ ] **Step 5: Commit**

```bash
git add libc/unistd/
git commit -m "feat(libc): add 19 single-function stub files in unistd/

Split uid_stubs.c, misc_stubs.c, and term_stubs.c into individual files
following the existing unistd/ convention of one function per file.

New files: getuid.c, getgid.c, getegid.c, setgid.c, setuid.c,
setegid.c, seteuid.c, vfork.c, setsid.c, getsid.c, getpgrp.c,
setpgid.c, tcsetpgrp.c, tcgetpgrp.c, lchown.c, fchdir.c,
chroot.c, ttyname_r.c, alarm.c

Note: fchown.c NOT created — chown.c (currently filtered out)
already defines both chown and fchown.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Migrate signal files + create killpg

**Files:**
- Move: `libc/unistd/kill.c` → `libc/signal/kill.c`
- Move: `libc/unistd/raise.c` → `libc/signal/raise.c`
- Move: `libc/unistd/sigaction.c` → `libc/signal/sigaction.c`
- Move: `libc/unistd/signal.c` → `libc/signal/signal.c`
- Move: `libc/unistd/sigprocmask.c` → `libc/signal/sigprocmask.c`
- Move: `libc/unistd/sigsuspend.c` → `libc/signal/sigsuspend.c`
- Create: `libc/signal/killpg.c`
- Delete: `libc/unistd/sigfillset.c` (dead code, directly removed)

**Interfaces:**
- Produces: 7 `.o` files in `signal/`; 6 files no longer in `unistd/`

- [ ] **Step 1: Git-mv the 6 signal files**

```bash
git mv libc/unistd/kill.c        libc/signal/kill.c
git mv libc/unistd/raise.c       libc/signal/raise.c
git mv libc/unistd/sigaction.c   libc/signal/sigaction.c
git mv libc/unistd/signal.c      libc/signal/signal.c
git mv libc/unistd/sigprocmask.c libc/signal/sigprocmask.c
git mv libc/unistd/sigsuspend.c  libc/signal/sigsuspend.c
```

- [ ] **Step 2: Create `libc/signal/killpg.c`**

```c
#include <signal.h>
#include <errno.h>

int killpg(pid_t pgrp, int sig)
    { (void)pgrp; (void)sig; errno = ENOSYS; return -1; }
```

- [ ] **Step 3: Delete dead `sigfillset.c`**

```bash
git rm libc/unistd/sigfillset.c
```

Reason: `signal.h:87` has `#define sigfillset(set) (*(set) = ~0UL)` which makes the `.c` function unreachable. The function signature `int sigfillset(void *s)` is non-POSIX anyway.

- [ ] **Step 4: Commit**

```bash
git commit -m "refactor(libc): migrate signal files unistd/ -> signal/ + add killpg

Move 6 signal implementation files from unistd/ to new signal/ directory:
  kill.c raise.c sigaction.c signal.c sigprocmask.c sigsuspend.c

Add killpg.c (was in uid_stubs.c, now standalone with errno=ENOSYS).

Remove sigfillset.c — dead code, overridden by macro in signal.h:87.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Append functions to existing source files

**Files:**
- Modify: `libc/stdio/stdio_extras.c` — append 7 stdio stubs
- Modify: `libc/stdlib/environ.c` — append `putenv`, `unsetenv`
- Modify: `libc/time/time.c` — append `strftime`, `settimeofday`

**Interfaces:**
- Consumes: none (self-contained additions)
- Produces: new symbols in existing `.o` files

- [ ] **Step 1: Append 7 stdio stubs to `libc/stdio/stdio_extras.c`**

Read the current file. Add `#include <unistd.h>` to the includes (needed for `syscall()` in `fgets_unlocked`). Then append at end of file:

```c
/* ── stdio stubs (from busybox_stubs.c) ── */

int ferror_unlocked(void *f) { (void)f; return 0; }
int clearerr(void *f)        { (void)f; return 0; }
int fileno_unlocked(FILE *f) { (void)f; return 0; }

void *fopen(const char *path, const char *mode)
    { (void)path; (void)mode; return NULL; }

int fclose(void *f) { (void)f; return 0; }

void *fdopen(int fd, const char *mode)
    { (void)fd; (void)mode; return NULL; }

char *fgets_unlocked(char *s, int n, void *f)
{
    if (!s || n <= 1) return NULL;
    (void)f;
    int i = 0;
    while (i < n - 1) {
        int64_t ret = syscall(SYS_read, 0, (uint64_t)&s[i], 1);
        if (ret <= 0) break;
        i++;
        if (s[i - 1] == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}
```

- [ ] **Step 2: Append `putenv` + `unsetenv` to `libc/stdlib/environ.c`**

No new includes needed — `<string.h>` and `<stdlib.h>` are already present (lines 1-3). Append at end of file:

```c
/* ── putenv / unsetenv ── */

int unsetenv(const char *name) { (void)name; return 0; }

int putenv(char *string)
{
    static char **env_table = NULL;
    static int env_capacity = 0;
    static int env_count = 0;
    char *eq;

    if (!string || !(eq = strchr(string, '=')))
        return -1;

    size_t key_len = (size_t)(eq - string);

    /* Find existing key → replace */
    for (int i = 0; i < env_count; i++) {
        if (strncmp(env_table[i], string, key_len) == 0
            && env_table[i][key_len] == '=') {
            env_table[i] = string;
            return 0;
        }
    }

    /* New entry: need env_count + 2 slots (new entry + NULL terminator) */
    if (env_count + 2 > env_capacity) {
        int new_cap = env_capacity ? env_capacity * 2 : 16;
        char **new_table = realloc(env_table, new_cap * sizeof(char *));
        if (!new_table)
            return -1;
        env_table = new_table;
        env_capacity = new_cap;
    }

    env_table[env_count++] = string;
    env_table[env_count] = NULL;
    environ = env_table;

    return 0;
}
```

Note: `environ` is already declared `extern` in this file (line 8: `char **environ = NULL;`), so `putenv`'s `environ = env_table` assignment is valid.

- [ ] **Step 3: Append `strftime` + `settimeofday` to `libc/time/time.c`**

Add `#include <sys/time.h>` and `#include <errno.h>` to the includes. Then append:

```c
/* ── strftime ── */

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    (void)s; (void)max; (void)fmt; (void)tm;
    return 0;
}

/* ── settimeofday ── */

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv; (void)tz;
    errno = ENOSYS;
    return -1;
}
```

- [ ] **Step 4: Commit**

```bash
git add libc/stdio/stdio_extras.c libc/stdlib/environ.c libc/time/time.c
git commit -m "feat(libc): append stdio/env/time stubs from busybox_stubs.c

- stdio/stdio_extras.c: add ferror_unlocked, clearerr, fileno_unlocked,
  fopen, fclose, fdopen, fgets_unlocked
- stdlib/environ.c: add putenv (with env_table logic), unsetenv
- time/time.c: add strftime, settimeofday (with errno=ENOSYS)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Delete old stubs + Update Makefile

**Files:**
- Delete: `libc/unistd/busybox_stubs.c`, `libc/unistd/uid_stubs.c`, `libc/unistd/misc_stubs.c`, `libc/unistd/term_stubs.c`, `libc/unistd/net_stubs.c`
- Modify: `libc/Makefile`

**Interfaces:**
- Consumes: All new files from Tasks 2-5 must exist
- Produces: Clean build with zero duplicate symbols

- [ ] **Step 1: Delete the 5 old stub files**

```bash
git rm libc/unistd/busybox_stubs.c
git rm libc/unistd/uid_stubs.c
git rm libc/unistd/misc_stubs.c
git rm libc/unistd/term_stubs.c
git rm libc/unistd/net_stubs.c
```

Note: `sigfillset.c` was already deleted in Task 4.

- [ ] **Step 2: Update `libc/Makefile`**

Two changes:

**(a)** Add 12 wildcard lines for new directories after the existing wildcard block. Add before the `$(wildcard pthread/*.c)` line (or at the end of the `C_SOURCES :=` block):

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

**(b)** Change the filter-out line:
```makefile
# Old (line 33):
C_SOURCES := $(filter-out stdlib/free.c unistd/chown.c, $(C_SOURCES))
# New:
C_SOURCES := $(filter-out stdlib/free.c, $(C_SOURCES))
```

**(c)** Remove the dead `$(wildcard dirent/*.c)` line (line 21) — the `libc/dirent/` directory does not exist.

- [ ] **Step 3: Commit**

```bash
git add libc/Makefile
git commit -m "refactor(libc): remove *_stubs.c, update Makefile for new structure

Delete 5 mixed stub files:
  busybox_stubs.c uid_stubs.c misc_stubs.c term_stubs.c net_stubs.c

Makefile changes:
- Add 12 wildcard lines for new directories (termios, socket, netdb,
  sched, xattr, resource, pwd, grp, csu, signal, arpa, libgen)
- Remove unistd/chown.c from filter-out (uid_stubs.c deleted, no conflict)
- Remove dead wildcard dirent/*.c (directory does not exist)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Build verification

**Files:** None (verification only)

**Interfaces:**
- Consumes: All changes from Tasks 1-6

- [ ] **Step 1: Clean build of libc**

```bash
make -C libc clean && make -C libc
```

Expected: `libc.a` and `libk.a` created with no warnings.

- [ ] **Step 2: Check for duplicate symbols**

```bash
nm libc/libc.a | grep -oP 'T \K.+' | sort | uniq -d
```

Expected: **empty output** (zero duplicate defined symbols). If any duplicates appear, halt and investigate — this was the primary bug this refactoring fixes (tcflush).

- [ ] **Step 3: Full busybox link**

```bash
make clean && make
```

Expected: `busybox.elf` links successfully with no undefined symbol errors.

- [ ] **Step 4: Sanity-check symbol counts**

```bash
echo "=== New symbol count per domain ==="
for d in termios socket netdb sched xattr resource pwd grp csu arpa libgen signal; do
    count=$(nm libc/libc.a | grep -c "libc/${d}/")
    echo "  ${d}/: ${count} symbols"
done
echo "=== unistd/ symbol count ==="
nm libc/libc.a | grep -c "libc/unistd/"
```

Expected: Each new domain directory has expected symbol count. No unexpected zeros.

---

### Task 8: Final commit + cleanup

- [ ] **Step 1: Verify git status is clean**

```bash
git status
```

Expected: working tree clean, all changes committed.

- [ ] **Step 2: Final verification summary**

Run a quick summary command:
```bash
echo "=== Files deleted ===" && git diff --stat HEAD~6..HEAD --diff-filter=D
echo "=== Files created ===" && git diff --stat HEAD~6..HEAD --diff-filter=A
echo "=== Directories created ===" && git diff HEAD~6..HEAD --name-only | grep '/' | cut -d/ -f1-2 | sort -u | grep -E '^(termios|socket|netdb|sched|xattr|resource|pwd|grp|csu|arpa|libgen|signal)/'
```

Expected: Shows 6 deleted files, ~31 created files, 12 new directories.

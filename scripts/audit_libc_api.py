#!/usr/bin/env python3
"""OS01 libc public-API manifest and structural audit.

This script is the single source of truth for the libc completion work's
public API inventory.  The inventory is grouped by the task that delivers
that group; only the groups delivered so far are enabled.  T1 delivers the
``api`` group: the public header contract (a header is self-contained, has
exactly one definition across translation units, and gives its declarations C
linkage so a C++ consumer links against the same symbols).

T2 delivers the ``alloc`` group: the allocation interfaces (overflow-safe
``calloc``/``malloc``/``reallocarray`` and the aligned allocation calls).
That group owns its interfaces through an "owned" map instead of a header
inventory, because the headers it touches (malloc.h, stdlib.h) also carry
interfaces other groups own.

Scope rules (design spec "分配与环境"; plan "文件与构建责任"):

* the user libc is audited against the FULL user-facing interface set;
* libk is audited against the ALREADY-SUPPORTED subset only, plus the
  explicitly shared ``reallocarray``;
* user-only aligned allocation, the regex group and the float-scanning
  group are NOT libk requirements.

Usage::

    # print the enabled inventory
    scripts/audit_libc_api.py --list

    # emit the C manifest consumed by the host test (build-time)
    scripts/audit_libc_api.py --emit-c-header <path>

    # structural audit against the shipped headers and archives
    scripts/audit_libc_api.py --audit --include-dir libc/include \\
        --libc-archive build/<profile>/libc/lib/libc.a \\
        --libk-archive build/<profile>/libc/lib/libk.a
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ── Groups ──────────────────────────────────────────────────────────────
# Every group names the task that delivers it.  ENABLED_GROUPS lists the
# groups delivered so far; a later task appends its group here.
ENABLED_GROUPS: tuple[str, ...] = ("api", "alloc", "env", "shutdown", "stream")

# Headers that are deliberately NOT part of the api group, with the reason.
# An excluded header is still compiled by nothing in the api group, so it
# cannot make the api group fail; the reason is printed by --list and by
# --audit so the exclusion is never silent.
EXCLUDED_HEADERS: dict[str, str] = {
    # Deferred by the T1 brief: regex.h keeps its current types and inline
    # implementation until T16 migrates it.  The api group therefore does
    # not check its self-containment / two-TU / C++ properties yet.
    "regex.h": "deferred to T16 (current regex.h is a stub with inline bodies)",
    # Out of scope for this round (plan "范围覆盖与依赖").
    "glob.h": "out of scope this round (glob/globfree excluded by the plan)",
    "fnmatch.h": "out of scope this round (fnmatch rework excluded by the plan)",
    # Internal container helpers, not user-facing libc interfaces.
    "list.h": "internal container helper (kernel/libk), not a user libc interface",
    "queue.h": "internal container helper (kernel/libk), not a user libc interface",
    "rbtree.h": "internal container helper (kernel/libk), not a user libc interface",
    # Internal byte-order / crypto / feature-test helpers.
    "byteswap.h": "internal byte-order helper, not a user libc interface",
    "endian.h": "internal byte-order helper, not a user libc interface",
    "chacha20.h": "internal crypto helper, not a user libc interface",
    "features.h": "internal feature-test macro header",
    # Private stdio headers (T5).  They live under include/<module>/ because
    # they belong to the libc implementation, not to the public interface:
    # the public <stdio.h> keeps FILE opaque, and these two headers carry
    # the stream state and the bounded formatter.
    "stdio/file.h": "private stdio header (stream state), not a public libc interface",
    "stdio/internal.h": "private stdio header (bounded formatter), not a public libc interface",
}

# Symbols a header declares only under a host-test-only condition.  They
# are not part of the public interface, so they are not audited and never
# appear in the emitted manifest.
EXCLUDED_SYMBOLS: dict[str, str] = {
    "host_test_syscall": "declared only under #ifdef OS01_HOST_TEST (host-test hook)",
}

# Symbols the user libc must provide but libk must NOT be *required* to
# provide.  "Not required" is not "must not exist": libk currently happens
# to export some of them.  The audit only forbids *requiring* them, and it
# separately forbids libk from gaining the user-only aligned-allocation
# symbols (design spec: "libk archive 不应定义这两个用户态对齐符号").
LIBK_NOT_REQUIRED: frozenset[str] = frozenset({
    # user-only aligned allocation (T2)
    "posix_memalign", "aligned_alloc",
    # regex group (T16)
    "regcomp", "regexec", "regerror", "regfree",
    # float scanning (T11/T12)
    "strtof", "strtold", "sscanf", "vsscanf", "fscanf", "scanf", "vscanf",
})

# Explicitly shared between the user libc and libk.  ``reallocarray`` is
# delivered by the alloc group (T2) and is safe to share because it does not
# touch the allocator's private block layout.
LIBK_SHARED: frozenset[str] = frozenset({"reallocarray"})

# The api-group symbols that each profile's libk.a already provides — the
# "already-supported subset" the plan freezes for the libk audit.  libk is
# NOT required to provide the rest of the user-facing API: the api-group
# symbols that libk does not have are simply out of the libk scope, and
# the explicitly shared symbols (LIBK_SHARED) are the only additions a
# later task may make.  The x86_64 and aarch64 libk builds export
# different subsets, so the baseline is per profile.
LIBK_SUPPORTED: dict[str, frozenset[str]] = {
    "x86_64-clang": frozenset(
    """
_exit abort accept access alarm asctime asctime_r atexit atof atoi
atol basename bcmp bind bsearch calloc cfgetispeed cfgetospeed
cfsetispeed cfsetospeed chdir chmod chown chroot clearenv clearerr
clock clock_gettime close closedir connect ctime ctime_r dirname
dprintf dup dup2 execv execve execvp exit fchdir fchmod fchown fclose
fcntl fdopen ferror_unlocked fflush fgets_unlocked fgetxattr
fileno_unlocked flistxattr fopen fprintf fputc fputs fread free
freeaddrinfo fremovexattr fsetxattr fstat fstatat ftruncate fwrite
gai_strerror getaddrinfo getauxval getc_unlocked getchar
getchar_unlocked getcwd getdents64 getegid getenv geteuid getgid
getgrgid getgrnam getgroups gethostbyname getnameinfo getopt
getpeername getpgid getpgrp getpid getppid getpwnam getpwuid getrandom
getrlimit getservbyname getsid getsockname getsockopt gettimeofday
getuid getxattr gmtime gmtime_r hstrerror htonl htons inet_aton
inet_ntoa initgroups ioctl isalnum isalpha isascii isatty isblank
iscntrl isdigit isgraph islower isprint ispunct isspace isupper
isxdigit kill killpg lchown lgetxattr link listen listxattr llistxattr
localtime localtime_r longjmp lremovexattr lseek lsetxattr lstat
malloc memchr memcmp memcpy memmove mempcpy memset mkdir mknod mkstemp
mktime mmap mprotect munmap nanosleep ntohl ntohs open opendir perror
pipe poll prctl printf pselect pthread_mutex_destroy
pthread_mutex_init pthread_mutex_lock pthread_mutex_trylock
pthread_mutex_unlock putc_unlocked putchar putchar_unlocked putenv
puts qsort raise rand read readdir readlink realloc realpath recv
recvfrom removexattr rename rmdir sched_get_priority_max
sched_get_priority_min sched_getaffinity sched_getscheduler
sched_setaffinity sched_setscheduler sched_yield select send sendto
setegid setenv seteuid setgid setgroups setjmp setpgid setrlimit
setsid setsockopt settimeofday setuid setxattr shutdown sigaction
signal sigprocmask sigsuspend skip_atoi sleep snprintf socket
socketpair sprintf srand sscanf stat stpcpy stpncpy strcasecmp strcat
strchr strchrnul strcmp strcpy strcspn strdup strerror strftime strlen
strncasecmp strncat strncmp strncpy strndup strnlen strpbrk strptime
strrchr strsep strsignal strspn strstr strtod strtok strtok_r strtol
strtoll strtoul strtoull strverscmp symlink sysconf tcflow tcflush
tcgetattr tcgetpgrp tcsetattr tcsetpgrp time times tolower toupper
truncate ttyname_r umask uname unlink unsetenv utimes vasprintf vfork
vfprintf vprintf vsnprintf vsprintf waitpid write
    """.split()
),
    "aarch64-clang": frozenset(
    """
bcmp bsearch calloc free isalnum isalpha isascii isblank iscntrl
isdigit isgraph islower isprint ispunct isspace isupper isxdigit
malloc memchr memcmp memcpy memmove mempcpy memset perror realloc
realpath skip_atoi stpcpy stpncpy strcasecmp strcat strchr strchrnul
strcmp strcpy strcspn strdup strerror strlen strncasecmp strncat
strncmp strncpy strndup strnlen strpbrk strrchr strsep strsignal
strspn strstr strtok strtok_r strverscmp tolower toupper vsprintf
    """.split()
)
}

# ── The api group inventory ────────────────────────────────────────────
# header -> the public functions the header declares.  These are the
# user-facing interfaces the completion plan covers.  The script verifies
# this inventory against the shipped headers, so a manifest that drifts away
# from the headers is an audit failure, not a silent staleness.
API_GROUP_HEADERS: dict[str, list[str]] = {
    "arpa/inet.h": [
        "inet_aton",
        "inet_ntoa",
        "inet_addr",
        "inet_pton",
        "inet_ntop",
    ],
    "assert.h": [
        "__assert_fail",
    ],
    "ctype.h": [
        "isalnum",
        "isalpha",
        "isblank",
        "iscntrl",
        "isdigit",
        "isgraph",
        "islower",
        "isprint",
        "ispunct",
        "isspace",
        "isupper",
        "isxdigit",
        "isascii",
        "tolower",
        "toupper",
    ],
    "dirent.h": [
        "opendir",
        "readdir",
        "closedir",
    ],
    "errno.h": [],
    "fcntl.h": [],
    "float.h": [],
    "getopt.h": [
        "getopt",
    ],
    "grp.h": [
        "getgrnam",
        "getgrgid",
        "setgroups",
        "initgroups",
    ],
    "inttypes.h": [],
    "libgen.h": [
        "dirname",
        "basename",
    ],
    "limits.h": [],
    "malloc.h": [
        "malloc",
        "free",
        "calloc",
        "realloc",
        "posix_memalign",
        "aligned_alloc",
    ],
    "math.h": [],
    "mntent.h": [
        "setmntent",
        "getmntent",
        "endmntent",
    ],
    "net/ethernet.h": [],
    "net/if.h": [],
    "net/if_arp.h": [],
    "netdb.h": [
        "getnameinfo",
        "hstrerror",
        "getaddrinfo",
        "freeaddrinfo",
        "getservbyname",
        "gethostbyname",
        "htons",
        "ntohs",
        "htonl",
        "ntohl",
        "gai_strerror",
    ],
    "netinet/in.h": [
        "htons",
        "ntohs",
        "htonl",
        "ntohl",
    ],
    "paths.h": [],
    "poll.h": [
        "poll",
    ],
    "pthread.h": [
        "pthread_mutex_init",
        "pthread_mutex_lock",
        "pthread_mutex_unlock",
        "pthread_mutex_trylock",
        "pthread_mutex_destroy",
    ],
    "pwd.h": [
        "getpwnam",
        "getpwuid",
    ],
    "sched.h": [
        "sched_getaffinity",
        "sched_setaffinity",
        "sched_yield",
        "sched_get_priority_max",
        "sched_get_priority_min",
        "sched_setscheduler",
        "sched_getscheduler",
    ],
    "setjmp.h": [
        "setjmp",
        "longjmp",
    ],
    "signal.h": [
        "sigprocmask",
        "sigaction",
        "kill",
        "signal",
        "sigreturn_trampoline",
        "raise",
        "sigsuspend",
        "sigpending",
    ],
    "stdarg.h": [],
    "stdbool.h": [],
    "stddef.h": [],
    "stdint.h": [],
    "stdio.h": [
        "printf",
        "putchar",
        "puts",
        "vsprintf",
        "sprintf",
        "snprintf",
        "sscanf",
        "skip_atoi",
        "dprintf",
        "feof",
        "ferror",
        "clearerr",
        "feof_unlocked",
        "ferror_unlocked",
        "clearerr_unlocked",
        "ungetc",
        "perror",
        "fopen",
        "fdopen",
        "freopen",
        "fclose",
        "fseeko",
        "fread",
        "fflush",
        "vasprintf",
        "fileno_unlocked",
        "getc_unlocked",
        "putc_unlocked",
        "fputs",
        "getchar",
        "getchar_unlocked",
        "fgets_unlocked",
        "getline",
        "vprintf",
        "vfprintf",
        "fprintf",
        "vsnprintf",
        "putchar_unlocked",
        "fputc",
        "fwrite",
    ],
    "stdlib.h": [
        "realloc",
        "rand",
        "realpath",
        "srand",
        "abort",
        "malloc",
        "calloc",
        "free",
        "reallocarray",
        "posix_memalign",
        "aligned_alloc",
        "strtol",
        "strtoul",
        "atoi",
        "atol",
        "atoll",
        "strtoll",
        "strtoull",
        "qsort",
        "bsearch",
        "atexit",
        "exit",
        "getenv",
        "setenv",
        "unsetenv",
        "putenv",
        "clearenv",
        "strtod",
        "atof",
    ],
    "string.h": [
        "memcmp",
        "bcmp",
        "memcpy",
        "memmove",
        "memset",
        "strlen",
        "strnlen",
        "strcmp",
        "strncmp",
        "strverscmp",
        "strcpy",
        "strdup",
        "strchr",
        "strrchr",
        "strstr",
        "strtok",
        "strtok_r",
        "strsep",
        "strcat",
        "strncat",
        "strncpy",
        "memchr",
        "strspn",
        "strcspn",
        "strpbrk",
        "strerror",
        "perror",
        "strchrnul",
        "mempcpy",
        "stpncpy",
        "strsignal",
        "stpcpy",
        "strcasecmp",
        "strndup",
        "strncasecmp",
    ],
    "sys/auxv.h": [
        "getauxval",
    ],
    "sys/cdefs.h": [],
    "sys/futex.h": [],
    "sys/ioctl.h": [
        "ioctl",
    ],
    "sys/mman.h": [
        "mmap",
        "munmap",
        "mprotect",
    ],
    "sys/param.h": [],
    "sys/prctl.h": [
        "prctl",
    ],
    "sys/random.h": [
        "getrandom",
    ],
    "sys/resource.h": [
        "getrlimit",
        "setrlimit",
    ],
    "sys/select.h": [
        "select",
        "pselect",
    ],
    "sys/socket.h": [
        "socket",
        "bind",
        "listen",
        "accept",
        "connect",
        "recv",
        "recvfrom",
        "setsockopt",
        "getsockopt",
        "send",
        "sendto",
        "getsockname",
        "getpeername",
        "socketpair",
        "shutdown",
    ],
    "sys/ssp.h": [
        "__stack_chk_fail",
    ],
    "sys/stat.h": [
        "stat",
        "lstat",
        "fstat",
        "fstatat",
        "lseek",
        "fcntl",
        "ioctl",
        "getdents64",
        "access",
    ],
    "sys/statfs.h": [
        "statfs",
        "fstatfs",
    ],
    "sys/syscall.h": [],
    "sys/sysmacros.h": [],
    "sys/time.h": [
        "gettimeofday",
        "nanosleep",
        "utimes",
        "clock_gettime",
    ],
    "sys/times.h": [
        "times",
    ],
    "sys/types.h": [],
    "sys/un.h": [],
    "sys/utsname.h": [
        "uname",
    ],
    "sys/wait.h": [],
    "sys/xattr.h": [
        "getxattr",
        "lgetxattr",
        "fgetxattr",
        "listxattr",
        "llistxattr",
        "flistxattr",
        "setxattr",
        "lsetxattr",
        "fsetxattr",
        "removexattr",
        "lremovexattr",
        "fremovexattr",
    ],
    "termios.h": [
        "tcgetattr",
        "tcsetattr",
        "tcgetpgrp",
        "tcflow",
        "tcflush",
        "tcsetpgrp",
        "cfgetispeed",
        "cfgetospeed",
        "cfsetispeed",
        "cfsetospeed",
    ],
    "time.h": [
        "time",
        "gmtime",
        "localtime",
        "mktime",
        "clock",
        "strftime",
        "strptime",
        "localtime_r",
        "gmtime_r",
        "ctime_r",
        "asctime_r",
        "ctime",
        "asctime",
    ],
    "unistd.h": [
        "vfork",
        "setsid",
        "getsid",
        "open",
        "close",
        "read",
        "write",
        "dup",
        "dup2",
        "fork",
        "waitpid",
        "chdir",
        "getcwd",
        "pipe",
        "execve",
        "execv",
        "execvp",
        "execl",
        "execlp",
        "execle",
        "unlink",
        "link",
        "symlink",
        "readlink",
        "mkdir",
        "rmdir",
        "rename",
        "truncate",
        "ftruncate",
        "getppid",
        "getpid",
        "_exit",
        "time",
        "nanosleep",
        "chmod",
        "fchmod",
        "umask",
        "chown",
        "fchown",
        "lchown",
        "mknod",
        "alarm",
        "sleep",
        "isatty",
        "getuid",
        "getgid",
        "geteuid",
        "getgroups",
        "initgroups",
        "endgrent",
        "tcflush",
        "getopt",
        "sysconf",
        "getegid",
        "setgid",
        "setuid",
        "setegid",
        "seteuid",
        "fchdir",
        "chroot",
        "mkstemp",
        "ttyname_r",
        "settimeofday",
        "getpgrp",
        "setpgid",
        "getpgid",
        "tcsetpgrp",
        "tcgetpgrp",
        "killpg",
    ],
}

API_GROUPS: dict[str, dict] = {
    "api": {
        "task": "T1",
        "title": "public header contract: self-contained, single definition, C linkage",
        "headers": API_GROUP_HEADERS,
        # The api group is a header-level contract: it guarantees that every
        # listed interface is *declared* correctly, not that the behaviour
        # behind it is implemented.  Export enforcement belongs to the
        # functional groups (alloc/env/shutdown/...) that own the behaviour.
        "require_user_export": False,
    },
    "alloc": {
        "task": "T2",
        "title": "overflow-safe allocation and aligned allocation",
        # The alloc group owns the allocation interfaces, but it does not
        # own the headers they are declared in: malloc.h and stdlib.h are
        # audited for declaration drift by the api group, which lists every
        # public interface of both headers.
        "headers": {},
        # "owned" names the interfaces the group delivers (header -> the
        # symbols it owns).  It replaces the header inventory for the
        # export and libk checks: a header can declare interfaces other
        # groups own (stdlib.h declares the whole C library), while the
        # owned map names exactly what this group is responsible for.
        "owned": {
            "malloc.h": [
                "malloc",
                "free",
                "calloc",
                "realloc",
                "posix_memalign",
                "aligned_alloc",
            ],
            "stdlib.h": [
                "reallocarray",
            ],
        },
        # Unlike the api group, the alloc group owns behaviour: the user
        # libc must really export every interface it owns.
        "require_user_export": True,
    },
    "env": {
        "task": "T3",
        "title": "one environment table with per-entry ownership",
        # The env group owns the environment interfaces, but it does not
        # own the header they are declared in: stdlib.h declares the whole
        # C library and is audited for declaration drift by the api group.
        "headers": {},
        "owned": {
            "stdlib.h": [
                "getenv",
                "setenv",
                "unsetenv",
                "putenv",
                "clearenv",
            ],
        },
        # The env group owns behaviour: the user libc must really export
        # every interface it owns.
        "require_user_export": True,
    },
    "stream": {
        "task": "T5",
        "title": "one stream state for the standard streams and the opened files",
        # The stream group owns the status indicators of a stream, but it
        # does not own the header they are declared in: stdio.h declares
        # the whole stream API and is audited for declaration drift by
        # the api group.
        "headers": {},
        "owned": {
            "stdio.h": [
                "feof",
                "ferror",
                "clearerr",
                "feof_unlocked",
                "ferror_unlocked",
                "clearerr_unlocked",
                "ungetc",
            ],
        },
        # The stream group owns behaviour: the user libc must really
        # export every interface it owns.
        "require_user_export": True,
    },
    "shutdown": {
        "task": "T4",
        "title": "shutdown connected to the existing kernel syscall",
        # The shutdown group owns the shutdown interface, but it does not
        # own the header it is declared in: sys/socket.h declares the
        # whole socket API and is audited for declaration drift by the
        # api group.
        "headers": {},
        "owned": {
            "sys/socket.h": [
                "shutdown",
            ],
        },
        # The shutdown group owns behaviour: the user libc must really
        # export the interface it owns.
        "require_user_export": True,
    },
}


# ── Mechanical header inspection ───────────────────────────────────────

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_ATTRIBUTE = re.compile(r"__attribute__\s*\(\(.*?\)\)", re.S)
# The declared name is the last identifier before the parameter list, so
# every C type spelling (including "struct tm *") is accepted.
_FUNC_DECL = re.compile(
    r"^(?P<head>[A-Za-z_][^;()=]*?)\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\("
)
_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "defined", "do",
    "else", "case", "goto", "break", "continue", "default",
}


def _strip_comments(text: str) -> str:
    text = _BLOCK_COMMENT.sub(" ", text)
    return _LINE_COMMENT.sub(" ", text)


def _logical_statements(text: str) -> list[tuple[str, int]]:
    """Split a header into logical statements.

    A statement ends at a `;` or at a closing brace, but only at brace
    depth zero: a struct definition or a `static inline` body is one
    statement, not a sequence of declarations.

    Each statement is returned together with the preprocessor BRANCH PATH
    it appeared at: a tuple holding, for every open conditional, the index
    of the branch that is active.  Two declarations are in mutually
    exclusive branches exactly when their paths differ at some frame that
    BOTH of them have — that is the `#if`/`#else` case, where repeating a
    name is legal C.

    A path that is merely SHORTER is not a different branch: a declaration
    outside an include guard has a shorter path than one inside it, and
    that difference is exactly the defect this detects (a declaration left
    outside the guard's `#endif`).
    """
    out: list[tuple[str, tuple[int, ...]]] = []
    buf = ""
    depth = 0
    frames: list[int] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            words = line.split()
            directive = words[0] if words else ""
            if directive in ("#if", "#ifdef", "#ifndef"):
                frames.append(0)
            elif directive in ("#else", "#elif"):
                if frames:
                    frames[-1] += 1
            elif directive == "#endif":
                if frames:
                    frames.pop()
            # Preprocessor directives are not declarations.  A directive
            # may continue on the next line with a trailing backslash.
            if not line.endswith("\\"):
                buf = ""
                depth = 0
            continue
        buf += " " + line
        depth += line.count("{") - line.count("}")
        tail = buf.rstrip()
        if depth <= 0 and (tail.endswith(";") or tail.endswith("}")):
            out.append((tail, tuple(frames)))
            buf = ""
            depth = 0
    return out


def declared_declarations(text: str) -> list[tuple[str, str, tuple[int, ...]]]:
    """Return (name, raw text, branch path) for each declaration.

    The raw text is the statement with its whitespace collapsed but its
    ATTRIBUTES kept, so that an attribute-only variant of a declaration
    stays distinguishable from an exact repeat.  The branch path is the
    preprocessor branch path the declaration appeared at.
    """
    out: list[tuple[str, str, tuple[int, ...]]] = []
    for stmt, path in _logical_statements(_strip_comments(text)):
        s = _ATTRIBUTE.sub(" ", stmt).strip()
        if not s or s.startswith("typedef"):
            continue
        # A type definition (struct/union/enum body) is not a declaration
        # of a function; a "struct X *f(...)" prototype is.
        if "{" in s or s.startswith("}"):
            continue
        if s.split("(")[0].split()[:1] and s.split()[0] in _KEYWORDS:
            continue
        if "=" in s.split("(")[0]:
            continue
        m = _FUNC_DECL.match(s)
        if not m:
            continue
        head = m.group("head")
        name = m.group("name")
        if "static" in head.split():
            continue
        if name in _KEYWORDS:
            continue
        out.append((name, " ".join(stmt.split()), path))
    return out


def declared_functions(text: str, unique: bool = True) -> list[str]:
    """Return the externally linked functions a header declares.

    `static` helpers (including `static inline` bodies) are implementation
    detail, not the public interface, so they are not reported.

    With unique=False a name that appears more than once is reported once per
    occurrence.  The default (unique=True) hides duplicates, which is why a
    header that declares the same function twice used to pass the drift
    check unnoticed.
    """
    names: list[str] = []
    seen: set[str] = set()
    for name, _raw, _path in declared_declarations(text):
        if unique and name in seen:
            continue
        seen.add(name)
        names.append(name)
    return names


def duplicate_declarations(text: str) -> tuple[list[str], list[str]]:
    """Return (hard duplicates, benign repeats) in a header's declarations.

    A HARD duplicate is the very same declaration repeated unconditionally:
    the header says the exact same thing twice, so the second copy is dead
    text.  The historical cause was a declaration left outside the include
    guard's `#endif`, which made the tail re-process on every inclusion.

    A BENIGN repeat is the same name declared again with a different
    signature, or inside a conditional branch.  Both are legal C and must
    not be reported as a defect.
    """
    by_name: dict[str, list[tuple[str, tuple[int, ...]]]] = {}
    for name, raw, path in declared_declarations(text):
        by_name.setdefault(name, []).append((raw, path))
    hard: list[str] = []
    benign: list[str] = []
    for name, entries in by_name.items():
        if len(entries) < 2:
            continue
        for i, (raw_a, path_a) in enumerate(entries):
            for raw_b, path_b in entries[i + 1:]:
                if raw_a != raw_b:
                    continue
                # The same declaration twice.  It is only a defect when
                # both copies can be compiled at the same time — that is,
                # when their branch paths are not mutually exclusive.
                if not _mutually_exclusive(path_a, path_b):
                    hard.append(name)
                    break
            else:
                continue
            break
        else:
            benign.append(name)
    return sorted(set(hard)), sorted(set(benign))


def _mutually_exclusive(path_a: tuple[int, ...], path_b: tuple[int, ...]) -> bool:
    """True when two branch paths select different, mutually exclusive branches.

    The paths differ at a frame only when BOTH have that frame — a shorter
    path is not a different branch, it is the same branch seen from
    outside one more conditional.
    """
    for i in range(min(len(path_a), len(path_b))):
        if path_a[i] != path_b[i]:
            return True
    return False


def read_header(include_dir: str, header: str) -> str:
    path = os.path.join(include_dir, header)
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


# ── Archive inspection ─────────────────────────────────────────────────

def archive_exports(archive: str) -> set[str]:
    """Return the global symbols an archive defines.

    Only global definitions count: a local symbol (lowercase nm type) is
    not an export.  Leading underscores are NOT filtered out — POSIX
    reserves the `_`-prefixed namespace for the implementation, so
    `__stack_chk_fail` and `_exit` are ordinary public exports.
    """
    try:
        out = subprocess.run(
            ["nm", "--defined-only", archive],
            capture_output=True, text=True, check=False,
        )
    except OSError as exc:  # pragma: no cover - environment guard
        raise SystemExit(f"ERROR: cannot run nm on {archive}: {exc}")
    syms: set[str] = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TDBRWV":
            syms.add(parts[2])
    return syms


# ── Manifest access ────────────────────────────────────────────────────

def enabled_groups() -> list[str]:
    return [g for g in ENABLED_GROUPS if g in API_GROUPS]


def group_headers(group: str) -> dict[str, list[str]]:
    return dict(API_GROUPS[group]["headers"])


def all_group_symbols(group: str) -> set[str]:
    """The interfaces a group is responsible for.

    A group that declares an "owned" map owns exactly those symbols — even
    when the header they live in also declares interfaces other groups own.
    A group without an "owned" map owns every symbol of the headers it
    audits.
    """
    info = API_GROUPS[group]
    owned = info.get("owned")
    source = owned if owned is not None else info["headers"]
    syms: set[str] = set()
    for names in source.values():
        syms.update(names)
    return syms


def libk_required(group: str, profile: str = "x86_64-clang") -> set[str]:
    """The libk requirement set for a group on a profile.

    Everything the group lists is a user-libc requirement.  libk is only
    required to keep providing the already-supported subset; the symbols
    libk does not have, and the symbols in LIBK_NOT_REQUIRED (user-only
    aligned allocation, the regex group, the float-scanning group) are not
    libk requirements.  The explicitly shared symbols are added when their
    group delivers them.
    """
    syms = all_group_symbols(group)
    supported = LIBK_SUPPORTED.get(profile, frozenset())
    required = {
        s for s in syms
        if s in supported
        and s not in LIBK_NOT_REQUIRED
    }
    required |= (syms & LIBK_SHARED)
    return required


# ── C manifest emission ────────────────────────────────────────────────

def classified_headers(include_dir: str, groups: list[str]) -> set[str]:
    """The headers the manifest covers: every group header plus the
    explicitly excluded ones."""
    headers: set[str] = set(EXCLUDED_HEADERS)
    for group in groups:
        headers.update(group_headers(group))
    return headers


def check_manifest_against_headers(include_dir: str, groups: list[str]) -> list[str]:
    """Return the drift errors between the manifest and the shipped headers.

    The same checks the --audit mode performs, but without the archive half:
    every header under include_dir must be classified, and every group's
    header->symbol map must match the header's declared functions EXACTLY.
    """
    errors: list[str] = []
    classified: set[str] = set(EXCLUDED_HEADERS)
    for group in groups:
        classified.update(group_headers(group))
    for root, _dirs, files in os.walk(include_dir):
        for name in files:
            if not name.endswith(".h"):
                continue
            rel = os.path.relpath(os.path.join(root, name), include_dir)
            if rel not in classified:
                errors.append(
                    f"{rel}: public header is neither in an enabled group nor "
                    f"in EXCLUDED_HEADERS"
                )
    for group in groups:
        for header, syms in sorted(group_headers(group).items()):
            path = os.path.join(include_dir, header)
            if not os.path.isfile(path):
                errors.append(f"{group}/{header}: missing from {include_dir}")
                continue
            declared = [
                s for s in declared_functions(read_header(include_dir, header))
                if s not in EXCLUDED_SYMBOLS
            ]
            missing = [s for s in syms if s not in declared]
            unlisted = [s for s in declared if s not in syms]
            if missing:
                errors.append(
                    f"{group}/{header}: manifest lists symbols the header does not "
                    f"declare: {', '.join(missing)}"
                )
            if unlisted:
                errors.append(
                    f"{group}/{header}: header declares symbols missing from the "
                    f"manifest: {', '.join(unlisted)}"
                )
            # A function declared twice in one header is a real defect (the
            # historical cause was declarations placed after the include
            # guard's #endif, which left the tail outside the guard).  The
            # "not in" comparisons above cannot see it: a duplicate is still
            # "in" the manifest, so it never shows up as unlisted.
            hard, benign = duplicate_declarations(
                read_header(include_dir, header))
            hard = [s for s in hard if s not in EXCLUDED_SYMBOLS]
            benign = [s for s in benign if s not in EXCLUDED_SYMBOLS]
            if hard:
                errors.append(
                    f"{group}/{header}: header declares the same function more "
                    f"than once: {', '.join(hard)}"
                )
            if benign:
                notes.append(
                    f"{group}/{header}: the same name is declared again with a "
                    f"different signature or inside a conditional branch "
                    f"(legal C): {', '.join(benign)}"
                )
    return errors


def emit_c_header(path: str, groups: list[str]) -> None:
    lines: list[str] = []
    lines.append("/* GENERATED by scripts/audit_libc_api.py -- DO NOT EDIT. */")
    lines.append("#ifndef OS01_LIBC_API_MANIFEST_H")
    lines.append("#define OS01_LIBC_API_MANIFEST_H")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char *group;")
    lines.append("    const char *header;")
    lines.append("    const char *const *symbols;")
    lines.append("    unsigned nsymbols;")
    lines.append("} os01_api_header_t;")
    lines.append("")
    index = 0
    entries: list[str] = []
    for group in groups:
        for header, syms in sorted(group_headers(group).items()):
            lines.append(f"static const char *const os01_api_syms_{index}[] = {{")
            for sym in syms:
                lines.append(f'    "{sym}",')
            if not syms:
                # a zero length array is not valid C
                lines.append("    NULL,")
            lines.append("};")
            entries.append(
                f'    {{ "{group}", "{header}", os01_api_syms_{index}, {len(syms)} }},'
            )
            index += 1
    lines.append("")
    lines.append("static const os01_api_header_t os01_api_headers[] = {")
    lines.extend(entries)
    lines.append("};")
    lines.append("")
    lines.append(f"#define OS01_API_HEADER_COUNT {index}")
    lines.append(f"#define OS01_API_GROUP_COUNT {len(groups)}")
    lines.append("")
    lines.append("static const char *const os01_api_group_names[] = {")
    for group in groups:
        lines.append(f'    "{group}",')
    lines.append("};")
    lines.append("")
    lines.append("#endif /* OS01_LIBC_API_MANIFEST_H */")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


# ── Audit ──────────────────────────────────────────────────────────────

def infer_profile(*paths: str | None) -> str | None:
    """Infer the build profile from an archive path.

    The libk baseline is per profile (an x86_64 libk.a and an aarch64
    libk.a export different subsets), so the audit must know which profile
    it is looking at.  The archive path carries it: ``build/<profile>/...``.
    """
    for path in paths:
        if not path:
            continue
        parts = os.path.normpath(path).split(os.sep)
        for i, part in enumerate(parts):
            if part == "build" and i + 1 < len(parts):
                return parts[i + 1]
    return None


def audit(include_dir: str, libc_archive: str | None, libk_archive: str | None,
          profile: str = "x86_64-clang", groups: list[str] | None = None) -> int:
    errors: list[str] = []
    notes: list[str] = []
    if groups is None:
        groups = enabled_groups()

    # Every header under libc/include must be classified: either it belongs
    # to an enabled group or it is explicitly excluded with a reason.  A
    # header that is neither would silently escape the audit.
    classified: set[str] = set(EXCLUDED_HEADERS)
    for group in groups:
        classified.update(group_headers(group))
    for root, _dirs, files in os.walk(include_dir):
        for name in files:
            if not name.endswith(".h"):
                continue
            rel = os.path.relpath(os.path.join(root, name), include_dir)
            if rel not in classified:
                errors.append(
                    f"{rel}: public header is neither in an enabled group nor "
                    f"in EXCLUDED_HEADERS"
                )

    for group in groups:
        info = API_GROUPS[group]
        print(f"[audit] group '{group}' (task {info['task']}): {info['title']}")
        headers = group_headers(group)
        for header, syms in sorted(headers.items()):
            path = os.path.join(include_dir, header)
            if not os.path.isfile(path):
                errors.append(f"{group}/{header}: missing from {include_dir}")
                continue
            declared = [
                s for s in declared_functions(read_header(include_dir, header))
                if s not in EXCLUDED_SYMBOLS
            ]
            missing = [s for s in syms if s not in declared]
            unlisted = [s for s in declared if s not in syms]
            if missing:
                errors.append(
                    f"{group}/{header}: manifest lists symbols the header does not "
                    f"declare: {', '.join(missing)}"
                )
            if unlisted:
                errors.append(
                    f"{group}/{header}: header declares symbols missing from the "
                    f"manifest: {', '.join(unlisted)}"
                )
            hard, benign = duplicate_declarations(
                read_header(include_dir, header))
            hard = [s for s in hard if s not in EXCLUDED_SYMBOLS]
            benign = [s for s in benign if s not in EXCLUDED_SYMBOLS]
            if hard:
                errors.append(
                    f"{group}/{header}: header declares the same function more "
                    f"than once: {', '.join(hard)}"
                )
            if benign:
                notes.append(
                    f"{group}/{header}: the same name is declared again with a "
                    f"different signature or inside a conditional branch "
                    f"(legal C): {', '.join(benign)}"
                )
        for header, reason in sorted(EXCLUDED_HEADERS.items()):
            notes.append(f"excluded header {header}: {reason}")
        for symbol, reason in sorted(EXCLUDED_SYMBOLS.items()):
            notes.append(f"excluded symbol {symbol}: {reason}")

    if libc_archive:
        libc_syms = archive_exports(libc_archive)
        for group in groups:
            want = all_group_symbols(group)
            have = sorted(want & libc_syms)
            missing = sorted(want - libc_syms)
            print(f"[audit] user libc '{libc_archive}': {len(have)}/{len(want)} "
                  f"api-group interfaces exported")
            if missing:
                print(f"[audit]   declared but not exported by libc.a: "
                      f"{', '.join(missing)}")
            if API_GROUPS[group].get("require_user_export") and missing:
                errors.append(
                    f"{group}: user libc is missing required exports: "
                    f"{', '.join(missing)}"
                )
    if libk_archive:
        libk_syms = archive_exports(libk_archive)
        for group in groups:
            want = libk_required(group, profile)
            missing = sorted(want - libk_syms)
            print(f"[audit] libk '{libk_archive}': {len(want & libk_syms)}/{len(want)} "
                  f"already-supported interfaces still exported")
            if missing:
                errors.append(
                    f"{group}: libk lost an already-supported export: "
                    f"{', '.join(missing)}"
                )
            gained = sorted(
                s for s in ("posix_memalign", "aligned_alloc")
                if s in libk_syms
            )
            if gained:
                errors.append(
                    f"libk must not define the user-only aligned-allocation "
                    f"symbols: {', '.join(gained)}"
                )

    for note in notes:
        print(f"[audit] note: {note}")
    if errors:
        print("")
        for err in errors:
            print(f"[audit] ERROR: {err}")
        return 1
    print("[audit] OK")
    return 0


def list_manifest(profile: str = "x86_64-clang") -> int:
    for group in enabled_groups():
        info = API_GROUPS[group]
        print(f"group {group} (task {info['task']}): {info['title']}")
        for header, syms in sorted(group_headers(group).items()):
            print(f"  {header}: {len(syms)} interface(s)")
        owned = info.get("owned")
        if owned is not None:
            for header, syms in sorted(owned.items()):
                print(f"  owns {header}: {', '.join(syms)}")
        print(f"  libk requirement set: {len(libk_required(group, profile))} symbol(s)")
    print("")
    print("excluded headers:")
    for header, reason in sorted(EXCLUDED_HEADERS.items()):
        print(f"  {header}: {reason}")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true",
                    help="print the enabled manifest")
    ap.add_argument("--emit-c-header", metavar="PATH",
                    help="write the C manifest consumed by the host test")
    ap.add_argument("--audit", action="store_true",
                    help="audit the shipped headers (and archives if given)")
    ap.add_argument("--check-headers", action="store_true",
                    help="check the manifest against the headers only "
                         "(no archive); fails on drift")
    ap.add_argument("--groups", default="",
                    help="comma separated group list (default: enabled groups)")
    ap.add_argument("--include-dir", default=os.path.join(REPO_ROOT, "libc", "include"))
    ap.add_argument("--libc-archive", default="")
    ap.add_argument("--libk-archive", default="")
    ap.add_argument("--profile", default="",
                    help="build profile whose libk baseline applies "
                         "(default: inferred from the archive path, "
                         "else x86_64-clang)")
    args = ap.parse_args(argv)

    profile = args.profile or infer_profile(args.libc_archive, args.libk_archive) \
        or "x86_64-clang"
    if profile not in LIBK_SUPPORTED:
        print(f"ERROR: unknown profile '{profile}'; known: "
              f"{', '.join(sorted(LIBK_SUPPORTED))}", file=sys.stderr)
        return 2

    if args.groups:
        groups = [g.strip() for g in args.groups.split(",") if g.strip()]
        unknown = [g for g in groups if g not in API_GROUPS]
        if unknown:
            print(f"ERROR: unknown group(s): {', '.join(unknown)}", file=sys.stderr)
            return 2
    else:
        groups = enabled_groups()

    if args.check_headers:
        # The header half of the audit, on its own.  The api group runs
        # this on EVERY invocation: the manifest is only regenerated when
        # the script or a header changes, so a drift check that lived
        # inside --emit-c-header would be skipped on an ordinary rebuild.
        drift = check_manifest_against_headers(args.include_dir, groups)
        if drift:
            print("ERROR: the API manifest has drifted from the headers:",
                  file=sys.stderr)
            for e in drift:
                print(f"  {e}", file=sys.stderr)
            return 2
        print(f"[audit] headers OK ({len(groups)} group(s), "
              f"{len(classified_headers(args.include_dir, groups))} header(s))")
        return 0
    if args.emit_c_header:
        # The manifest is emitted at build time, so the drift check runs on
        # EVERY build that consumes it: a header that was added or changed
        # without updating the manifest fails the build here instead of
        # being noticed by hand later.
        drift = check_manifest_against_headers(args.include_dir, groups)
        if drift:
            print("ERROR: the API manifest has drifted from the headers:",
                  file=sys.stderr)
            for e in drift:
                print(f"  {e}", file=sys.stderr)
            print("ERROR: fix scripts/audit_libc_api.py (and every group that "
                  "lists the affected header) or the header itself.",
                  file=sys.stderr)
            return 2
        emit_c_header(args.emit_c_header, groups)
        return 0
    if args.audit:
        return audit(args.include_dir, args.libc_archive or None,
                     args.libk_archive or None, profile, groups)
    return list_manifest(profile)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

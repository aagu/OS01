// libc/unistd/fcntl.c — fcntl(2) wrapper.
//
// Contract (libc-completion plan T7):
//   - F_GETFL / F_GETFD take NO third argument: va_arg on a call that
//     passed none is undefined behaviour, so these commands must not
//     read the variable-argument list at all.
//   - F_SETFL / F_SETFD / F_DUPFD / F_DUPFD_CLOEXEC take an int: the
//     list is read as EXACTLY an int (the caller's int is what is in
//     the register save area — reading a wider type would consume
//     garbage from the padding).
//   - Every other command is rejected with EINVAL here, without a
//     syscall and without touching the argument list.
//   - A negative kernel answer (the kernel returns -errno encoded in
//     the result) is mapped to -1 with errno set, per POSIX.
//
// There is no O_CLOEXEC promise anywhere: F_SETFD accepts the flag and
// the kernel answers 0, but exec() does not honour it yet — the header
// deliberately declares no O_CLOEXEC and fdopen() never claims it.

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/syscall.h>

int fcntl(int fd, int cmd, ...)
{
    int64_t r;

    switch (cmd) {
    case F_GETFL:
    case F_GETFD:
        r = (int64_t)syscall(SYS_fcntl, (uint64_t)fd, (uint64_t)cmd, 0UL);
        break;

    case F_SETFL:
    case F_SETFD:
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        va_list ap;
        va_start(ap, cmd);
        int arg = va_arg(ap, int);
        va_end(ap);
        r = (int64_t)syscall(SYS_fcntl, (uint64_t)fd, (uint64_t)cmd,
                             (uint64_t)(unsigned int)arg);
        break;
    }

    default:
        errno = EINVAL;
        return -1;
    }

    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}

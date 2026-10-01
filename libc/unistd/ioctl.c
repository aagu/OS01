#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    uint64_t arg = va_arg(ap, uint64_t);
    va_end(ap);

    // Spec §5: the kernel returns -errno directly (preserved across
    // the syscall ABI so call sites that need the raw negative value
    // can still get it).  libc, however, must publish the OS-style
    // "return -1, set errno" contract — without it, every ioctl
    // caller has to repeat the same negative-to-errno translation,
    // and busybox code that tests `if (ioctl(...) < 0)` against
    // user-provided errno values silently mis-behaves.  The new
    // /dev/gfx0 path (GfxView.present, gfx_get_info failure) relies
    // on this for ENOTTY/EFAULT propagation.  Zero return is
    // preserved verbatim (success).
    int rc = (int)syscall(SYS_ioctl, (uint64_t)fd, request, arg);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

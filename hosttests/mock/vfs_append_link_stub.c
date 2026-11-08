/*
 * hosttests/mock/vfs_append_link_stub.c
 *
 * libc-completion Task T6 — the O_APPEND path in kernel/fs/file.c now
 * dispatches through vfs_append() (declared by <fs/vfs.h>, defined in
 * kernel/fs/vfs.c).  The host harnesses that compile kernel/fs/file.c
 * against a REDUCED mock runtime (poll / PTY / fd_install / boot stdio /
 * gfx) do NOT link kernel/fs/vfs.c, so the new call site needs a
 * definition to stay linkable.
 *
 * This stub is that definition.  It is intentionally NOT weak: every
 * binary that links a kernel/fs/file.c object compiled without
 * kernel/fs/vfs.c MUST link this stub explicitly (same rule as
 * nofile_soft_stub.c — the link must not depend on --gc-sections
 * dropping the fd_write section), and a binary that links BOTH the
 * stub and the real vfs.c fails the link with a duplicate symbol
 * instead of silently keeping the stub.
 *
 * None of today's consumers can reach the call: the O_APPEND branch is
 * guarded by f->type == FD_VFS, and those harnesses either never call
 * fd_write on an FD_VFS descriptor at all (poll / fd_install / PTY /
 * boot stdio) or only write to FD_DEV descriptors (gfx).  The return
 * value mirrors vfs_append()'s own answer for a filesystem that has no
 * append hook but is writable, so an accidental future caller fails
 * with the kernel's real errno instead of a made-up one.
 */

#include <errno.h>
#include <stdint.h>

struct file;

int vfs_append(struct file *f, uint64_t size, const void *buffer)
{
    (void)f;
    (void)size;
    (void)buffer;
    return -EOPNOTSUPP;
}

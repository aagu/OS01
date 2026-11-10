/*
 * libc/stdio/remove.c — remove(): delete a file, a directory or a link.
 *
 * remove(path) removes the named object:
 *
 *   - a DIRECTORY is removed with rmdir();
 *   - everything else (a regular file, a symbolic link, a device node)
 *     is removed with unlink().
 *
 * The classification uses lstat(), NOT stat(): lstat() does not follow the
 * final component, so a symbolic link is seen as the LINK itself.  A link
 * is therefore removed with unlink(), which removes the link and never
 * its target — the target stays exactly as it was.
 *
 * Exactly ONE deletion call is made.  There is deliberately NO fallback:
 * if rmdir() fails (a non-empty directory, for instance), the error is
 * reported as it is, and unlink() is NOT tried afterwards.  A fallback
 * would both destroy an object the caller was told about as undeletable
 * and overwrite the errno of the failure that actually happened.
 *
 * The errno of the failing call is preserved: the lstat() failure keeps
 * lstat()'s errno, the deletion failure keeps the deletion's errno.
 */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef __is_libk
int remove(const char *path)
{
    struct stat st;

    /* Classify the path.  lstat() does not follow the final component, so
     * a symbolic link is classified as the link itself. */
    if (lstat(path, &st) != 0)
        return -1;              /* errno is the one lstat() set */

    if (S_ISDIR(st.st_mode))
        return rmdir(path);     /* a directory goes through rmdir() */

    return unlink(path);        /* everything else goes through unlink() */
}
#else
/* The kernel side has no user filesystem: remove() is a user interface,
 * so the libk build answers the way the other user-only wrappers do —
 * with a failure, and without touching errno. */
int remove(const char *path)
{
    (void)path;
    return -1;
}
#endif

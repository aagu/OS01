#include <libgen.h>
#include <string.h>

/*
 * POSIX (SUSv4) dirname/basename.  Both may modify the input string and
 * return a pointer into it (or to static storage for the degenerate
 * cases).  Callers must pass a modifiable buffer.
 *
 *   dirname:  /usr/lib -> /usr ; /usr/ -> / ; usr -> . ; / -> / ; "" -> .
 *   basename: /usr/lib -> lib ; /usr/ -> usr ; usr -> usr ; / -> / ; "" -> .
 */

char *dirname(char *path)
{
    static char dot[2] = ".";
    if (!path || !*path)
        return dot;

    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';            /* strip trailing slashes */
    if (len == 1 && path[0] == '/')
        return path;                   /* "/" -> "/" */

    char *slash = strrchr(path, '/');
    if (!slash)
        return dot;                    /* no slash -> "." */
    if (slash == path)
        return path;                   /* "/foo" -> "/" */
    *slash = '\0';                     /* "a/b/c" -> "a/b" */
    return path;
}

char *basename(char *path)
{
    static char dot[2] = ".";
    if (!path || !*path)
        return dot;

    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';            /* strip trailing slashes */
    if (len == 1 && path[0] == '/')
        return path;                   /* "/" -> "/" */

    char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

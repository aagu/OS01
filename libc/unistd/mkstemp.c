#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

int mkstemp(char *tmpl) {
    size_t len = strlen(tmpl);
    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) { errno = EINVAL; return -1; }
    /* Try a few times with different patterns */
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 6; j++)
            tmpl[len - 6 + j] = 'a' + (i * 37 + j * 7) % 26;
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
    }
    errno = EEXIST;
    return -1;
}

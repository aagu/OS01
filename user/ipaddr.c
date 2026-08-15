#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

int main(void)
{
    /* get netif IPv4 address via syscall 61 (SYS_getifaddr) */
    int64_t ip = syscall(61, 0, 0, 0);
    if (ip <= 0) {
        write(2, "no IP\n", 6);
        exit(1);
    }

    /* ip is in network byte order, print MSB first */
    unsigned char *b = (unsigned char *)&ip;
    char out[20];
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        unsigned int o = (unsigned int)b[i];
        if (o >= 100) out[p++] = 48 + o / 100;
        if (o >= 10)  out[p++] = 48 + (o / 10) % 10;
        out[p++] = 48 + o % 10;
        if (i > 0) out[p++] = 46;
    }
    out[p++] = 10;
    write(1, out, p);
    exit(0);
}

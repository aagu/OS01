#include <arpa/inet.h>
#include <string.h>

int inet_aton(const char *cp, struct in_addr *inp)
{
    if (!cp || !inp) return 0;
    unsigned int a, b, c, d;
    int n = 0;
    (void)n;
    // Simple sscanf-style parse
    const char *p = cp;
    a = 0; while (*p >= '0' && *p <= '9') a = a * 10 + (unsigned int)(*p++ - '0');
    if (*p++ != '.') return 0;
    b = 0; while (*p >= '0' && *p <= '9') b = b * 10 + (unsigned int)(*p++ - '0');
    if (*p++ != '.') return 0;
    c = 0; while (*p >= '0' && *p <= '9') c = c * 10 + (unsigned int)(*p++ - '0');
    if (*p++ != '.') return 0;
    d = 0; while (*p >= '0' && *p <= '9') d = d * 10 + (unsigned int)(*p++ - '0');
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    inp->s_addr = (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
    return 1;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[16];
    uint32_t ip = in.s_addr;
    // Use built-in snprintf-like approach
    unsigned int aa = (ip >> 0)  & 0xFF;
    unsigned int bb = (ip >> 8)  & 0xFF;
    unsigned int cc = (ip >> 16) & 0xFF;
    unsigned int dd = (ip >> 24) & 0xFF;
    int pos = 0;
    // Manual itoa
    unsigned int v[4] = {aa, bb, cc, dd};
    for (int i = 0; i < 4; i++) {
        if (i > 0) buf[pos++] = '.';
        if (v[i] >= 100) buf[pos++] = '0' + (char)(v[i]/100);
        if (v[i] >= 10)  buf[pos++] = '0' + (char)((v[i]/10)%10);
        buf[pos++] = '0' + (char)(v[i]%10);
    }
    buf[pos] = 0;
    return buf;
}

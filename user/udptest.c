// udptest.c — minimal UDP DNS query to 10.0.2.3:53, with diagnostics
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

int main(void) {
    write(1, "udptest: socket()\n", 18);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { write(1, "udptest: socket FAIL\n", 21); return 1; }
    write(1, "udptest: socket OK\n", 19);

    struct sockaddr_in dns = {0};
    dns.sin_family = AF_INET;
    dns.sin_port = htons(53);
    struct in_addr ia;
    if (!inet_aton("10.0.2.3", &ia)) {
        write(1, "udptest: inet_aton FAIL\n", 24);
        return 1;
    }
    dns.sin_addr = ia;

    // Build DNS query: example.com A
    uint8_t q[512] = {0};
    q[0] = 0x12; q[1] = 0x34;         // TXID
    q[2] = 0x01; q[3] = 0x00;         // RD
    q[5] = 0x01;                       // QDCOUNT=1
    int qi = 12;
    const char *name = "example.com";
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t seg = dot ? (size_t)(dot - p) : strlen(p);
        q[qi++] = (uint8_t)seg;
        memcpy(&q[qi], p, seg); qi += (int)seg;
        p = dot ? dot + 1 : "";
    }
    q[qi++] = 0;
    q[qi++] = 0; q[qi++] = 1;   // QTYPE=A
    q[qi++] = 0; q[qi++] = 1;   // QCLASS=IN

    write(1, "udptest: sendto()\n", 18);
    int sr = sendto(fd, q, qi, 0, (struct sockaddr *)&dns, sizeof(dns));
    if (sr < 0) {
        write(1, "udptest: sendto FAIL\n", 21);
        return 1;
    }
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "udptest: sent %d bytes\n", sr);
    write(1, msg, n);

    // NOTE: SO_RCVTIMEO is a no-op in OS01 (do_setsockopt stub).
    // recvfrom may block forever if the DNS response never arrives.

    write(1, "udptest: recvfrom()...\n", 23);
    uint8_t reply[512];
    int r = recvfrom(fd, reply, sizeof(reply), 0, NULL, 0);
    if (r < 0) {
        write(1, "udptest: recvfrom FAIL/blocked\n", 30);
        return 2;
    }
    n = snprintf(msg, sizeof(msg), "udptest: got %d bytes\n", r);
    write(1, msg, n);
    if (r >= 12) {
        write(1, "udptest: DNS response OK\n", 25);
    }
    return 0;
}

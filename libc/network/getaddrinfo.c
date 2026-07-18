// libc/network/getaddrinfo.c — minimal DNS resolution for wget
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

// Use QEMU user-mode NAT gateway's built-in DNS proxy
#define DNS_IP     0x0302000A  // 10.0.2.3 in network byte order
#define DNS_PORT   53

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    (void)service; (void)hints;
    if (!node || !res) return EAI_NONAME;
    *res = NULL;

    // Try dotted-quad first
    struct in_addr ip;
    if (inet_aton(node, &ip)) {
        struct addrinfo *ai = calloc(1, sizeof(*ai));
        if (!ai) return EAI_MEMORY;
        ai->ai_family = AF_INET;
        ai->ai_socktype = SOCK_STREAM;
        struct sockaddr_in *sa = calloc(1, sizeof(*sa));
        if (!sa) { free(ai); return EAI_MEMORY; }
        sa->sin_family = AF_INET;
        memcpy(&sa->sin_addr, &ip, sizeof(ip));
        ai->ai_addr = (struct sockaddr *)sa;
        ai->ai_addrlen = sizeof(*sa);
        *res = ai;
        return 0;
    }

    // DNS A-record query
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return EAI_SYSTEM;

    struct sockaddr_in dns = {0};
    dns.sin_family = AF_INET;
    dns.sin_port = htons(DNS_PORT);
    dns.sin_addr.s_addr = DNS_IP;

    uint8_t query[512] = {0};
    query[0] = 0x00; query[1] = 0x01;  // TXID=1
    query[2] = 0x01; query[3] = 0x00;  // flags: RD=1
    query[5] = 0x01;                    // QDCOUNT=1

    int qi = 12;
    const char *p = node;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t seg = dot ? (size_t)(dot - p) : strlen(p);
        query[qi++] = (uint8_t)seg;
        memcpy(&query[qi], p, seg); qi += (int)seg;
        p = dot ? dot + 1 : "";
    }
    query[qi++] = 0;
    query[qi++] = 0; query[qi++] = 1;   // QTYPE=A
    query[qi++] = 0; query[qi++] = 1;   // QCLASS=IN

    sendto(fd, query, qi, 0, (struct sockaddr *)&dns, sizeof(dns));

    uint8_t reply[512];
    int64_t n = recvfrom(fd, reply, sizeof(reply), 0, NULL, 0);
    close(fd);
    if (n < 12) return EAI_FAIL;

    // Parse answer: skip header + question
    int a = qi;
    if (a >= (int)n) return EAI_FAIL;
    if (reply[a] == 0xC0) a += 2;  // compressed name pointer
    a += 10;  // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)
    if (a + 4 > (int)n) return EAI_FAIL;
    uint32_t result_ip;
    memcpy(&result_ip, &reply[a], 4);

    struct addrinfo *ai = calloc(1, sizeof(*ai));
    if (!ai) return EAI_MEMORY;
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    if (!sa) { free(ai); return EAI_MEMORY; }
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = result_ip;
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_addrlen = sizeof(*sa);
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int ecode) {
    (void)ecode;
    return "DNS error";
}

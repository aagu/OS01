// libc/network/getaddrinfo.c — minimal IPv4 DNS resolution
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
#define DNS_PACKET_SIZE 512

static uint16_t dns_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// Skip one DNS wire-format name. Compression pointers terminate a name.
static int dns_skip_name(const uint8_t *packet, size_t packet_len, size_t *off)
{
    size_t p = *off;
    size_t labels = 0;

    while (p < packet_len) {
        uint8_t len = packet[p++];
        if (len == 0) {
            *off = p;
            return 0;
        }
        if ((len & 0xC0) == 0xC0) {
            if (p >= packet_len) return -1;
            *off = p + 1;
            return 0;
        }
        if ((len & 0xC0) != 0 || len > 63 || len > packet_len - p)
            return -1;
        p += len;
        if (++labels > 127) return -1;
    }
    return -1;
}

static int make_result(uint32_t address, uint16_t port,
                       const struct addrinfo *hints, struct addrinfo **res)
{
    struct addrinfo *ai = calloc(1, sizeof(*ai));
    if (!ai) return EAI_MEMORY;

    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    if (!sa) {
        free(ai);
        return EAI_MEMORY;
    }

    ai->ai_family = AF_INET;
    ai->ai_socktype = (hints && hints->ai_socktype)
                      ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = sizeof(*sa);
    ai->ai_addr = (struct sockaddr *)sa;
    sa->sin_family = AF_INET;
    sa->sin_port = port;
    sa->sin_addr.s_addr = address;
    *res = ai;
    return 0;
}

static int parse_service(const char *service, const struct addrinfo *hints,
                         uint16_t *port)
{
    *port = 0;
    if (!service) return 0;
    if (!*service) return EAI_SERVICE;

    char *end = NULL;
    unsigned long value = strtoul(service, &end, 10);
    if (end != service && *end == '\0' && value <= 65535) {
        *port = htons((uint16_t)value);
        return 0;
    }

    const char *proto = (hints && hints->ai_socktype == SOCK_DGRAM)
                        ? "udp" : "tcp";
    struct servent *entry = getservbyname(service, proto);
    if (!entry) return EAI_SERVICE;
    *port = (uint16_t)entry->s_port;
    return 0;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    if (!node || !res) return EAI_NONAME;
    *res = NULL;

    uint16_t service_port;
    int service_err = parse_service(service, hints, &service_port);
    if (service_err) return service_err;

    // Try dotted-quad first.
    struct in_addr ip;
    if (inet_aton(node, &ip))
        return make_result(ip.s_addr, service_port, hints, res);

    uint8_t query[DNS_PACKET_SIZE] = {0};
    query[0] = 0x00; query[1] = 0x01;  // TXID=1
    query[2] = 0x01; query[3] = 0x00;  // flags: RD=1
    query[5] = 0x01;                    // QDCOUNT=1

    size_t qi = 12;
    const char *p = node;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len = dot ? (size_t)(dot - p) : strlen(p);
        // Reserve the root label and QTYPE/QCLASS as well as this label.
        if (label_len == 0 || label_len > 63 ||
            label_len > DNS_PACKET_SIZE - qi - 6)
            return EAI_NONAME;
        query[qi++] = (uint8_t)label_len;
        memcpy(&query[qi], p, label_len);
        qi += label_len;
        p = dot ? dot + 1 : "";
    }
    query[qi++] = 0;
    query[qi++] = 0; query[qi++] = 1;   // QTYPE=A
    query[qi++] = 0; query[qi++] = 1;   // QCLASS=IN

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return EAI_SYSTEM;

    struct sockaddr_in dns = {0};
    dns.sin_family = AF_INET;
    dns.sin_port = htons(DNS_PORT);
    dns.sin_addr.s_addr = DNS_IP;

    if (sendto(fd, query, qi, 0, (struct sockaddr *)&dns, sizeof(dns)) < 0) {
        close(fd);
        return EAI_SYSTEM;
    }

    uint8_t reply[DNS_PACKET_SIZE];
    int64_t nread = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
    close(fd);
    if (nread < 12) return EAI_FAIL;
    size_t n = (size_t)nread;

    uint16_t flags = dns_u16(&reply[2]);
    uint16_t qdcount = dns_u16(&reply[4]);
    uint16_t ancount = dns_u16(&reply[6]);
    if (dns_u16(reply) != 1 || !(flags & 0x8000)) return EAI_FAIL;
    switch (flags & 0x000f) {
    case 0: break;
    case 2: return EAI_AGAIN;   // SERVFAIL
    case 3: return EAI_NONAME;  // NXDOMAIN
    default: return EAI_FAIL;
    }
    if (qdcount == 0 || ancount == 0) return EAI_NONAME;

    size_t off = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        if (dns_skip_name(reply, n, &off) < 0 || off > n || n - off < 4)
            return EAI_FAIL;
        off += 4;
    }

    for (uint16_t i = 0; i < ancount; i++) {
        if (dns_skip_name(reply, n, &off) < 0 || off > n || n - off < 10)
            return EAI_FAIL;
        uint16_t type = dns_u16(&reply[off]);
        uint16_t class = dns_u16(&reply[off + 2]);
        uint16_t rdlength = dns_u16(&reply[off + 8]);
        off += 10;
        if (rdlength > n - off) return EAI_FAIL;
        if (type == 1 && class == 1 && rdlength == 4) {
            uint32_t result_ip;
            memcpy(&result_ip, &reply[off], sizeof(result_ip));
            return make_result(result_ip, service_port, hints, res);
        }
        off += rdlength;
    }
    return EAI_NONAME;
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

const char *gai_strerror(int ecode)
{
    (void)ecode;
    return "DNS error";
}

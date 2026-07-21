#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    /* 1. socket() */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { write(2, "socket FAIL\n", 12); exit(1); }
    write(1, "socket OK\n", 10);

    /* 2. DNS resolve example.com */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);

    /* Try 93.184.216.34 (example.com) directly */
    struct in_addr ip;
    if (!inet_aton("93.184.216.34", &ip)) {
        write(2, "inet_aton FAIL\n", 15); exit(1);
    }
    addr.sin_addr = ip;
    write(1, "resolve OK (93.184.216.34)\n", 27);

    /* 3. connect() */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        write(2, "connect FAIL\n", 13); close(fd); exit(1);
    }
    write(1, "connect OK\n", 11);

    /* 4. send HTTP request */
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    write(fd, req, strlen(req));
    write(1, "request sent\n", 13);

    /* 5. recv response */
    char buf[4096];
    for (int i = 0; i < 5; i++) {
        int64_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        write(1, buf, (uint64_t)n);
    }
    write(1, "\n--- done ---\n", 14);

    close(fd);
    exit(0);
}

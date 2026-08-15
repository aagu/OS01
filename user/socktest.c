#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    /* 1. socket() */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { write(2, "socket FAIL\n", 12); exit(1); }
    write(1, "socket OK\n", 10);

    /* 2. Resolve the current address instead of relying on a stale A record. */
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("example.com", "80", &hints, &result) != 0 || !result) {
        write(2, "resolve FAIL\n", 13); close(fd); exit(1);
    }
    write(1, "resolve OK\n", 11);

    /* 3. connect() */
    if (connect(fd, result->ai_addr, result->ai_addrlen) < 0) {
        write(2, "connect FAIL\n", 13);
        freeaddrinfo(result); close(fd); exit(1);
    }
    freeaddrinfo(result);
    write(1, "connect OK\n", 11);

    /* 4. send HTTP request */
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    write(fd, req, strlen(req));
    write(1, "request sent\n", 13);

    /* 5. recv response */
    char buf[4096];
    for (int i = 0; i < 5; i++) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 5000);
        if (ready <= 0 || !(pfd.revents & (POLLIN | POLLHUP))) break;
        int64_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        write(1, buf, (uint64_t)n);
    }
    write(1, "\n--- done ---\n", 14);

    close(fd);
    exit(0);
}

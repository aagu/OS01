// nettest.c — deterministic QEMU network regression test.
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HOST_IP "10.0.2.2"
#define UDP_PORT 10001
#define TCP_PORT 10002
#define HTTP_PORT 18080

static int passed;
static int failed;

static void result(const char *name, int ok)
{
    printf("[NET TEST] %s: %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) passed++; else failed++;
}

static int wait_for_dhcp(void)
{
    struct in_addr expected;
    if (!inet_aton("10.0.2.20", &expected))
        return 0;
    for (int i = 0; i < 40; i++) {
        uint32_t current = (uint32_t)syscall(SYS_getifaddr, 0, 0, 0);
        if (current == expected.s_addr)
            return 1;
        if (poll(NULL, 0, 250) < 0)
            return 0;
    }
    return 0;
}

static int wait_readable(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 5000);
    return ready == 1 && (pfd.revents & POLLIN);
}

static int test_udp(void)
{
    static const char message[] = "os01-udp";
    char reply[32];
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(UDP_PORT);
    if (!inet_aton(HOST_IP, &peer.sin_addr) ||
        sendto(fd, message, sizeof(message), 0,
               (struct sockaddr *)&peer, sizeof(peer)) != (int)sizeof(message) ||
        !wait_readable(fd)) {
        close(fd);
        return 0;
    }
    int n = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
    close(fd);
    return n == (int)sizeof(message) && !memcmp(reply, message, sizeof(message));
}

static int test_dns(void)
{
    struct addrinfo hints = {0};
    struct addrinfo *answer = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo("localhost", "80", &hints, &answer);
    printf("[NET TEST] DNS diagnostic: rc=%d", rc);
    int ok = 0;
    if (rc == 0 && answer && answer->ai_family == AF_INET &&
        answer->ai_addr && answer->ai_addrlen == sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *resolved =
            (const struct sockaddr_in *)answer->ai_addr;
        const unsigned char *octet =
            (const unsigned char *)&resolved->sin_addr.s_addr;
        printf(" address=%u.%u.%u.%u", octet[0], octet[1], octet[2], octet[3]);
        struct in_addr loopback;
        ok = inet_aton("127.0.0.1", &loopback) &&
             resolved->sin_addr.s_addr == loopback.s_addr;
    }
    printf("\n");
    if (answer) freeaddrinfo(answer);
    return ok;
}

static int test_tcp(void)
{
    static const char message[] = "os01-tcp";
    char reply[32];
    size_t sent = 0;
    size_t received = 0;
    int ok = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(TCP_PORT);
    if (!inet_aton(HOST_IP, &peer.sin_addr) ||
        connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0)
        goto out;

    struct pollfd out_ready = { .fd = fd, .events = POLLOUT, .revents = 0 };
    if (poll(&out_ready, 1, 0) != 1 || !(out_ready.revents & POLLOUT))
        goto out;

    struct pollfd both = {
        .fd = fd,
        .events = POLLIN | POLLOUT,
        .revents = 0,
    };
    if (poll(&both, 1, 0) != 1 || !(both.revents & POLLOUT))
        goto out;

    while (sent < sizeof(message)) {
        int n = (int)write(fd, message + sent, sizeof(message) - sent);
        if (n <= 0 || (size_t)n > sizeof(message) - sent)
            goto out;
        sent += (size_t)n;
    }

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(fd, &rfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {0, 0};
    if (select(fd + 1, &rfds, &wfds, NULL, &tv) != 1 ||
        !FD_ISSET(fd, &wfds))
        goto out;

    while (received < sizeof(message)) {
        if (!wait_readable(fd))
            goto out;
        int n = (int)read(fd, reply + received, sizeof(message) - received);
        if (n <= 0 || (size_t)n > sizeof(message) - received)
            goto out;
        received += (size_t)n;
    }

    ok = !memcmp(reply, message, sizeof(message));
out:
    close(fd);
    return ok;
}

static int test_wget(void)
{
    const char *output = "/home/nettest.out";
    static const char payload[] = "OS01 network test\n";
    const size_t payload_len = sizeof(payload) - 1;
    unlink(output);
    int64_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s:%d/payload", HOST_IP, HTTP_PORT);
        char *argv[] = { "/bin/wget", "-q", "-O", (char *)output, url, NULL };
        exec("/bin/wget", argv, NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 0;
    int fd = open(output, O_RDONLY);
    char data[32] = {0};
    int n = fd >= 0 ? (int)read(fd, data, sizeof(data)) : -1;
    if (fd >= 0) close(fd);
    unlink(output);
    return n == (int)payload_len && !memcmp(data, payload, payload_len);
}

int main(void)
{
    printf("[NET TEST] starting\n");
    result("DHCP", wait_for_dhcp());
    result("UDP", test_udp());
    result("DNS", test_dns());
    result("TCP", test_tcp());
    result("wget", test_wget());
    printf("[NET TEST] RESULT: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

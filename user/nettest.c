// nettest.c — deterministic QEMU network regression test.
#include <sys/socket.h>
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
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 250000000 };
    for (int i = 0; i < 40; i++) {
        if (syscall(SYS_getifaddr, 0, 0, 0) > 0)
            return 1;
        nanosleep(&delay, NULL);
    }
    return 0;
}

static int wait_readable(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 5000);
    return ready > 0 && (pfd.revents & (POLLIN | POLLHUP));
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
    int rc = getaddrinfo("example.com", "80", &hints, &answer);
    int ok = rc == 0 && answer && answer->ai_addrlen == sizeof(struct sockaddr_in);
    if (answer) freeaddrinfo(answer);
    return ok;
}

static int test_tcp(void)
{
    static const char message[] = "os01-tcp";
    char reply[32];
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(TCP_PORT);
    if (!inet_aton(HOST_IP, &peer.sin_addr) ||
        connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0 ||
        write(fd, message, sizeof(message)) != (int)sizeof(message) ||
        !wait_readable(fd)) {
        close(fd);
        return 0;
    }
    int n = read(fd, reply, sizeof(reply));
    close(fd);
    return n == (int)sizeof(message) && !memcmp(reply, message, sizeof(message));
}

static int test_wget(void)
{
    const char *output = "/home/nettest.out";
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
    return n == 18 && !memcmp(data, "OS01 network test\n", 18);
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

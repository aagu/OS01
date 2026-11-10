#include "systest.h"


// ── Network syscall boundary checks ─────────────────────────
// Use the six-argument entry directly so sendto's destination and
// addrlen arrive in arguments five and six at the common dispatcher.
void test_sendto_user_boundaries(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        // No UDP stack in this boot (the systest QEMU runs without a
        // NIC, so the kernel has no netdev to bring up).  The EFAULT
        // boundary checks below only need a socket fd, not a working
        // network — skip rather than fail; the case is required=0.
        skip_case("no_udp_socket");
        return;
    }

    char payload = 'x';
    int64_t bad_pointer = syscall6(SYS_sendto, (uint64_t)fd,
                                   (uint64_t)&payload, sizeof(payload), 0,
                                   1, sizeof(struct sockaddr_in));
    CHECKF(bad_pointer == -EFAULT, "sendto invalid sockaddr pointer",
           "%lld", "%lld", (long long)bad_pointer);

    struct sockaddr_in destination = { 0 };
    destination.sin_family = AF_INET;
    int64_t short_length = syscall6(SYS_sendto, (uint64_t)fd,
                                    (uint64_t)&payload, sizeof(payload), 0,
                                    (uint64_t)&destination,
                                    sizeof(destination) - 1);
    CHECKF(short_length == -EFAULT, "sendto short sockaddr length",
           "%lld", "%lld", (long long)short_length);

    close(fd);
}


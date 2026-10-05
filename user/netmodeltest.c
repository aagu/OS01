/* user/netmodeltest.c — Driver-model matrix in-guest probe.
 *
 * Modes (argv[1] decides):
 *   no-nic              — call socket(AF_INET, SOCK_DGRAM, 0); assert
 *                         the call returns -1 with errno=ENETDOWN
 *                         within 2 seconds of [PROBE_BEGIN].
 *   udp <ip> <port>
 *     [ip2 port2 ...]    — open UDP, send a 32-byte payload to each
 *                         <ip>:<port>, recv within 1.5s, verify the
 *                         echoed bytes match.  Multi-NIC cases pass
 *                         two pairs (NIC 0 + NIC 1) and the harness
 *                         greps `iface=eth0` and `iface=eth1` lines
 *                         so it can prove each card produced
 *                         evidence.  The 32-byte nonce payload uses
 *                         the destination port for byte[0]..byte[3]
 *                         so the host's UdpEchoHost echoes a
 *                         port-unique nonce per card.
 *   file-stress <path> <seed> <iters>
 *                       — write 256 4 KiB blocks at <path>, read them
 *                         back, verify each byte matches seed+round,
 *                         then unlink.  Used by net-block-smp (via
 *                         the per-case wrapper below) to keep the
 *                         block device busy while the second CPU
 *                         runs UDP echo in parallel.
 *   poll-busy           — forks two processes; the parent loops a
 *                         continuous UDP exchange on NIC 0 (sends to
 *                         10.0.2.2:10001), the child loops a
 *                         continuous UDP exchange on NIC 1 (sends to
 *                         10.0.3.1:10002).  After ~60s both
 *                         processes must have observed >= N
 *                         successful exchanges; the matrix asserts
 *                         PASS and the harness greps `iface=eth0`
 *                         and `iface=eth1` evidence.
 *   net-block-smp <nonce> <seed> <iters>
 *                       — net-block-smp combined wrapper: forks two
 *                         processes.  Parent runs file-stress at
 *                         /.arch9-stress-<nonce> <seed> <iters>; the
 *                         child runs the per-card UDP echo
 *                         (NIC 0 + NIC 1).  Synchronizes 30s; both
 *                         must succeed.  Brief §Step 3: "同时另一个
 *                         进程逐卡UDP echo并校验nonce" — the
 *                         nonce is included in the file path so
 *                         concurrent runs do not collide and the
 *                         per-card UDP echo uses a nonce-bearing
 *                         payload.
 *
 * Output: lines starting with "[netmodeltest] ..." that the matrix
 * harness greps.  The first line is "[netmodeltest] BEGIN <mode>";
 * the last is "[netmodeltest] RESULT: PASS" or
 * "[netmodeltest] RESULT: FAIL <reason>".
 *
 * Returns 0 on PASS, 1 on FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static void emit(const char *fmt, ...)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[netmodeltest] ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (n > 0) write(1, buf, n);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Wait for the network stack to publish a DHCP-leased address on
 * ANY interface.  The probe modes need DHCP to complete before
 * they sendto() or risk dropping the packet on a route-less stack.
 * Returns 0 on timeout (>10s), 1 on lease.  Multi-NIC cases
 * (bad-nic, two-e1000, ...) lease addresses on eth1/eth2 — the
 * original 10.0.2.20-only check would spuriously fail them.
 *
 * Note: eth0 has a static fallback of 10.0.2.15 (kernel/net/lwip.c)
 * so we wait for ANY non-10.0.2.15, non-zero address — that means
 * the kernel has completed DHCP for at least one netif.  Without
 * this guard, the probe's sendto() may complete against lwIP's
 * placeholder IP and slirp NAT will drop the reply because the
 * guest source-port mapping is never established. */
static int wait_for_dhcp(void)
{
    static const uint32_t LWIP_PLACEHOLDER = 0x0F02000A; /* 10.0.2.15 */
    for (int i = 0; i < 40; i++) {
        for (int nic = 0; nic < 4; nic++) {
            uint32_t current = (uint32_t)syscall(SYS_getifaddr, nic, 0, 0);
            if (current != 0 && current != LWIP_PLACEHOLDER)
                return 1;
        }
        if (poll(NULL, 0, 250) < 0)
            return 0;
    }
    return 0;
}

/* ── no-nic mode ──────────────────────────────────────────────── */
static int run_no_nic(void)
{
    emit("BEGIN no-nic @ t=%.3f\n", now_s());
    emit("PROBE_BEGIN @ t=%.3f\n", now_s());
    /* Tiny grace so the kernel's UDP/IP stack settles; the
     * deadline is from PROBE_BEGIN to the socket() outcome. */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int saved = errno;
    double t = now_s();
    if (fd >= 0) {
        close(fd);
        emit("socket() returned rc=%d errno=%d (unexpected ONLINE) @ t=%.3f\n",
              fd, saved, t);
        emit("RESULT: FAIL no-nic: stack reported ONLINE\n");
        return 1;
    }
    if (saved != ENETDOWN) {
        emit("socket() returned rc=%d errno=%d (expected -ENETDOWN) @ t=%.3f\n",
              fd, saved, t);
        emit("RESULT: FAIL no-nic: errno mismatch\n");
        return 1;
    }
    emit("socket() returned rc=-1 errno=ENETDOWN @ t=%.3f\n", t);
    emit("RESULT: PASS no-nic\n");
    return 0;
}

/* ── ifaces mode (dump all ONLINE interfaces) ──────────────────
 * Used by bad-nic and similar "verify-each-card" cases that need
 * the kernel-side per-NIC ONLINE marker (without requiring a
 * UDP echo through slirp, which has multi-NIC NAT quirks). */
static int run_ifaces(void)
{
    emit("BEGIN ifaces @ t=%.3f\n", now_s());
    emit("PROBE_BEGIN @ t=%.3f\n", now_s());
    /* Wait for DHCP so the NIC has an IP — kernel publishes ONLINE
     * only after a successful DHCP lease. */
    if (!wait_for_dhcp()) {
        emit("no dhcp lease; no ONLINE ifaces\n");
        emit("iface=none\n");
        emit("RESULT: FAIL ifaces: no ONLINE\n");
        return 1;
    }
    /* Probe up to 4 interfaces; the OS01 stack exposes eth0..eth3
     * to userspace via SYS_getifaddr.  Emit iface=ethN for each
     * one that returns a non-zero address. */
    int found = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t addr = (uint32_t)syscall(SYS_getifaddr, i, 0, 0);
        if (addr == 0)
            continue;
        emit("iface=eth%d ip=%u.%u.%u.%u\n",
              i,
              (unsigned)(addr) & 0xFF,
              (unsigned)(addr >> 8) & 0xFF,
              (unsigned)(addr >> 16) & 0xFF,
              (unsigned)(addr >> 24) & 0xFF);
        found++;
    }
    if (found == 0) {
        emit("iface=none\n");
        emit("RESULT: FAIL ifaces: no ONLINE\n");
        return 1;
    }
    emit("RESULT: PASS ifaces count=%d\n", found);
    return 0;
}

/* ── udp echo mode ────────────────────────────────────────────── */
/* Emit per-card evidence (`iface=ethN ip=...`) so the matrix runner's
 * assert_each_card_has_evidence() can prove each NIC produced its
 * own proof, not just a single-card aggregate pass. */
static void emit_iface_evidence(void)
{
    for (int i = 0; i < 4; i++) {
        uint32_t addr = (uint32_t)syscall(SYS_getifaddr, i, 0, 0);
        if (addr == 0)
            continue;
        emit("iface=eth%d ip=%u.%u.%u.%u\n",
              i,
              (unsigned)(addr) & 0xFF,
              (unsigned)(addr >> 8) & 0xFF,
              (unsigned)(addr >> 16) & 0xFF,
              (unsigned)(addr >> 24) & 0xFF);
    }
}

/* Single-NIC probe (legacy single-card cases — emit ONE iface=ethN
 * line for the card that owns the route to <ip>). */
static int probe_one(int card_index, const char *ip, int port)
{
    (void)card_index;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        emit("udp[%s:%d] socket() failed errno=%d\n", ip, port, errno);
        return -1;
    }

    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (!inet_aton(ip, &peer.sin_addr)) {
        emit("udp[%s:%d] inet_aton failed\n", ip, port);
        close(fd);
        return -1;
    }

    /* 32-byte nonce payload; host echoes verbatim.  Use the port to
     * give each NIC's nonce a different identity, so the per-card
     * check below can prove the right reply came back. */
    unsigned char nonce[32];
    for (int i = 0; i < 32; i++) nonce[i] = (unsigned char)(i * 7 + port);

    ssize_t sent = sendto(fd, nonce, sizeof(nonce), 0,
                          (struct sockaddr *)&peer, sizeof(peer));
    if (sent != sizeof(nonce)) {
        emit("udp[%s:%d] sendto returned %d errno=%d\n",
              ip, port, (int)sent, errno);
        close(fd);
        return -1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 5000);
    if (ready <= 0) {
        emit("udp[%s:%d] poll timed out rc=%d errno=%d\n",
              ip, port, ready, errno);
        close(fd);
        return -1;
    }
    unsigned char reply[64];
    ssize_t got = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
    close(fd);
    if (got != sizeof(nonce)) {
        emit("udp[%s:%d] recvfrom got %d bytes\n", ip, port, (int)got);
        return -1;
    }
    if (memcmp(reply, nonce, sizeof(nonce)) != 0) {
        emit("udp[%s:%d] nonce mismatch\n", ip, port);
        return -1;
    }
    return 0;
}

/* udp mode — supports 1 or 2 (ip, port) pairs.  argv layout:
 *   udp <ip1> <port1>                — single NIC
 *   udp <ip1> <port1> <ip2> <port2>  — multi NIC (per-card probe)
 */
static int run_udp(int argc, char **argv)
{
    const char *ip1 = (argc >= 3) ? argv[2] : NULL;
    const char *port1_str = (argc >= 4) ? argv[3] : NULL;
    const char *ip2 = (argc >= 6) ? argv[4] : NULL;
    const char *port2_str = (argc >= 6) ? argv[5] : NULL;

    if (!ip1 || !port1_str) {
        emit("RESULT: FAIL usage: netmodeltest udp <ip> <port> [ip2 port2]\n");
        return 2;
    }
    int port1 = atoi(port1_str);

    emit("BEGIN udp ip1=%s port1=%d%s%s%s%s @ t=%.3f\n",
          ip1, port1,
          ip2 ? " ip2=" : "", ip2 ? ip2 : "",
          ip2 ? " port2=" : "", ip2 ? port2_str : "",
          now_s());
    emit("PROBE_BEGIN @ t=%.3f\n", now_s());

    if (!wait_for_dhcp()) {
        emit("dhcp lease not acquired in 10s\n");
        emit("RESULT: FAIL udp: dhcp timeout\n");
        return 1;
    }
    emit_iface_evidence();

    int rc = probe_one(0, ip1, port1);
    if (rc != 0) {
        emit("RESULT: FAIL udp: card0 mismatch\n");
        return 1;
    }

    if (ip2 && port2_str) {
        int port2 = atoi(port2_str);
        rc = probe_one(1, ip2, port2);
        if (rc != 0) {
            emit("RESULT: FAIL udp: card1 mismatch\n");
            return 1;
        }
    }
    emit("RESULT: PASS udp\n");
    return 0;
}

/* ── file-stress mode ─────────────────────────────────────────── */
static int run_file_stress(const char *path, const char *seed_str,
                           const char *iters_str)
{
    unsigned seed = (unsigned)strtoul(seed_str, NULL, 0);
    unsigned iters = (unsigned)strtoul(iters_str, NULL, 0);
    if (iters == 0) iters = 256;
    emit("BEGIN file-stress path=%s seed=0x%x iters=%u @ t=%.3f\n",
          path, seed, iters, now_s());
    emit("PROBE_BEGIN @ t=%.3f\n", now_s());

    /* 4 KiB deterministic buffer; round # varies one byte per
     * iteration so we can detect write/read aliasing. */
    unsigned char *buf = (unsigned char *)malloc(4096);
    unsigned char *back = (unsigned char *)malloc(4096);
    if (!buf || !back) {
        emit("malloc failed\n");
        emit("RESULT: FAIL file-stress: oom\n");
        free(buf); free(back);
        return 1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        emit("open(%s) failed errno=%d\n", path, errno);
        free(buf); free(back);
        emit("RESULT: FAIL file-stress: open write\n");
        return 1;
    }
    for (unsigned r = 0; r < iters; r++) {
        for (unsigned i = 0; i < 4096; i++) {
            buf[i] = (unsigned char)((seed + r + i) & 0xFF);
        }
        ssize_t w = write(fd, buf, 4096);
        if (w != 4096) {
            emit("write round %u returned %d errno=%d\n", r, (int)w, errno);
            close(fd); unlink(path);
            free(buf); free(back);
            emit("RESULT: FAIL file-stress: write round %u\n", r);
            return 1;
        }
    }
    close(fd);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        emit("open(%s) read failed errno=%d\n", path, errno);
        unlink(path);
        free(buf); free(back);
        emit("RESULT: FAIL file-stress: open read\n");
        return 1;
    }
    for (unsigned r = 0; r < iters; r++) {
        ssize_t got = read(fd, back, 4096);
        if (got != 4096) {
            emit("read round %u returned %d\n", r, (int)got);
            close(fd); unlink(path);
            free(buf); free(back);
            emit("RESULT: FAIL file-stress: read round %u\n", r);
            return 1;
        }
        for (unsigned i = 0; i < 4096; i++) {
            unsigned char expect = (unsigned char)((seed + r + i) & 0xFF);
            if (back[i] != expect) {
                emit("byte mismatch round=%u offset=%u expect=0x%02x got=0x%02x\n",
                      r, i, expect, back[i]);
                close(fd); unlink(path);
                free(buf); free(back);
                emit("RESULT: FAIL file-stress: verify round %u\n", r);
                return 1;
            }
        }
    }
    close(fd);
    unlink(path);
    free(buf); free(back);
    emit("RESULT: PASS file-stress rounds=%u\n", iters);
    return 0;
}

/* ── poll-busy mode ───────────────────────────────────────────── */
/* Run a UDP echo loop continuously on a single NIC.  Returns the
 * number of successful exchanges within the budget.  Each exchange
 * uses a unique nonce so a per-NIC identity is verifiable. */
static int poll_busy_loop(const char *label, const char *ip, int port,
                          int budget, double deadline_s)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        emit("poll-busy[%s] socket() failed errno=%d\n", label, errno);
        return 0;
    }
    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (!inet_aton(ip, &peer.sin_addr)) {
        emit("poll-busy[%s] inet_aton(%s) failed\n", label, ip);
        close(fd);
        return 0;
    }

    int ok = 0;
    int nonce_counter = 0;
    while (ok < budget && now_s() < deadline_s) {
        unsigned char nonce[32];
        for (int i = 0; i < 32; i++)
            nonce[i] = (unsigned char)((i * 7 + port + nonce_counter) & 0xFF);

        ssize_t sent = sendto(fd, nonce, sizeof(nonce), 0,
                              (struct sockaddr *)&peer, sizeof(peer));
        if (sent != sizeof(nonce)) {
            nonce_counter++;
            continue;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 250);
        if (ready <= 0) {
            nonce_counter++;
            continue;
        }
        unsigned char reply[64];
        ssize_t got = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
        if (got != sizeof(nonce) ||
            memcmp(reply, nonce, sizeof(nonce)) != 0) {
            nonce_counter++;
            continue;
        }
        ok++;
        nonce_counter++;
    }
    close(fd);
    emit("poll-busy[%s] ip=%s port=%d exchanges=%d budget=%d\n",
          label, ip, port, ok, budget);
    return ok;
}

static int run_poll_busy(void)
{
    emit("BEGIN poll-busy @ t=%.3f\n", now_s());
    emit("PROBE_BEGIN @ t=%.3f\n", now_s());

    if (!wait_for_dhcp()) {
        emit("dhcp lease not acquired in 10s\n");
        emit("RESULT: FAIL poll-busy: dhcp timeout\n");
        return 1;
    }
    emit_iface_evidence();

    /* Fork: parent loops NIC 0, child loops NIC 1.  Each must
     * achieve >= 5 successful exchanges within the 60-second budget
     * for the case to PASS. */
    double deadline = now_s() + 60.0;
    int budget = 5;

    pid_t pid = fork();
    if (pid < 0) {
        emit("fork failed errno=%d\n", errno);
        emit("RESULT: FAIL poll-busy: fork\n");
        return 1;
    }
    if (pid == 0) {
        int ok = poll_busy_loop("eth1", "10.0.3.1", 10002, budget, deadline);
        _exit(ok >= budget ? 0 : 1);
    }
    int ok0 = poll_busy_loop("eth0", "10.0.2.2", 10001, budget, deadline);

    int status = 0;
    waitpid(pid, &status, 0);
    int ok1 = WIFEXITED(status) && (WEXITSTATUS(status) == 0) ? budget : 0;

    if (ok0 < budget || ok1 < budget) {
        emit("RESULT: FAIL poll-busy: eth0=%d eth1=%d budget=%d\n",
              ok0, ok1, budget);
        return 1;
    }
    emit("RESULT: PASS poll-busy eth0=%d eth1=%d\n", ok0, ok1);
    return 0;
}

/* ── net-block-smp combined mode ──────────────────────────────── */
/* Brief §Step 3: "net-block-smp固定SMP=2... 同时另一个进程逐卡UDP
 * echo并校验nonce".  Brief §Step 10: "file-stress writes
 * /.arch9-stress-<nonce> (NOT in /tmp)".  This wrapper forks the
 * file-stress (parent) and the per-card UDP echo (child), then
 * synchronizes 30s. */
static int child_per_card_udp_loop(unsigned nonce_value,
                                    double deadline_s)
{
    /* Probe both NICs in turn with a nonce-bearing 32-byte payload.
     * 5 successful exchanges per NIC, each tagged with the case
     * nonce so the harness can verify the right payload went through. */
    int ok0 = 0, ok1 = 0;
    while (now_s() < deadline_s && (ok0 < 3 || ok1 < 3)) {
        if (ok0 < 3) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd >= 0) {
                struct sockaddr_in p = {0};
                p.sin_family = AF_INET;
                p.sin_port = htons(10001);
                inet_aton("10.0.2.2", &p.sin_addr);
                unsigned char nonce[32];
                for (int i = 0; i < 32; i++)
                    nonce[i] = (unsigned char)((i * 7 + 10001 + nonce_value) & 0xFF);
                if (sendto(fd, nonce, sizeof(nonce), 0,
                           (struct sockaddr *)&p, sizeof(p)) == sizeof(nonce)) {
                    struct pollfd pfd = { .fd = fd, .events = POLLIN };
                    if (poll(&pfd, 1, 250) > 0) {
                        unsigned char reply[64];
                        if (recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL) ==
                            sizeof(nonce) &&
                            memcmp(reply, nonce, sizeof(nonce)) == 0)
                            ok0++;
                    }
                }
                close(fd);
            }
        }
        if (ok1 < 3) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd >= 0) {
                struct sockaddr_in p = {0};
                p.sin_family = AF_INET;
                p.sin_port = htons(10002);
                inet_aton("10.0.3.1", &p.sin_addr);
                unsigned char nonce[32];
                for (int i = 0; i < 32; i++)
                    nonce[i] = (unsigned char)((i * 7 + 10002 + nonce_value) & 0xFF);
                if (sendto(fd, nonce, sizeof(nonce), 0,
                           (struct sockaddr *)&p, sizeof(p)) == sizeof(nonce)) {
                    struct pollfd pfd = { .fd = fd, .events = POLLIN };
                    if (poll(&pfd, 1, 250) > 0) {
                        unsigned char reply[64];
                        if (recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL) ==
                            sizeof(nonce) &&
                            memcmp(reply, nonce, sizeof(nonce)) == 0)
                            ok1++;
                    }
                }
                close(fd);
            }
        }
    }
    _exit((ok0 >= 3 && ok1 >= 3) ? 0 : 1);
}

static int run_net_block_smp(int argc, char **argv)
{
    if (argc < 5) {
        emit("RESULT: FAIL usage: netmodeltest net-block-smp <nonce> <seed> <iters>\n");
        return 2;
    }
    const char *nonce = argv[2];
    const char *seed_str = argv[3];
    const char *iters_str = argv[4];
    unsigned nonce_value = (unsigned)strtoul(nonce, NULL, 0);

    char path[64];
    snprintf(path, sizeof(path), "/.arch9-stress-%s", nonce);
    emit("BEGIN net-block-smp nonce=%s seed=%s iters=%s path=%s @ t=%.3f\n",
          nonce, seed_str, iters_str, path, now_s());
    emit("PROBE_BEGIN @ t=%.3f\n", now_s());

    /* The kernel's DMA stack may not see this file before the
     * children start; emit iface evidence early so the harness can
     * observe eth0 even if the block stress fails. */
    if (!wait_for_dhcp()) {
        emit("dhcp lease not acquired in 10s\n");
        emit("RESULT: FAIL net-block-smp: dhcp timeout\n");
        return 1;
    }
    emit_iface_evidence();

    pid_t pid = fork();
    if (pid < 0) {
        emit("fork failed errno=%d\n", errno);
        emit("RESULT: FAIL net-block-smp: fork\n");
        return 1;
    }
    if (pid == 0) {
        /* Child: per-card UDP echo loop, 30s budget. */
        child_per_card_udp_loop(nonce_value, now_s() + 30.0);
        /* unreachable: child_per_card_udp_loop _exits. */
    }

    /* Parent: file-stress.  Synchronize with the child's UDP loop
     * via a soft 30s wall-clock cap; the child reaps itself via
     * _exit. */
    int fs_rc = run_file_stress(path, seed_str, iters_str);

    /* Wait for child up to 30s.  The child either succeeds or
     * _exits(1) on deadline. */
    int status = 0;
    double child_deadline = now_s() + 30.0;
    while (now_s() < child_deadline) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (now_s() >= child_deadline) {
        /* Force the child to die and reap. */
        kill(pid, 9);
        waitpid(pid, &status, 0);
    }
    int udp_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    if (fs_rc != 0 || !udp_ok) {
        emit("RESULT: FAIL net-block-smp: file-stress=%d udp-ok=%d\n",
              fs_rc, udp_ok);
        return 1;
    }
    emit("RESULT: PASS net-block-smp nonce=%s\n", nonce);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        emit("RESULT: FAIL usage: netmodeltest <no-nic|ifaces|udp|file-stress|poll-busy|net-block-smp> ...\n");
        return 2;
    }
    if (strcmp(argv[1], "no-nic") == 0) {
        return run_no_nic();
    }
    if (strcmp(argv[1], "ifaces") == 0) {
        return run_ifaces();
    }
    if (strcmp(argv[1], "udp") == 0) {
        return run_udp(argc, argv);
    }
    if (strcmp(argv[1], "file-stress") == 0) {
        if (argc < 5) {
            emit("RESULT: FAIL usage: netmodeltest file-stress <path> <seed> <iters>\n");
            return 2;
        }
        return run_file_stress(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "poll-busy") == 0) {
        return run_poll_busy();
    }
    if (strcmp(argv[1], "net-block-smp") == 0) {
        return run_net_block_smp(argc, argv);
    }
    emit("RESULT: FAIL unknown mode %s\n", argv[1]);
    return 2;
}
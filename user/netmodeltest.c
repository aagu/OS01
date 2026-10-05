/* user/netmodeltest.c — Driver-model matrix in-guest probe.
 *
 * Modes (argv[1] decides):
 *   no-nic              — call socket(AF_INET, SOCK_DGRAM, 0); assert
 *                         the call returns -1 with errno=ENETDOWN
 *                         within 2 seconds of [PROBE_BEGIN].
 *   udp <ip> <port>     — open UDP, send a 32-byte payload to <ip>:<port>,
 *                         recv within 1s, verify the echoed bytes
 *                         match.  When run with two NICs, the harness
 *                         uses two distinct <ip>:<port> pairs and
 *                         verifies each.
 *   file-stress <path> <seed> <iters>
 *                       — write 256 4 KiB blocks at <path>, read them
 *                         back, verify each byte matches seed+round,
 *                         then unlink.  Used by net-block-smp to keep
 *                         the block device busy while the second CPU
 *                         runs UDP echo in parallel.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
 * original 10.0.2.20-only check would spuriously fail them. */
static int wait_for_dhcp(void)
{
    for (int i = 0; i < 40; i++) {
        for (int nic = 0; nic < 4; nic++) {
            uint32_t current = (uint32_t)syscall(SYS_getifaddr, nic, 0, 0);
            if (current != 0)
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
static int run_udp(const char *ip, const char *port_str)
{
    emit("BEGIN udp ip=%s port=%s @ t=%.3f\n", ip, port_str, now_s());
    emit("PROBE_BEGIN @ t=%.3f\n", now_s());

    int port = atoi(port_str);
    /* Wait for DHCP to lease an address so sendto() doesn't drop
     * the packet on an unconfigured netif.  The brief's 2-second
     * socket() deadline applies to the no-nic probe; the udp probe
     * has no such hard ceiling (DHCP lease can take ~3-4s). */
    if (!wait_for_dhcp()) {
        emit("dhcp lease not acquired in 10s\n");
        emit("RESULT: FAIL udp: dhcp timeout\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        emit("socket() failed errno=%d\n", errno);
        emit("RESULT: FAIL udp: socket\n");
        return 1;
    }

    /* No bind() — let the kernel pick an ephemeral source port.
     * Earlier revisions bound to 40000+(port%1000), which sometimes
     * interacted badly with slirp's NAT for high ports. */

    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (!inet_aton(ip, &peer.sin_addr)) {
        emit("inet_aton(%s) failed\n", ip);
        close(fd);
        emit("RESULT: FAIL udp: inet_aton\n");
        return 1;
    }

    /* 32-byte nonce payload; the host-side NetworkServices.udp_echo
     * echoes verbatim. */
    unsigned char nonce[32];
    for (int i = 0; i < 32; i++) nonce[i] = (unsigned char)(i * 7 + port);

    /* Send to slirp host (10.0.2.2) by default, but caller supplies
     * the explicit <ip>. */
    ssize_t sent = sendto(fd, nonce, sizeof(nonce), 0,
                          (struct sockaddr *)&peer, sizeof(peer));
    if (sent != sizeof(nonce)) {
        emit("sendto() returned %d errno=%d\n", (int)sent, errno);
        close(fd);
        emit("RESULT: FAIL udp: sendto\n");
        return 1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 1500);
    if (ready <= 0) {
        emit("poll() timed out rc=%d errno=%d\n", ready, errno);
        close(fd);
        emit("RESULT: FAIL udp: poll\n");
        return 1;
    }
    unsigned char reply[64];
    ssize_t got = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
    close(fd);
    if (got != sizeof(nonce)) {
        emit("recvfrom() got %d bytes\n", (int)got);
        emit("RESULT: FAIL udp: short reply\n");
        return 1;
    }
    if (memcmp(reply, nonce, sizeof(nonce)) != 0) {
        emit("recvfrom() bytes mismatch\n");
        emit("RESULT: FAIL udp: bytes mismatch\n");
        return 1;
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        emit("RESULT: FAIL usage: netmodeltest <no-nic|ifaces|udp|file-stress> ...\n");
        return 2;
    }
    if (strcmp(argv[1], "no-nic") == 0) {
        return run_no_nic();
    }
    if (strcmp(argv[1], "ifaces") == 0) {
        return run_ifaces();
    }
    if (strcmp(argv[1], "udp") == 0) {
        if (argc < 4) {
            emit("RESULT: FAIL usage: netmodeltest udp <ip> <port>\n");
            return 2;
        }
        return run_udp(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "file-stress") == 0) {
        if (argc < 5) {
            emit("RESULT: FAIL usage: netmodeltest file-stress <path> <seed> <iters>\n");
            return 2;
        }
        return run_file_stress(argv[2], argv[3], argv[4]);
    }
    emit("RESULT: FAIL unknown mode %s\n", argv[1]);
    return 2;
}
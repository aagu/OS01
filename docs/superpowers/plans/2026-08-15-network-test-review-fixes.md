# Network Test Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the QEMU DNS regression independent of external DNS and remove the wget payload-length magic number without changing its correct 18-byte behavior.

**Architecture:** Keep the existing guest-to-QEMU DNS proxy path and replace only the queried hostname with `localhost`, then validate the returned address is IPv4 loopback. Keep the payload literal on both sides of the Python/C boundary, but derive its byte length independently with `len(payload)` in Python and `sizeof(payload) - 1` in C.

**Tech Stack:** Freestanding C guest program, OS01 libc sockets/getaddrinfo, Python 3 QEMU test harness, QEMU user-mode networking.

## Global Constraints

- DNS must still traverse `getaddrinfo()`'s UDP query path to `10.0.2.3:53`.
- DNS must not require Internet connectivity, but the host resolver used by
  slirp must answer `localhost` as `127.0.0.1`; an NXDOMAIN response is a host
  configuration failure and must be diagnosed explicitly.
- DNS success must return exactly IPv4 `127.0.0.1`, not merely a correctly sized sockaddr.
- The wget response body remains exactly `OS01 network test\n` (18 bytes).
- C must exclude the terminating NUL with `sizeof(payload) - 1`.
- `user/socktest.c` remains unchanged because it is outside `make test-network`.

---

### Task 0: Commit the working network-test harness baseline

**Files:**
- Modify: `Makefile`
- Modify: `tests/run_test.py`
- Create: `config/inittab.nettest`
- Create: `user/nettest.c`

**Interfaces:**
- Produces: `make test-network`, which builds the `OS01_NETTEST` disk image,
  starts host TCP/UDP/HTTP endpoints, boots QEMU with E1000, and requires five
  guest network checks to pass.

- [ ] **Step 1: Verify the complete uncommitted harness**

Run:

```bash
make test-network
```

Expected: exit 0 with `[NET TEST] RESULT: 5 passed, 0 failed`.
This baseline still queries `example.com`, so it requires working external DNS;
a DNS failure here should be recorded as the known dependency Task 1 removes.

- [ ] **Step 2: Commit the harness as the buildable base**

```bash
git add Makefile tests/run_test.py config/inittab.nettest user/nettest.c
git commit -m "test: add automated QEMU network regression"
```

Expected: the resulting commit independently provides a working
`make test-network` target. Do not include the pre-existing dirty BusyBox or
posix-uefi submodules.

### Task 1: Make DNS coverage local and exact

**Files:**
- Modify: `user/nettest.c:68-79`
- Test: `tests/run_test.py` via the existing `network` integration test

**Interfaces:**
- Consumes: `getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **)` and the QEMU DNS proxy at `10.0.2.3:53`.
- Produces: `test_dns()` returns true only when `localhost` resolves to IPv4 `127.0.0.1`.

- [ ] **Step 1: Record the current integration baseline**

Run:

```bash
python3 tests/run_test.py network
```

Expected: exit 0 with `[NET TEST] DNS: PASS`; this characterizes the current path but does not prove offline determinism.

- [ ] **Step 2: Create and observe the failing address assertion**

Temporarily change the DNS query name to `localhost`, print the
`getaddrinfo()` return code and, on success, the returned IPv4 address, then add
a temporary assertion that expects the wrong literal address `127.0.0.2`.
For example, immediately after `getaddrinfo()` add:

```c
printf("[NET TEST] DNS diagnostic: rc=%d", rc);
if (rc == 0 && answer && answer->ai_family == AF_INET && answer->ai_addr) {
    const struct sockaddr_in *resolved =
        (const struct sockaddr_in *)answer->ai_addr;
    const unsigned char *octet = (const unsigned char *)&resolved->sin_addr.s_addr;
    printf(" address=%u.%u.%u.%u", octet[0], octet[1], octet[2], octet[3]);
}
printf("\n");
struct in_addr deliberately_wrong;
int wrong_address_match =
    inet_aton("127.0.0.2", &deliberately_wrong) && rc == 0 && answer &&
    answer->ai_family == AF_INET && answer->ai_addr &&
    ((const struct sockaddr_in *)answer->ai_addr)->sin_addr.s_addr ==
        deliberately_wrong.s_addr;
if (answer) freeaddrinfo(answer);
return wrong_address_match;
```

Rebuild and run:

```bash
make -C user ../build/x86_64/user/nettest.elf
make test-network
```

Expected on a supported host: the diagnostic reports `rc=0` and
`address=127.0.0.1`, followed by `[NET TEST] DNS: FAIL`; this proves the wrong
address assertion observes the real answer. If the diagnostic reports nonzero
`rc` (for example NXDOMAIN), stop and report that the host resolver does not
satisfy the plan's prerequisite—the failure does not validate the assertion.
Remove only the deliberately wrong assertion before the next step; retain the
diagnostic for actionable CI failures.

- [ ] **Step 3: Implement the exact loopback assertion**

Replace `test_dns()` with this behavior:

```c
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
```

- [ ] **Step 4: Build and verify the DNS change**

Run:

```bash
make -C user ../build/x86_64/user/nettest.elf
make test-network
```

Expected: exit 0 with `[NET TEST] DNS: PASS` and `[NET TEST] RESULT: 5 passed, 0 failed`.

- [ ] **Step 5: Commit the DNS change**

```bash
git add user/nettest.c
git commit -m "test: make QEMU DNS regression local"
```

### Task 2: Derive the wget payload length

**Files:**
- Modify: `user/nettest.c:105-123`
- Modify: `docs/roadmap.md:29`
- Verify: `tests/run_test.py:230-243` already derives host length with `len(payload)`

**Interfaces:**
- Consumes: HTTP response body `b"OS01 network test\n"` served by `_PayloadHTTPHandler`.
- Produces: `test_wget()` compares exactly 18 downloaded bytes without a numeric length literal.

- [ ] **Step 1: Confirm the host already derives its length**

Read `_PayloadHTTPHandler.do_GET()` and confirm it uses:

```python
payload = b"OS01 network test\n"
self.send_header("Content-Length", str(len(payload)))
```

No Python change is required.

- [ ] **Step 2: Prove the guest comparison catches a length mutation**

Temporarily change the guest expected length from `18` to `17`, rebuild, and run:

```bash
make -C user ../build/x86_64/user/nettest.elf
make test-network
```

Expected: QEMU reports `[NET TEST] wget: FAIL`. Restore the numeric value before implementing the refactor.

- [ ] **Step 3: Replace the guest magic number**

Inside the block scope of `test_wget()`, define the expected payload once and
derive the non-NUL byte count:

```c
static const char payload[] = "OS01 network test\n";
const size_t payload_len = sizeof(payload) - 1;
```

Replace the return expression with:

```c
return n == (int)payload_len && !memcmp(data, payload, payload_len);
```

- [ ] **Step 4: Run complete verification**

After the full test passes, retain the existing `docs/roadmap.md` change that
marks the deterministic network regression complete.

Run:

```bash
make test-network
git diff --check
```

Expected: `make test-network` exits 0 with all five network cases passing; `git diff --check` exits 0 with no output.

- [ ] **Step 5: Commit the payload refactor**

```bash
git add user/nettest.c docs/roadmap.md
git commit -m "test: finalize deterministic QEMU network regression"
```

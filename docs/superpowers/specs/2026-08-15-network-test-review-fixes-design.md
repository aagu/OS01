# Network Test Review Fixes

## Scope

Address the review feedback for the QEMU network regression without weakening
coverage or adding external infrastructure.

## Design

The DNS case will resolve `localhost` instead of `example.com`. The guest still
sends a DNS packet to QEMU's DNS proxy at `10.0.2.3:53`, so the test continues
to cover UDP transport, DNS request/response handling, and `getaddrinfo()` result
parsing. The test does not require Internet connectivity, but it does require
the host resolver used by slirp to answer `localhost` as `127.0.0.1` (as
systemd-resolved and dnsmasq normally do). Hosts that forward `localhost` to an
upstream resolver may return NXDOMAIN; the test must print the `getaddrinfo()`
return code and any returned IPv4 address so that this environment failure is
distinguishable from an address-validation failure. The assertion will
require a successful IPv4 result whose `sin_addr.s_addr` is exactly the IPv4
loopback address `127.0.0.1`; checking `ai_addrlen` alone is not sufficient.

The wget response body remains `OS01 network test\n`. Its actual length is 18
bytes, so the existing numeric check is correct, but the host and guest will each
derive the length from the payload definition instead of duplicating a magic
number. The C side will define a payload array and use `sizeof(payload) - 1` so
the terminating NUL is excluded; the Python side will continue to use
`len(payload)`. The payload content remains duplicated across the Python/C
language boundary by design, while each side derives its own byte count. This
prevents future payload edits from producing an off-by-one review or regression.

The standalone `user/socktest.c` remains unchanged because it is a manual
diagnostic and is not part of `make test-network`.

The network-test harness changes in `Makefile`, `tests/run_test.py`,
`config/inittab.nettest`, and `user/nettest.c` must be committed as a working
baseline before the review fixes are split into follow-up commits.

## Verification

1. Demonstrate the old DNS case depends on `example.com` and the old wget check
   embeds a numeric length.
2. Build the guest `nettest` program.
3. Run `make test-network` and require exactly five passes and zero failures.
4. Run `git diff --check`.

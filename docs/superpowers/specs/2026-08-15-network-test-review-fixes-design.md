# Network Test Review Fixes

## Scope

Address the review feedback for the QEMU network regression without weakening
coverage or adding external infrastructure.

## Design

The DNS case will resolve `localhost` instead of `example.com`. The guest still
sends a DNS packet to QEMU's DNS proxy at `10.0.2.3:53`, so the test continues
to cover UDP transport, DNS request/response handling, and `getaddrinfo()` result
parsing. The host resolver can answer `localhost` locally, so the test does not
depend on Internet connectivity or an upstream DNS server. The assertion will
require a successful IPv4 result.

The wget response body remains `OS01 network test\n`. Its actual length is 18
bytes, so the existing numeric check is correct, but the host and guest will each
derive the length from the payload definition instead of duplicating a magic
number. This prevents future payload edits from producing an off-by-one review
or regression.

## Verification

1. Demonstrate the old DNS case depends on `example.com` and the old wget check
   embeds a numeric length.
2. Build the guest `nettest` program.
3. Run `make test-network` and require exactly five passes and zero failures.
4. Run `git diff --check`.

# Task 6 report — PS/2 packet parser

Status: DONE

## Changes

- Added pure C `mouse_proto_reset` / `mouse_proto_feed` state machine in `kernel/driver/mouse_proto.c` with symmetric public header under `kernel/include/driver/`.
- The parser accepts a packet header only when bit 3 is set, then consumes all data bytes regardless of bit 3. It returns -1 for a discarded header, 0 while collecting, and 1 for a complete event.
- Decodes 9-bit X/Y signed displacement, inverts Y in `int16_t`, zeroes only an overflowing axis, sign extends the 4-bit wheel, masks buttons to three bits, and clears the reserved ABI field.
- Registered `test_mouse_proto` in the host suite. The case checks 3/4-byte packets, buttons, wheel +1/-1/-8, X/Y signed boundaries, Y inversion, both overflow axes, invalid headers, data byte bit 3, reset of a partial packet, and reserved zero.

## TDD evidence

- RED: compiled `hosttests/cases/test_mouse_proto.c` without implementation; linker exited 1 with undefined references to `mouse_proto_reset` and `mouse_proto_feed`.
- GREEN: `clang -g -O0 -Wall -Wextra -Werror -Ikernel/include -Ihosttests/include hosttests/cases/test_mouse_proto.c kernel/driver/mouse_proto.c -o /tmp/test_mouse_proto_green && /tmp/test_mouse_proto_green`: 52 assertions passed, 0 failed.
- An intermediate green attempt caught an incorrect X-sign bit in a test fixture (51/52); corrected the fixture and reran.

## Verification and self-review

- `make test-host`: exit 0; 30 suites, 0 failed; `test_mouse_proto` 52/52; PMM boot reservation cases passed.
- `git diff --check`: clean.
- Reviewed the state transitions, arithmetic boundaries, fixed eight-byte ABI, and source/header layout. No remaining concerns within Task 6 scope. Hardware ACK/BAT/ID filtering belongs to the upstream command path, as specified.

## Commit

See the commit containing this report (`Add pure PS/2 mouse packet parser and host tests`).

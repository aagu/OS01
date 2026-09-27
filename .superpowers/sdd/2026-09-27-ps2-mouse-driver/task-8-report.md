# Task 8 report — event ring and devfs read/poll

Commit: `worktree-ps2-mouse` HEAD (hash in `git log -1 --format=%h`; the report is part of that commit).

## Implementation

- Added a 64-event shared ring with one IRQ-safe lock for producer, readers, and poll registration. Full rings discard the oldest complete event and increment a drop counter.
- Read returns `-EINVAL` for buffers smaller than eight bytes and `-EAGAIN` for an empty queue. Reads copy only complete events to the kernel-supplied buffer while locked; `fd_read` performs the user copy and preserves negative errno from Task 2.
- Poll checks emptiness and registers under the same lock. Producer removes and wakes every registered waiter while holding that lock, matching `poll_table_cleanup` lifetime handling. `this_cpu()->need_resched` is gated by `num_cpus != 0` as in Task 5.
- `/dev/mouse` is registered after devfs initialization only when `subsys_status("mouse") == 1`; a failed registration is logged.

## TDD evidence

- RED: `make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/.claude/worktrees/ps2-mouse/mk/profiles/x86_64-clang.mk test_mouse_init` failed to compile with undeclared `mouse_devfs_read` and `mouse_poll_dev`, after the production-object test was added.
- GREEN: the same focused command produced `Total: 377 | Passed: 377 | Failed: 0` after implementation. The host fixture links the real `kernel/driver/mouse.c`; its scriptable parser emits events at packet boundaries. Existing `test_mouse_proto` covers the real parser separately.
- `make PROFILE=x86_64-clang test-host`: 31 suites, 0 failed; PMM reservation cases passed.
- `make PROFILE=x86_64-clang kernel.bin`: exit 0.
- `make PROFILE=x86_64-clang disk.img`: exit 0, GPT/ext2/FAT32 self-checks OK.
- `git diff --check`: exit 0.

Tests cover empty and short reads, complete-event sizing, shared consumption, 66 events through the 64-entry ring with two drops, ten poll waiters, wake under the event lock, and the pre-GS reschedule gate. Task 2's `fd_read` negative errno tests remain in the full host suite.

## Bounded QEMU observation

Used the profile's `print-run-paths` firmware/image with q35, AHCI, 512 MiB, SMP 2, `-serial stdio`, and `timeout 42`; injected `ls /dev/mouse` at 22 seconds. Serial output included `subsys: init  mouse ... ok` and `DHCP_ACK received`. QEMU was terminated by the timeout (`124`) before a shell prompt or command response. This matches the pre-existing local OVMF stall after DHCP noted in the Task 5 baseline. Userspace node presence remains unverified by this run; the conditional registration path compiled and the mouse subsystem succeeded.

## Self-review

No fixed waiter cap or unlocked saved waiter pointers. Poll entry removal and wake share the event lock with `poll_table_cleanup`; no sleep or user copy occurs under it. Lock order is i8042 then event then poll wait queue in the IRQ producer, while read and poll never acquire i8042. The host test uses a parser double for event production, so QEMU userspace behavior remains the principal unverified integration point.

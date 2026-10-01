# Repeated systest: active page-table lifetime across CPUs

## Reproduction

From the normal terminal/BusyBox ash, run `systest`, followed by
`for i in 1..3; do systest; done`. In ash, `1..3` is one literal word;
`for i in 1 2 3; do systest; done` actually runs three iterations.

The new `qemutests/x86_64_systest_repeat.py` runs all three stages (five suites
total) on a private copy of the normal disk. On the original image it
passed the first two stages and QEMU exited during the last stage. Other
four-CPU runs failed during the first suite, so the failure is timing
sensitive. QEMU `-d int,cpu_reset` recorded recursive kernel page faults
and a final triple fault, including lost LAPIC and kernel-code mappings.

## Two confirmed lifetime errors

### Freeing the local CPU's active PGD

A hardware breakpoint at `kfree`, conditioned on
`$rdi == $cr3 + 0xffff800000000000`, stopped with:

```
FREEING ACTIVE PGD: rdi=0xffff800001208000 cr3=0x1208000
#0 kfree
#1 sys_exec
#2 do_system_call
```

`sys_exec` freed the old hierarchy before installing the new CR3.
`do_exit` likewise freed the current hierarchy before its final schedule.
Freed page-table storage could be reused while hardware still referred
to it. Updating hardware alone would not suffice: a preempted task could
resume using its stale `thread->cr3`.

The fix switches exit to the permanent kernel PGD, and switches exec to
its prepared new PGD, before teardown. Each saved-CR3/hardware-CR3 update
runs with local interrupts disabled. Exec also publishes its new mm in
that interval, after all fallible preparation and user argument copies.

### AP idle retained another task's user PGD

The local lifetime fix alone passed five suites with one CPU but still
failed with four. At a hardware breakpoint on `task_init` (after AP boot),
GDB showed:

```
init_mm.pgdir = 0
CPU 1: saved_cr3=0 hardware_cr3=0x101000
CPU 2: saved_cr3=0 hardware_cr3=0x101000
CPU 3: saved_cr3=0 hardware_cr3=0x101000
CPU 4: saved_cr3=0 hardware_cr3=0x101000
```

`arch_task_init_platform` initialized the kernel PGD too late. AP entry
had already copied zero into each idle thread's saved CR3, and boot-time
kthreads could inherit zero too. `__switch_to` intentionally skips a zero
next CR3, leaving the previous user's PGD active on the idle CPU. That
user task can migrate and free its PGD elsewhere while the idle CPU
continues to use it. Failing traces had AP idle-stack addresses and a
user CR3, consistent with this path.

Kernel PGD capture now happens in `arch_task_init_early`, before AP and
boot-kthread creation. `vmm_init` extends that same root rather than
replacing it. Thus idle/kthread saved CR3 values name the permanent
kernel PGD, and switching away from user tasks relinquishes their PGDs.

## Regression command

```
make PROFILE=x86_64-clang SMP=4 test-syscall-repeat
```

This uses normal init, not `OS01_SYSTEST=1`, and rejects kernel selftests.
The Python driver checks each suite result and a shell completion marker
that cannot be satisfied by the terminal echo of its input command.
A timeout or premature QEMU exit fails the test. Serial output is saved
at `/tmp/os01-systest-repeat.log` by default.

No structs or user ABI changed. Independent PMM range/index issues and
fork's unsafe OOM-sharing fallbacks remain separate work; this change
addresses the two observed active-PGD lifetime errors.

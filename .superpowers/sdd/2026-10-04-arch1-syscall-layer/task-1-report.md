# Task 1 report: syscall boundary and incremental dispatcher

## Result

Implemented the generic syscall context and dispatcher boundary. The static syscall table has the UAPI range, stores handler/name pairs, and is intentionally unpopulated in this migration stage. Unknown and unimplemented numbers, including `SYS_getpeername` (62), return `-EINVAL`. x86_64 preserves the Linux syscall translation before constructing the context; it dispatches populated entries through the new interface and falls back to the existing syscall switch otherwise. Both branches converge on the original CPL=3 signal-delivery tail.

## RED/GREEN evidence

- RED: before adding the API, `test_syscall_dispatch.c` failed to compile because `syscall/dispatch.h` did not exist.
- GREEN: `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE=$PWD/mk/profiles/x86_64-clang.mk test-syscall-dispatch` passed (`syscall dispatch: PASS`). This test links the production `kernel/syscall/dispatch.c` and checks an unimplemented in-range number, an out-of-range number, and `-EINVAL` dispatch behavior.
- x86 kernel translation units `trap.c` and `syscall/dispatch.c` compiled successfully with the kernel's freestanding x86_64 flags by targeting their object files directly.
- Full `make PROFILE=x86_64-clang kernel.bin` was attempted after `make clean`; sysroot preparation stopped before kernel linking with `ERROR: private mbedtls copy incomplete`. A subsequent normal trap object build also showed the stable sysroot was absent. Using the profile's staged libc include directory, both focused object compiles succeeded.
- `git diff --check` passed.

## Changed files

- `kernel/include/syscall/dispatch.h` — context, handler typedef, and dispatcher declarations.
- `kernel/syscall/dispatch.c` — static range-sized table, lookup, and `-EINVAL` default.
- `kernel/Makefile` — discovers `kernel/syscall/*.c` for x86_64.
- `kernel/arch/x86_64/intr/trap.c` — context adapter, populated-handler dispatch, and legacy-switch fallback.
- `hosttests/cases/test_syscall_dispatch.c` — focused behavior test.
- `hosttests/Makefile` — focused target and `TEST_BINS` registration.

## Review notes

The Linux number translation is unchanged and still precedes the adapter. The legacy switch body remains intact. Signal delivery remains outside the new/legacy branch so there is a single return tail. No syscall handlers have migrated yet, by design; E2E syscall behavior remains on the legacy baseline.

# Syscall interface

System calls use `int $0x80` with syscall number in `rax` and arguments in `rdi`, `rsi`, `rdx`. Return value in `rax`.

## Syscall table

| NR | Name | Args | Description |
|----|------|------|-------------|
| 0 | `SYS_putchar` | rdi=char | Write one character to framebuffer |
| 1 | `SYS_write` | rdi=str, rsi=len | Write string to framebuffer |
| 2 | `SYS_exit` | rdi=code | Terminate current process |
| 3 | `SYS_brk` | rdi=addr | Get/set program break (0=query) |
| 4 | `SYS_getpid` | — | Return current PID |
| 5 | `SYS_exec` | rdi=path | Replace process image with ELF file |
| 6 | `SYS_read` | rdi=path, rsi=buf, rdx=size | Read from VFS file/device |

## Definitions

Syscall numbers are duplicated in two locations — both must stay in sync:
- `kernel/include/uapi/syscall.h` (kernel)
- `libc/include/sys/syscall.h` (user)

## User-space invocation

The libc `syscall()` inline function in `<sys/syscall.h>` wraps the `int $0x80` invocation:

```c
static inline int64_t syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}
```

Higher-level libc functions (`read()`, `exec()`, etc.) call this.

## Kernel-side dispatch

`kernel/arch/x86_64/trap.c` — `do_system_call()`:
- Entry via `entry.S:system_call` → `error_code:` path → `do_system_call(regs, 0)`
- Dispatches on `regs->rax` (syscall number)
- Arguments from `regs->rdi`, `regs->rsi`, `regs->rdx`
- Sets `regs->rax` as return value
- For `SYS_read` and `SYS_exec`: copies user-provided path strings to kernel heap (`strdup`) before VFS operations to prevent TOCTOU races
- Return via `ret_from_exception` → `RESTORE_ALL` → `iretq`

## Known bug

`SYS_putchar` → `color_printk` shares a static 4KB buffer with `serial_printk`. Repeated spawn of programs using framebuffer I/O (like `/init.elf` with `printf`) can corrupt the buffer during concurrent use, causing crashes on the 3rd–4th spawn. See [[spawn-ud-crash-syscall-prefault]].

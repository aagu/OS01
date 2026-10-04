#include <arch/syscall.h>
#include <arch/x86_64/hw.h>
#include <core/printk.h>
#include <driver/serial.h>
#include <intr/apic.h>
#include <sched/task.h>
#include <uapi/syscall.h>
#include <log/log.h>
#include <memory/uaccess.h>
#include <errno.h>

int64_t arch_syscall_putchar(uint64_t ch)
{
    // putchar(int c) — write one char to framebuffer AND serial
    char c = (char)ch;
    color_printk(WHITE, BLACK, "%c", c);
    {
        // Hold serial_lock across the putchar so the byte is
        // emitted atomically with respect to other writers.
        // Use write_serial_unlocked to avoid re-locking deadlock
        // (write_serial() now acquires serial_lock internally —
        // see kernel/driver/serial.c).
        uint64_t sf = spin_lock_irqsave(&serial_lock);
        write_serial_unlocked(c);  // also echo to serial for interactive shell
        spin_unlock_irqrestore(&serial_lock, sf);
    }
    return (unsigned char)c;
}

void arch_syscall_reboot(int cmd)
{
    log_info("syscall: reboot(cmd=%d) from pid=%d\n",
             cmd, (int)current->pid);

    // ── ACPI power-off ─────────────────────────────────
    if (cmd == RB_POWER_OFF && apic_info.pm1a_port) {
        // SLP_EN (bit 13) toggles sleep.  SLP_TYPa=0 for
        // S5 on QEMU q35 (the \_S5 object reports {0, 0}).
        log_info("ACPI: powering off via PM1a=%#x\n",
                 (unsigned)apic_info.pm1a_port);
        outw(apic_info.pm1a_port, 0x2000);
        // Block — platform powers off asynchronously.
        while (1) __asm__ __volatile__("hlt");
    }

    // ── ACPI reboot / halt fallback ────────────────────
    // Keyboard controller pulse-reset ($0xFE → port $0x64)
    while ((inb(0x64) & 0x02) != 0) { /* wait */ }
    outb(0xFE, 0x64);
    while (1) __asm__ __volatile__("hlt");
}

int64_t arch_syscall_fork(void *arch_frame)
{
    return do_fork((pt_regs_t *)arch_frame, 0, 0, 0);
}

int64_t arch_syscall_exec(void *arch_frame, const char *path,
                          const char *const *argv, const char *const *envp)
{
    return sys_exec(path, (pt_regs_t *)arch_frame, argv, envp);
}

int64_t arch_syscall_sigreturn(void *arch_frame)
{
    pt_regs_t *regs = (pt_regs_t *)arch_frame;
    // regs->rsp == sigframe start in user space (handler ret pop'd
    // trampoline, then int $0x80 saved this RSP as pt_regs->rsp).
    // Read the whole frame via VIRTUAL addresses (per-page walker)
    // so a frame that crosses a 4KB page boundary is read correctly;
    // the old user_va_to_phys+Phy_To_Virt only translated the start
    // and would read junk for the second page under 4KB mappings.
    struct sigframe frame;
    if (!syscall_check_user_range(regs->rsp, sizeof(frame), false) ||
        copy_from_user_ft(&frame, (void *)regs->rsp,
                          sizeof(frame)) < 0) {
        return -EFAULT;  // other regs untouched
    }

    // Validate sigframe: iretq CS must be ring-3
    if ((frame.cs & 3) != 3) return -EINVAL;

    // Restore blocked mask
    current->blocked = frame.blocked;

    // Restore all GPRs (from kernel-stack frame only)
    regs->r15=frame.r15; regs->r14=frame.r14; regs->r13=frame.r13;
    regs->r12=frame.r12; regs->r11=frame.r11; regs->r10=frame.r10;
    regs->r9=frame.r9;   regs->r8=frame.r8;
    regs->rbx=frame.rbx; regs->rcx=frame.rcx; regs->rdx=frame.rdx;
    regs->rsi=frame.rsi; regs->rdi=frame.rdi; regs->rbp=frame.rbp;
    regs->ds=frame.ds;   regs->es=frame.es;    regs->rax=frame.rax;

    // Restore iretq frame → RESTORE_ALL → iretq to original context
    regs->rip=frame.rip; regs->cs=frame.cs; regs->rflags=frame.rflags;
    regs->rsp=frame.rsp; regs->ss=frame.ss;

    // Pending signals are delivered by the entry adapter after restoration.
    return 0;
}

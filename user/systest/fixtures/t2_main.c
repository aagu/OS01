/*
 * t2_main.c — the T2 dynamic-link acceptance main.
 *
 * Freestanding, -fPIE, linked with:
 *   -pie --dynamic-linker /lib/ld.so --hash-style=sysv -z now
 *   -z norelro -z max-page-size=4096 --pack-dyn-relocs=none
 * (no -static: lld refuses -static -pie with dynamic inputs)
 *
 * Carries PT_INTERP=/lib/ld.so and — through the SONAME of the lib
 * given on the link line — exactly one DT_NEEDED.  The reference
 * shapes the contract exercises:
 *   - the call to t2_add   → R_X86_64_JUMP_SLOT in .rela.plt
 *   - the read of t2_value → R_X86_64_GLOB_DAT  in .rela.dyn
 *
 * Success path (every relocation resolved correctly by /lib/ld.so):
 * t2_add(1,1) == 43, plus t2_value == 41 and t2_gain() == 7 (a
 * MAIN-defined symbol the lib imports — the scope order proof) → 91;
 * the
 * main prints [DYN-T2] and exits 0.  A wrong value (ld.so skipped
 * the library's RELATIVE pass, or resolved a symbol to the wrong
 * bias) exits 92; a missing AT_BASE (never execed through the
 * interpreter) exits 94.  Entry: _start comes from the shared
 * interp_entry.S stub, which hands the initial rsp in rdi.
 */

#include <stdint.h>
#include <stddef.h>

#define SYS_write 1L
#define SYS_exit  2L

#define AT_BASE 7UL
#define AT_ENTRY 9UL

static long sysc3(long n, long a, long b, long c)
{
    long r;
    /* OS01 native int $0x80 ABI: rax=nr, rdi/rsi/rdx=args. */
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "memory");
    return r;
}

static void put_line(const char *s, unsigned len)
{
    sysc3(SYS_write, 1, (long)s, (long)len);
}

static void exit_raw(long code)
{
    sysc3(SYS_exit, code, 0, 0);
    for (;;)
        ;
}

#ifdef T2_GNUABI
/* The ABI-gate negative fixture: a GNU ABI tag note (name "GNU",
 * type NT_GNU_ABI_TAG, desc os=ELF_NOTE_OS_LINUX=0) so
 * elf_detect_abi() marks the execed task PF_LINUX_ABI and the
 * exec layer rejects it before the interpreter ever runs.  The
 * section MUST be @note (SHT_NOTE): with @progbits lld packs it
 * into a LOAD and no PT_NOTE appears — the kernel then never reads
 * the record. */
__asm__(".section .note.gnu.abi-tag,\"a\",@note\n"
        ".balign 4\n"
        ".long 4, 16, 1\n"          /* namesz, descsz, NT_GNU_ABI_TAG */
        ".asciz \"GNU\"\n"
        ".balign 4\n"
        ".long 0, 3, 0, 0\n"        /* os=Linux(0), major=3 */
        ".previous");
#endif /* T2_GNUABI */

extern long t2_value;              /* GLOB_DAT against libt2.so */
long t2_add(long a, long b);       /* JUMP_SLOT via .rela.plt */
long t2_gain(void);                /* JUMP_SLOT; lib imports main_exported */

/* Defined here, referenced ONLY by the library: the executable
 * exports a symbol without ever taking it through its own GOT, so
 * the main image has no symbolic relocation for it — ld.so must put
 * the main in the lookup scope regardless (spec §6). */
long main_exported = 7;

static const char dyn_marker[] = "[DYN-T2]\n";

static uint64_t auxv_get(const uint64_t *sp, uint64_t tag, uint64_t fallback)
{
    const uint64_t *p = sp;
    uint64_t argc = *p++;
    p += argc;                 /* skip argv[] */
    p += 1;                    /* argv NULL */
    while (*p != 0)            /* skip envp[] */
        p++;
    p += 1;                    /* envp NULL */
    for (; p[0] != 0; p += 2) {
        if (p[0] == tag)
            return p[1];
    }
    return fallback;
}

/* Same C entry symbol the shared stub calls (interp_entry.S). */
void interp_probe_main(uint64_t orig_rsp)
{
    const uint64_t *sp = (const uint64_t *)orig_rsp;
    if (auxv_get(sp, AT_BASE, 0) == 0)
        exit_raw(94);          /* never execed through /lib/ld.so */

#ifndef T2_EXPORT_ONLY
    long r = t2_add(1, 1) + t2_value + t2_gain();
    if (r != 91)
        exit_raw(92);          /* relocations did not land where the
                                * psABI says they must */
#endif
    /* T2_EXPORT_ONLY: the main carries DT_NEEDED but NO symbolic
     * relocations of its own — every import lives on the library
     * side (t2_gain reads main_exported).  Success is reaching this
     * marker at all: ld.so resolved the library's import against the
     * main image, proving the lookup scope includes the executable
     * even when it imports nothing. */

    put_line(dyn_marker, sizeof(dyn_marker) - 1);
    exit_raw(0);
}

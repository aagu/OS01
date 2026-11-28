/*
 * ldso.c — the OS01 dynamic linker (/lib/ld.so).
 *
 * The kernel loads this image as a second ET_DYN at its own ASLR-B
 * bias whenever the main image carries PT_INTERP, hands it the
 * initial RIP with the original SysV stack untouched, and publishes
 * the interpreter bias in auxv AT_BASE while AT_ENTRY/AT_PHDR keep
 * describing the main image.  This file is the T1 skeleton moved out
 * of the fixture tree (user/systest/fixtures/interp_probe.c) and
 * grown into the T2 linker:
 *
 *   - walk auxv (from the FIRST pair — starting the scan anywhere
 *     else silently falls back on every lookup),
 *   - locate the main image's PT_DYNAMIC through AT_PHDR,
 *   - load each DT_NEEDED object: open, shape-validate, one
 *     anonymous mmap, read the file in per PT_LOAD, mprotect each
 *     segment window,
 *   - resolve symbols through the SYSV DT_HASH tables (GNU hash is
 *     not supported: every image is linked --hash-style=sysv),
 *   - apply the relocations: RELATIVE in libraries, GLOB_DAT /
 *     JUMP_SLOT everywhere (the kernel already applied the main
 *     image's RELATIVE records; applying again is idempotent, so
 *     they are skipped),
 *   - sign the T1 handshake pad and jump to AT_ENTRY.
 *
 * Freestanding, no libc: raw int $0x80 with the OS01 native ABI
 * (rax=nr, rdi/rsi/rdx[/r10/r8/r9]).  Every failure exits with a
 * distinct code in the 90/95-99 range and prints one [LDSO-ERR-*]
 * line so a serial log pins the failure point.
 */

#include "ldso_elf.h"

/* ── raw system calls (OS01 native int $0x80 ABI) ───────────── */

#define SYS_write    1L
#define SYS_exit     2L
#define SYS_read     6L
#define SYS_open     7L
#define SYS_close    8L
#define SYS_lseek    18L
#define SYS_mmap     44L
#define SYS_mprotect 45L

#define PROT_READ  0x1L
#define PROT_WRITE 0x2L
#define PROT_EXEC  0x4L
#define PROT_NONE  0x0L

#define MAP_PRIVATE   0x02L
#define MAP_FIXED     0x10L
#define MAP_ANONYMOUS 0x20L

static long sysc3(long n, long a, long b, long c)
{
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "memory");
    return r;
}

static long sysc6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "D"(a), "S"(b), "d"(c),
                           "r"(r10), "r"(r8), "r"(r9)
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

/* ── diagnostics: fixed literals printed whole, one line each ── */
#define LDSO_ERR(tag) put_line("[LDSO-ERR-" tag "]\n", \
                               sizeof("[LDSO-ERR-" tag "]\n") - 1)

/* ── markers (T1 strings preserved verbatim) ────────────────── */

static const char ldso_enter[] = "[LDSO-ENTER]\n";
static const char ldso_marker[] = "OS01_LDSO\n";
static const char ldso_reloc[] = "[LDSO-RELOC]\n";

/* ── auxv walk (T1 contract: the pair scan starts at the FIRST
 * auxv pair; starting it at the AT_NULL slot makes the p[0] != 0
 * loop a no-op and every lookup silently falls back) ─────────── */

/* Address of the AT_NULL tag slot (NULL if the walk runs off — the
 * builder always emits AT_NULL, so a miss means a foreign stack). */
static const uint64_t *auxv_null_slot(const uint64_t *sp)
{
    const uint64_t *p = sp;
    uint64_t argc = *p++;
    p += argc;                 /* skip argv[] */
    p += 1;                    /* argv NULL */
    while (*p != 0)            /* skip envp[] */
        p++;
    p += 1;                    /* envp NULL */
    for (unsigned i = 0; i < 32; i++, p += 2) {
        if (p[0] == 0)
            return p;
    }
    return NULL;
}

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

/* ── T1 skeleton (kept verbatim in behaviour) ────────────────────
 * The C entry keeps the fixture's symbol name: interp_entry.S calls
 * interp_probe_main, and one entry stub serves /lib/ld.so and the
 * PT_INTERP fixtures alike. */

void interp_probe_main(uint64_t orig_rsp)
{
    const uint64_t *sp = (const uint64_t *)orig_rsp;
    put_line(ldso_enter, sizeof(ldso_enter) - 1);
    uint64_t base  = auxv_get(sp, AT_BASE, 0);
    uint64_t entry = auxv_get(sp, AT_ENTRY, 0);

    /* AT_BASE != 0 only when the kernel built this stack for an
     * interpreted main image (we are the interpreter).  Execed
     * directly, AT_BASE is 0 and jumping to AT_ENTRY (our own entry)
     * would loop forever — report the role and exit. */
    if (base == 0 || entry == 0) {
        LDSO_ERR("ROLE");
        exit_raw(91);
    }

    /* Handshake for the main image: mark the pad after AT_NULL so
     * the main can prove its control flow really passed through
     * here.  The pad is at least 16 bytes (builder contract). */
    {
        const uint64_t *at_null = auxv_null_slot(sp);
        if (at_null == NULL) {
            LDSO_ERR("ROLE");
            exit_raw(91);
        }
        uint64_t *pad = (uint64_t *)(uintptr_t)at_null;
        pad[2] = OS01_INTERP_TAG;
        pad[3] = OS01_INTERP_MAGIC;
    }

    put_line(ldso_marker, sizeof(ldso_marker) - 1);

    /* T2 relocation machinery fills in here (loaded objects, symbol
     * resolution, GLOB_DAT/JUMP_SLOT fixups).  The marker below is
     * printed once everything the main image needs is in place. */
    put_line(ldso_reloc, sizeof(ldso_reloc) - 1);

    /* Hand control back to the main image with the original stack.
     * Nothing may sit above rsp in this frame when we jump. */
    __asm__ __volatile__("movq %0, %%rsp\n\t"
                         "jmp *%1"
                         :
                         : "r"(orig_rsp), "r"(entry)
                         : "memory");
    LDSO_ERR("ROLE");
    exit_raw(91);
}

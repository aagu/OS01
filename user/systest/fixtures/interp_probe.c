/*
 * interp_probe.c — PT_INTERP exec-acceptance probes (x86_64 T1).
 *
 * Freestanding, no libc, raw int $0x80 — same shape as the PT_TLS
 * fixtures.  One source, two images:
 *
 *   -DINTERP_LDSO  the /lib/ld.so skeleton.  The kernel loads it as a
 *                  second ET_DYN image and hands it the initial RIP
 *                  with the original SysV stack untouched.  It walks
 *                  auxv, prints one marker line, then restores the
 *                  stack pointer and jumps to AT_ENTRY (the main
 *                  image).  It relocates nothing — symbol relocation
 *                  is T2.
 *   -DINTERP_MAIN  the PT_INTERP-carrying main image.  Control
 *                  arrives here only after the ld.so handoff; it
 *                  verifies the auxv describes an interpreted image
 *                  (AT_BASE set, AT_PHDR pointing at a table that
 *                  still contains its own PT_INTERP) and exits 0.
 *
 * Exit codes: 90 = defensive-halt path (entry stub), 91..93 = auxv
 * verification failures, 0 = pass.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SYS_write 1L
#define SYS_exit  2L

/* auxv tags (kernel/include/sys/auxv.h values, redeclared freestanding) */
#define AT_PHDR   3UL
#define AT_PHENT  4UL
#define AT_PHNUM  5UL
#define AT_BASE   7UL
#define AT_ENTRY  9UL

/* Minimal ELF64 program header (the only structure the probes read). */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

#define PT_INTERP 3U

static long sysc3(long n, long a, long b, long c)
{
    long r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(n), "b"(a), "c"(b), "d"(c)
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

/* Walk the SysV initial stack: argc | argv[] | NULL | envp[] | NULL |
 * auxv pairs | AT_NULL.  Returns the requested a_val, or fallback. */
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

#ifdef INTERP_LDSO
static const char ldso_marker[] = "OS01_LDSO\n";

/* C entry: rdi = the kernel-built initial rsp (from interp_entry.S).
 * Both image flavours export the same entry symbol; only one is
 * active per build. */
void interp_probe_main(uint64_t orig_rsp)
{
    const uint64_t *sp = (const uint64_t *)orig_rsp;
    uint64_t base  = auxv_get(sp, AT_BASE, 0);
    uint64_t entry = auxv_get(sp, AT_ENTRY, 0);

    /* AT_BASE != 0 only when the kernel built this stack for an
     * interpreted main image (we are the interpreter).  Execed
     * directly, AT_BASE is 0 and jumping to AT_ENTRY (our own entry)
     * would loop forever — report the role and exit. */
    if (base == 0 || entry == 0)
        exit_raw(91);

    put_line(ldso_marker, sizeof(ldso_marker) - 1);

    /* Hand control back to the main image with the original stack.
     * Nothing may sit above rsp in this frame when we jump. */
    __asm__ __volatile__("movq %0, %%rsp\n\t"
                         "jmp *%1"
                         :
                         : "r"(orig_rsp), "r"(entry)
                         : "memory");
    exit_raw(91);
}
#endif /* INTERP_LDSO */

#ifdef INTERP_MAIN
static const char main_marker[] = "OS01_DYNMAIN\n";

void interp_probe_main(uint64_t orig_rsp)
{
    const uint64_t *sp = (const uint64_t *)orig_rsp;
    uint64_t base   = auxv_get(sp, AT_BASE, 0);
    uint64_t phdr   = auxv_get(sp, AT_PHDR, 0);
    uint64_t phent  = auxv_get(sp, AT_PHENT, 0);
    uint64_t phnum  = auxv_get(sp, AT_PHNUM, 0);

    if (base == 0)
        exit_raw(92);            /* no interpreter bias: T1 not honoured */

    /* AT_PHDR must address a program-header table that still contains
     * this image's own PT_INTERP — i.e. it describes the main image,
     * not the interpreter. */
    if (phdr == 0 || phent != sizeof(elf64_phdr_t) || phnum == 0 ||
        phnum > 128)
        exit_raw(93);

    const elf64_phdr_t *ph = (const elf64_phdr_t *)phdr;
    bool has_interp = false;
    for (uint64_t i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_INTERP)
            has_interp = true;
    }
    if (!has_interp)
        exit_raw(93);

    put_line(main_marker, sizeof(main_marker) - 1);
    exit_raw(0);
}
#endif /* INTERP_MAIN */

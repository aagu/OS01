/*
 * ldso.c — the OS01 dynamic linker (/lib/ld.so).
 *
 * The kernel loads this image as a second ET_DYN at its own ASLR-B
 * bias whenever the main image carries PT_INTERP, hands it the
 * initial RIP with the original SysV stack untouched, and publishes
 * the interpreter bias in auxv AT_BASE while AT_ENTRY/AT_PHDR keep
 * describing the main image.  Behaviour by slice:
 *
 *   T1 (contract preserved verbatim): walk auxv — the pair scan
 *   starts at the FIRST pair, anywhere else silently falls back —
 *   sign the handshake pad after AT_NULL, print the markers, and
 *   jump to AT_ENTRY.  Execed directly (AT_BASE == 0) it exits 91.
 *
 *   T2: locate the main image's PT_DYNAMIC through AT_PHDR, load
 *   each DT_NEEDED object (open, shape-validate, one anonymous
 *   mmap, read the file in per PT_LOAD, mprotect each segment
 *   window), resolve symbols through the SYSV DT_HASH tables, and
 *   apply GLOB_DAT/JUMP_SLOT relocations (libraries additionally
 *   apply their own RELATIVE records; the kernel already applied
 *   the main image's — repeating them is idempotent, so they are
 *   skipped there).  GNU hash is not supported: shared objects are
 *   linked --hash-style=sysv and must carry DT_HASH; the MAIN image
 *   may carry a GNU hash alongside (T1 fixtures link the default
 *   style) — it is ignored, DT_HASH is what this linker reads.
 *
 * Freestanding, no libc: raw int $0x80 with the OS01 native ABI
 * (rax=nr, rdi/rsi/rdx[/r10/r8/r9]).  Every failure exits with a
 * distinct code (91, 95-99) after printing one [LDSO-ERR-*] line so
 * a serial log pins the failure point:
 *   95 open/missing library, 96 unresolved symbol, 97 unknown
 *   relocation type, 98 shape/contract/validation violation,
 *   99 internal error.
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

#define SEEK_SET 0L
#define SEEK_END 2L

#define PROT_READ  0x1L
#define PROT_WRITE 0x2L
#define PROT_EXEC  0x4L

#define MAP_PRIVATE   0x02L
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

/* ── per-image view ─────────────────────────────────────────── */

#define LDSO_MAX_LIBS     8U
#define LDSO_PATH_MAX     160U
#define LDSO_PHDR_MAX     128U
#define LDSO_DYN_MAX      128U   /* dynamic entry cap per image */
#define LDSO_NEEDED_MAX   8U     /* DT_NEEDED entries per image */
#define LDSO_PAGE         4096ULL
#define LDSO_SYMENT       24U
#define LDSO_SCOPE_BOUND  65536U /* hash nbucket/nchain bound */
#define LDSO_GOT_RESERVED 24ULL  /* GOT[0..2] behind DT_PLTGOT */
#define LDSO_NAME_MAX     64U    /* symbol name scratch */


/* ── tiny string/memory helpers (freestanding) ──────────────── */

/* Freestanding memcpy/memset: clang lowers struct assignment and
 * recognized idioms to these calls even under -fno-builtin, and the
 * -nostdlib link provides no library — define them here. */
void *memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, uint64_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

static void lds_zero(void *dst, uint64_t n)
{
    memset(dst, 0, n);
}

static void lds_copy(void *dst, const void *src, uint64_t n)
{
    memcpy(dst, src, n);
}

static bool lds_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static bool add_ovf(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return true;
    *out = a + b;
    return false;
}

static bool mul_ovf(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b != 0 && a > UINT64_MAX / b)
        return true;
    *out = a * b;
    return false;
}

static uint64_t align_down4k(uint64_t v)
{
    return v & ~(LDSO_PAGE - 1);
}

static bool align_up4k(uint64_t v, uint64_t *out)
{
    uint64_t r;
    if (add_ovf(v, LDSO_PAGE - 1, &r))
        return true;
    *out = r & ~(LDSO_PAGE - 1);
    return false;
}

/* ── auxv walk (T1 contract: the pair scan starts at the FIRST
 * auxv pair; starting it at the AT_NULL slot makes the p[0] != 0
 * loop a no-op and every lookup silently falls back) ─────────── */

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

typedef struct {
    uint64_t bias;             /* map_start - lo (libs); AT_PHDR - PT_PHDR (main) */
    /* Raw link-time vaddrs.  Every one is bounds-checked against the
     * image's own PT_LOADs (readable-LOAD coverage) before any
     * dereference; absolute address = bias + raw. */
    uint64_t dyn_raw, dyn_bytes;
    uint64_t rela_raw, rela_bytes;
    uint64_t jmprel_raw, jmprel_bytes;
    uint64_t pltgot_raw;       /* 0 = absent */
    uint64_t strtab_raw, strsz;
    uint64_t symtab_raw, syment;
    uint64_t hash_raw;
    uint64_t nbucket, nchain;  /* SYSV hash header (0 = not read yet) */
    uint64_t needed_off[LDSO_NEEDED_MAX];
    unsigned nneeded;
    bool has_pltrel;           /* DT_PLTREL seen; value pinned to DT_RELA */
} dso_img_t;

/* Loaded images: [0] is the main image, [1..] the libraries in
 * DT_NEEDED discovery order.  Static storage — the linker is
 * freestanding and this slice has no heap contract. */
static dso_img_t g_dso[1 + LDSO_MAX_LIBS];
static char g_path[1 + LDSO_MAX_LIBS][LDSO_PATH_MAX];
static unsigned g_ndso;
static bool g_main_hash_ok;
/* Per-library phdr snapshots: load_lib reuses one scratch buffer, and
 * relocation application runs after ALL loads, so each library's
 * program headers are copied out at load time. */
static elf64_phdr_t g_lib_phdrs[LDSO_MAX_LIBS][LDSO_PHDR_MAX];
static unsigned g_lib_phnum[LDSO_MAX_LIBS];

static const elf64_phdr_t *find_phdr(const elf64_phdr_t *ph, unsigned phnum,
                                     uint32_t type)
{
    for (unsigned i = 0; i < phnum; i++) {
        if (ph[i].p_type == type)
            return &ph[i];
    }
    return NULL;
}

/* [vaddr, vaddr+bytes) fully inside one readable PT_LOAD of this
 * image (filesz coverage — the mapping mirrors the file layout). */
static bool range_in_readable_load(const elf64_phdr_t *ph, unsigned phnum,
                                   uint64_t vaddr, uint64_t bytes)
{
    if (bytes == 0)
        return false;
    for (unsigned i = 0; i < phnum; i++) {
        const elf64_phdr_t *l = &ph[i];
        if (l->p_type != PT_LOAD || !(l->p_flags & PF_R) ||
            l->p_filesz == 0)
            continue;
        if (vaddr < l->p_vaddr)
            continue;
        uint64_t off = vaddr - l->p_vaddr;
        if (off > l->p_filesz || bytes > l->p_filesz - off)
            continue;
        return true;
    }
    return false;
}

/* The writable non-executable PT_LOAD containing [vaddr, vaddr+8):
 * the only legitimate relocation target region. */
static bool target_in_writable_load(const elf64_phdr_t *ph, unsigned phnum,
                                    uint64_t vaddr)
{
    if (vaddr > UINT64_MAX - 8)
        return false;
    for (unsigned i = 0; i < phnum; i++) {
        const elf64_phdr_t *l = &ph[i];
        if (l->p_type != PT_LOAD)
            continue;
        if (!(l->p_flags & PF_W) || (l->p_flags & PF_X))
            continue;
        if (vaddr < l->p_vaddr)
            continue;
        uint64_t off = vaddr - l->p_vaddr;
        if (off > l->p_filesz || 8 > l->p_filesz - off)
            continue;
        return true;
    }
    return false;
}

/* ── bounded dynamic-section scan ────────────────────────────────
 * `bytes` must be 16-byte granular; DT_NULL must appear within the
 * declared range.  Values are recorded as RAW link-time vaddrs —
 * bounds checking happens against the image's PT_LOADs afterwards.
 * reject_gnu_hash: shared objects must be --hash-style=sysv; the
 * main image may carry a GNU hash (T1 default-style links) and it
 * is simply ignored. */

static bool scan_dynamic(const elf64_dyn_t *dyn, uint64_t bytes,
                         bool reject_gnu_hash, dso_img_t *out)
{
    if ((bytes & 15ULL) != 0 || bytes < 16)
        return false;
    uint64_t n = bytes / 16;
    if (n == 0 || n > LDSO_DYN_MAX)
        return false;
    bool seen_null = false;
    for (uint64_t i = 0; i < n; i++) {
        int64_t tag = dyn[i].d_tag;
        uint64_t val = dyn[i].d_val;
        if (tag == DT_NULL) {
            seen_null = true;
            break;
        }
        switch (tag) {
        case DT_NEEDED:
            if (out->nneeded >= LDSO_NEEDED_MAX)
                return false;
            out->needed_off[out->nneeded++] = val;
            break;
        case DT_RELA:
            if (out->rela_raw != 0)
                return false;
            out->rela_raw = val;
            break;
        case DT_RELASZ:
            if (out->rela_bytes != 0)
                return false;
            out->rela_bytes = val;
            break;
        case DT_RELAENT:
            if (val != LDSO_SYMENT)
                return false;
            break;
        case DT_RELACOUNT:
            /* Prefix semantics belong to the kernel's RELATIVE pass;
             * this linker treats the tag as inert. */
            break;
        case DT_JMPREL:
            if (out->jmprel_raw != 0)
                return false;
            out->jmprel_raw = val;
            break;
        case DT_PLTRELSZ:
            if (out->jmprel_bytes != 0)
                return false;
            out->jmprel_bytes = val;
            break;
        case DT_PLTGOT:
            if (out->pltgot_raw != 0)
                return false;
            out->pltgot_raw = val;
            break;
        case DT_PLTREL:
            if (out->has_pltrel || val != DT_RELA)
                return false;
            out->has_pltrel = true;
            break;
        case DT_STRTAB:
            if (out->strtab_raw != 0)
                return false;
            out->strtab_raw = val;
            break;
        case DT_STRSZ:
            if (out->strsz != 0)
                return false;
            out->strsz = val;
            break;
        case DT_SYMTAB:
            if (out->symtab_raw != 0)
                return false;
            out->symtab_raw = val;
            break;
        case DT_SYMENT:
            if (out->syment != 0)
                return false;
            out->syment = val;
            break;
        case DT_HASH:
            if (out->hash_raw != 0)
                return false;
            out->hash_raw = val;
            break;
        case DT_DEBUG:
        case DT_FLAGS:
        case DT_FLAGS_1:
            /* Binding is eager (-z now); the kernel already pinned
             * these for the main image.  Inert here. */
            break;
        case DT_GNU_HASH:
            if (reject_gnu_hash)
                return false;
            break;
        default:
            return false;
        }
    }
    return seen_null;
}

/* Read + bound one image's SYSV hash header.  The table bytes must
 * be covered by the image's readable LOADs. */
static bool init_hash_header(dso_img_t *d, const elf64_phdr_t *ph,
                             unsigned phnum)
{
    if (d->hash_raw == 0 || d->nbucket != 0)
        return d->nbucket != 0;
    if (!range_in_readable_load(ph, phnum, d->hash_raw, 16))
        return false;
    const uint32_t *hash = (const uint32_t *)(d->bias + d->hash_raw);
    uint64_t nbucket = hash[0];
    uint64_t nchain = hash[1];
    if (nbucket == 0 || nchain == 0 ||
        nbucket > LDSO_SCOPE_BOUND || nchain > LDSO_SCOPE_BOUND)
        return false;
    uint64_t entries, bytes;
    if (add_ovf(2, nbucket, &entries) ||
        add_ovf(entries, nchain, &entries) ||
        mul_ovf(entries, 4, &bytes))
        return false;
    if (!range_in_readable_load(ph, phnum, d->hash_raw, bytes))
        return false;
    if (d->syment != LDSO_SYMENT)
        return false;
    uint64_t symtab_bytes;
    if (mul_ovf(nchain, LDSO_SYMENT, &symtab_bytes))
        return false;
    if (d->symtab_raw == 0 ||
        !range_in_readable_load(ph, phnum, d->symtab_raw, symtab_bytes))
        return false;
    d->nbucket = nbucket;
    d->nchain = nchain;
    return true;
}

/* ── SYSV ELF hash + scope lookup ───────────────────────────── */

static uint64_t elf_hash(const char *name)
{
    uint64_t h = 0, g;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h = (h << 4) + *p;
        g = h & 0xf0000000ULL;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* String equality inside a bounded strtab window: the name must be
 * NUL-terminated WITHIN the window (no reads past strsz). */
static bool strtab_name_eq(const char *strtab, uint64_t strsz,
                           uint32_t off, const char *name)
{
    if (off >= strsz)
        return false;
    const char *s = strtab + off;
    uint64_t rem = strsz - off;
    for (uint64_t i = 0; i < rem; i++) {
        if (s[i] != name[i])
            return false;
        if (s[i] == '\0')
            return name[i] == '\0';
    }
    return false;
}

/* Search one image's dynsym via its SYSV hash table.  Returns the
 * resolved value S through `out`.  SHN_ABS symbols are absolute —
 * no bias applied (Review Focus 5: untested by the T2 fixtures; the
 * formula follows the gABI). */
static bool lookup_in_dso(const dso_img_t *d, const char *name, uint64_t *out)
{
    if (d->nbucket == 0 || d->nchain == 0 ||
        d->symtab_raw == 0 || d->strtab_raw == 0 || d->strsz == 0)
        return false;
    const uint32_t *hash = (const uint32_t *)(d->bias + d->hash_raw);
    uint64_t bucket = elf_hash(name) % d->nbucket;
    uint64_t idx = hash[2 + bucket];
    const uint32_t *chains = &hash[2 + d->nbucket];
    const elf64_sym_t *symtab = (const elf64_sym_t *)(d->bias + d->symtab_raw);
    const char *strtab = (const char *)(d->bias + d->strtab_raw);
    for (uint64_t step = 0; idx != 0 && step < d->nchain; step++) {
        if (idx >= d->nchain)
            return false;
        const elf64_sym_t *sym = &symtab[idx];
        uint32_t bind = ELF64_ST_BIND(sym->st_info);
        if ((bind == STB_GLOBAL || bind == STB_WEAK) &&
            sym->st_shndx != SHN_UNDEF &&
            strtab_name_eq(strtab, d->strsz, sym->st_name, name)) {
            *out = (sym->st_shndx == SHN_ABS) ? sym->st_value
                                              : d->bias + sym->st_value;
            return true;
        }
        idx = chains[idx];
    }
    return false;
}

/* Scope order: main first, then libraries in discovery order.  The
 * main image's hash header is initialized lazily: a RELATIVE-only
 * main without DT_HASH (legal, kernel position rule does not fire)
 * must never be searched, only skipped. */
static bool lookup_symbol(const char *name, uint64_t *out)
{
    for (unsigned i = 0; i < g_ndso; i++) {
        if (i == 0 && !g_main_hash_ok)
            continue;
        if (lookup_in_dso(&g_dso[i], name, out))
            return true;
    }
    return false;
}

/* ── relocation application ─────────────────────────────────── */

typedef struct {
    uint64_t lo, hi;   /* forbidden write windows, image-relative */
} guard_t;

#define GUARD_MAX 7U

/* Build the forbidden-write interval list for one image: everything
 * the loader still has to read after applying relocations (the
 * dynamic section, the relocation tables, the symbol/string/hash
 * metadata, and the reserved GOT[0..2] slots). */
static unsigned build_guards(const dso_img_t *d, guard_t *g)
{
    unsigned n = 0;
    g[n].lo = d->dyn_raw;
    g[n].hi = d->dyn_raw + d->dyn_bytes;
    n++;
    if (d->rela_bytes > 0) {
        g[n].lo = d->rela_raw;
        g[n].hi = d->rela_raw + d->rela_bytes;
        n++;
    }
    if (d->jmprel_bytes > 0) {
        g[n].lo = d->jmprel_raw;
        g[n].hi = d->jmprel_raw + d->jmprel_bytes;
        n++;
    }
    if (d->strsz > 0 && d->strtab_raw != 0) {
        g[n].lo = d->strtab_raw;
        g[n].hi = d->strtab_raw + d->strsz;
        n++;
    }
    if (d->symtab_raw != 0 && d->syment == LDSO_SYMENT && d->nchain > 0) {
        uint64_t bytes;
        if (!mul_ovf(d->nchain, d->syment, &bytes)) {
            g[n].lo = d->symtab_raw;
            g[n].hi = d->symtab_raw + bytes;
            n++;
        }
    }
    if (d->hash_raw != 0 && d->nbucket > 0 && d->nchain > 0) {
        uint64_t entries, bytes;
        if (!add_ovf(2, d->nbucket, &entries) &&
            !add_ovf(entries, d->nchain, &entries) &&
            !mul_ovf(entries, 4, &bytes)) {
            g[n].lo = d->hash_raw;
            g[n].hi = d->hash_raw + bytes;
            n++;
        }
    }
    if (d->pltgot_raw != 0) {
        g[n].lo = d->pltgot_raw;
        g[n].hi = d->pltgot_raw + LDSO_GOT_RESERVED;
        n++;
    }
    return n;
}

/* Symbol name for record index rsym, extracted from the OWNING
 * image's symtab/strtab with every read bounded by strsz. */
static bool sym_name_of(const dso_img_t *d, uint32_t rsym,
                        char *out, unsigned outsz)
{
    if (d->symtab_raw == 0 || d->strtab_raw == 0 || d->strsz == 0 ||
        d->syment != LDSO_SYMENT || d->nchain == 0)
        return false;
    if (rsym >= d->nchain)
        return false;
    const elf64_sym_t *sym =
        &((const elf64_sym_t *)(d->bias + d->symtab_raw))[rsym];
    if (sym->st_name >= d->strsz)
        return false;
    const char *s = (const char *)(d->bias + d->strtab_raw) + sym->st_name;
    uint64_t rem = d->strsz - sym->st_name;
    for (unsigned i = 0; i < outsz; i++) {
        if ((uint64_t)i >= rem)
            return false;          /* no NUL inside the window */
        out[i] = s[i];
        if (s[i] == '\0')
            return i > 0;          /* nonempty, terminated */
    }
    return false;
}

/* Apply one relocation table.  jmprel_mode pins every record to
 * R_X86_64_JUMP_SLOT (the PLT table's shape).  is_main skips
 * RELATIVE records: the kernel applied them during exec, and
 * applying again would be idempotent — skipping keeps one writer. */
static int apply_rela_table(dso_img_t *d, const elf64_phdr_t *ph,
                            unsigned phnum, bool is_main, bool jmprel_mode,
                            uint64_t table_raw, uint64_t bytes,
                            const guard_t *guards, unsigned nguards)
{
    if (bytes == 0)
        return 0;
    if ((bytes & 15ULL) != 0 || (bytes % 24) != 0)
        return 98;
    uint64_t n = bytes / 24;
    if (n > LDSO_SCOPE_BOUND)
        return 98;
    if (!range_in_readable_load(ph, phnum, table_raw, bytes))
        return 98;
    const elf64_rela_t *rela = (const elf64_rela_t *)(d->bias + table_raw);

    for (uint64_t i = 0; i < n; i++) {
        const elf64_rela_t *rel = &rela[i];
        uint64_t off = rel->r_offset;
        uint32_t rtype = ELF64_R_TYPE(rel->r_info);

        if ((off & 7ULL) != 0)
            return 98;
        if (!target_in_writable_load(ph, phnum, off))
            return 98;
        for (unsigned g = 0; g < nguards; g++) {
            if (off < guards[g].hi && guards[g].lo < off + 8)
                return 98;
        }

        if (rtype == R_X86_64_RELATIVE) {
            if (is_main)
                continue;
            *(uint64_t *)(d->bias + off) =
                d->bias + (uint64_t)rel->r_addend;
            continue;
        }
        if (rtype != R_X86_64_GLOB_DAT && rtype != R_X86_64_JUMP_SLOT)
            return 97;             /* unknown relocation type */
        if (jmprel_mode && rtype != R_X86_64_JUMP_SLOT)
            return 98;
        /* psABI table 4.2: GLOB_DAT/JUMP_SLOT write S, no addend. */
        if (rel->r_addend != 0)
            return 98;
        if (is_main && !g_main_hash_ok) {
            /* The kernel's position rule guarantees DT_HASH exists
             * whenever a symbolic record does; a miss here means the
             * two validators disagree. */
            if (!init_hash_header(d, ph, phnum))
                return 98;
            g_main_hash_ok = true;
        }
        uint32_t rsym = ELF64_R_SYM(rel->r_info);
        char namebuf[LDSO_NAME_MAX];
        if (!sym_name_of(d, rsym, namebuf, sizeof(namebuf)))
            return 98;
        uint64_t sval;
        if (!lookup_symbol(namebuf, &sval))
            return 96;
        *(uint64_t *)(d->bias + off) = sval;
    }
    return 0;
}

static int apply_image_relocs(dso_img_t *d, const elf64_phdr_t *ph,
                              unsigned phnum, bool is_main)
{
    guard_t guards[GUARD_MAX];
    unsigned nguards = build_guards(d, guards);
    int rc = apply_rela_table(d, ph, phnum, is_main, false,
                              d->rela_raw, d->rela_bytes,
                              guards, nguards);
    if (rc != 0)
        return rc;
    if (d->jmprel_bytes > 0 && !d->has_pltrel)
        return 98;                 /* PLTREL must name DT_RELA */
    return apply_rela_table(d, ph, phnum, is_main, true,
                            d->jmprel_raw, d->jmprel_bytes,
                            guards, nguards);
}

/* ── file reading ───────────────────────────────────────────── */

static bool read_full(long fd, void *buf, uint64_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        uint64_t chunk = len > (1ULL << 30) ? (1ULL << 30) : len;
        long r = sysc3(SYS_read, fd, (long)p, (long)chunk);
        if (r <= 0)
            return false;
        p += r;
        len -= (uint64_t)r;
    }
    return true;
}

/* ── DT_NEEDED library loading ──────────────────────────────── */

static uint8_t g_phdr_scratch[LDSO_PHDR_MAX * sizeof(elf64_phdr_t)];
static uint8_t g_dyn_scratch[LDSO_DYN_MAX * 16];

/* Segment permission windows for the mprotect pass. */
#define LDSO_SEG_MAX 32U

typedef struct {
    uint64_t lo, hi;
    long prot;
} segwin_t;

static long prot_for(uint32_t p_flags)
{
    long prot = PROT_READ;
    if (p_flags & PF_W)
        prot |= PROT_WRITE;
    if (p_flags & PF_X)
        prot |= PROT_EXEC;
    return prot;
}

/* Validate one library's file shape (ehdr already checked).  Fills
 * `d` (raw vaddrs) and returns the per-load geometry via the arrays.
 * Everything here runs on fd-read bytes BEFORE any mapping. */
static int validate_lib_shape(long fd, uint64_t file_size,
                              const elf64_ehdr_t *eh,
                              elf64_phdr_t *ph,
                              dso_img_t *d)
{
    if (eh->e_phentsize != sizeof(elf64_phdr_t) ||
        eh->e_phnum == 0 || eh->e_phnum > LDSO_PHDR_MAX ||
        eh->e_phoff < sizeof(elf64_ehdr_t) || (eh->e_phoff & 7ULL) != 0)
        return 98;
    uint64_t ph_bytes = (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t);
    if (eh->e_phoff > file_size || ph_bytes > file_size - eh->e_phoff)
        return 98;
    if (sysc3(SYS_lseek, fd, (long)eh->e_phoff, SEEK_SET) < 0)
        return 99;
    if (!read_full(fd, ph, ph_bytes))
        return 99;

    unsigned nloads = 0;
    uint64_t lo = 0, hi = 0;
    bool first = true;
    const elf64_phdr_t *dynp = NULL;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *l = &ph[i];
        if (l->p_type == PT_DYNAMIC) {
            if (dynp)
                return 98;
            dynp = l;
            continue;
        }
        if (l->p_type != PT_LOAD)
            continue;
        if (++nloads > LDSO_SEG_MAX)
            return 98;
        if (l->p_memsz == 0 || l->p_filesz > l->p_memsz)
            return 98;
        if ((l->p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
            return 98;
        if ((l->p_offset & (LDSO_PAGE - 1)) !=
            (l->p_vaddr & (LDSO_PAGE - 1)))
            return 98;
        uint64_t fend;
        if (add_ovf(l->p_offset, l->p_filesz, &fend) || fend > file_size)
            return 98;
        uint64_t plo = align_down4k(l->p_vaddr), phi;
        if (align_up4k(l->p_vaddr + l->p_memsz, &phi))
            return 98;
        if (!first) {
            if (plo < hi && lo < phi)
                return 98;         /* page windows overlap */
        } else {
            lo = plo;
        }
        first = false;
        if (phi > hi)
            hi = phi;
        if (lo > plo)
            lo = plo;
    }
    if (nloads == 0 || !dynp)
        return 98;

    /* Read + scan the dynamic section from the file. */
    if (dynp->p_filesz == 0 || (dynp->p_filesz & 15ULL) != 0 ||
        dynp->p_filesz > sizeof(g_dyn_scratch) ||
        dynp->p_filesz != dynp->p_memsz)
        return 98;
    if (sysc3(SYS_lseek, fd, (long)dynp->p_offset, SEEK_SET) < 0)
        return 99;
    if (!read_full(fd, g_dyn_scratch, dynp->p_filesz))
        return 99;
    if (!scan_dynamic((const elf64_dyn_t *)g_dyn_scratch, dynp->p_filesz,
                      true /* shared objects: no GNU hash */, d))
        return 98;

    /* A loaded object must be hash-lookup ready and every table it
     * declared must be covered by its own readable LOADs. */
    if (d->hash_raw == 0)
        return 98;
    d->dyn_raw = dynp->p_vaddr;
    d->dyn_bytes = dynp->p_filesz;
    if (!range_in_readable_load(ph, eh->e_phnum, d->dyn_raw, d->dyn_bytes))
        return 98;
    if (d->rela_bytes > 0 &&
        (!range_in_readable_load(ph, eh->e_phnum, d->rela_raw,
                                 d->rela_bytes) ||
         (d->rela_bytes & 15ULL) != 0))
        return 98;
    if (d->jmprel_bytes > 0 &&
        (!range_in_readable_load(ph, eh->e_phnum, d->jmprel_raw,
                                 d->jmprel_bytes) ||
         (d->jmprel_bytes & 15ULL) != 0))
        return 98;
    if (d->strsz > 0 &&
        !range_in_readable_load(ph, eh->e_phnum, d->strtab_raw, d->strsz))
        return 98;
    return 0;
}

/* Copy a DT_NEEDED string out of the image's strtab (bounded: the
 * NUL must sit inside the window) and register it in the pending
 * queue if not already loaded. */
static int queue_needed(const dso_img_t *d, unsigned *npending,
                        char pending[][LDSO_PATH_MAX])
{
    for (unsigned k = 0; k < d->nneeded; k++) {
        uint64_t off = d->needed_off[k];
        if (d->strsz == 0 || d->strtab_raw == 0 || off >= d->strsz)
            return 98;
        const char *s = (const char *)(d->bias + d->strtab_raw) + off;
        uint64_t rem = d->strsz - off;
        unsigned len = 0;
        while (len < rem && s[len] != '\0')
            len++;
        if (len >= rem || len == 0 || len >= LDSO_PATH_MAX)
            return 98;             /* unterminated or empty */
        if (s[0] != '/')
            return 98;             /* absolute paths only (PT_INTERP
                                    * contract, spec §DT_NEEDED) */
        for (unsigned e = 0; e < g_ndso; e++) {
            if (lds_streq(g_path[e], s))
                goto next;         /* already loaded */
        }
        for (unsigned e = 0; e < *npending; e++) {
            if (lds_streq(pending[e], s))
                goto next;         /* already queued */
        }
        if (*npending >= LDSO_MAX_LIBS)
            return 98;             /* dependency fan-out cap */
        lds_copy(pending[*npending], s, (uint64_t)len + 1);
        pending[*npending][len] = '\0';
        (*npending)++;
    next:;
    }
    return 0;
}

/* Load one shared object: validate, map, read, permission, parse. */
static int load_lib(const char *path, const elf64_phdr_t **out_ph,
                    unsigned *out_phnum)
{
    long fd = sysc3(SYS_open, (long)path, 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        LDSO_ERR("OPEN");
        return 95;
    }
    long fsize = sysc3(SYS_lseek, fd, 0, SEEK_END);
    if (fsize <= 0) {
        sysc3(SYS_close, fd, 0, 0);
        LDSO_ERR("ELF");
        return 98;
    }
    elf64_ehdr_t eh;
    if (sysc3(SYS_lseek, fd, 0, SEEK_SET) < 0 ||
        !read_full(fd, &eh, sizeof(eh))) {
        sysc3(SYS_close, fd, 0, 0);
        LDSO_ERR("ELF");
        return 98;
    }
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F' ||
        eh.e_ident[4] != ELFCLASS64 || eh.e_ident[5] != ELFDATA2LSB ||
        eh.e_machine != EM_X86_64 || eh.e_type != ET_DYN) {
        sysc3(SYS_close, fd, 0, 0);
        LDSO_ERR("ELF");
        return 98;
    }

    dso_img_t *d = &g_dso[g_ndso];
    lds_zero(d, sizeof(*d));
    int rc = validate_lib_shape(fd, (uint64_t)fsize, &eh,
                                (elf64_phdr_t *)g_phdr_scratch, d);
    if (rc == 95) {
        sysc3(SYS_close, fd, 0, 0);
        return rc;
    }
    if (rc != 0) {
        sysc3(SYS_close, fd, 0, 0);
        LDSO_ERR("ELF");
        return rc;
    }

    /* Map [lo, hi) anonymous RW, read the segments in, then flip
     * permissions per segment window.  bias = map_start - lo so the
     * link-time vaddrs land exactly where relocations expect them. */
    uint64_t lo = 0, map_len = 0, map_start = 0;
    {
        uint64_t minva = UINT64_MAX, maxend = 0;
        bool first = true;
        for (unsigned i = 0; i < eh.e_phnum; i++) {
            const elf64_phdr_t *l = (const elf64_phdr_t *)g_phdr_scratch + i;
            if (l->p_type != PT_LOAD || l->p_memsz == 0)
                continue;
            uint64_t plo = align_down4k(l->p_vaddr), phi;
            if (align_up4k(l->p_vaddr + l->p_memsz, &phi)) {
                sysc3(SYS_close, fd, 0, 0);
                LDSO_ERR("ELF");
                return 98;
            }
            if (first || plo < minva)
                minva = plo;
            if (first || phi > maxend)
                maxend = phi;
            first = false;
        }
        lo = minva;
        /* Both ends are page-rounded already, so map_len = hi - lo. */
        if (maxend < minva)
            map_len = 0;
        else
            map_len = maxend - minva;
        if (map_len == 0) {
            sysc3(SYS_close, fd, 0, 0);
            LDSO_ERR("ELF");
            return 98;
        }
        long ms = sysc6(SYS_mmap, 0, (long)map_len,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1L, 0);
        if (ms < 0) {
            sysc3(SYS_close, fd, 0, 0);
            LDSO_ERR("MAP");
            return 99;
        }
        map_start = (uint64_t)ms;
        d->bias = map_start - lo;
    }

    /* Read each PT_LOAD's file bytes to bias + p_vaddr; BSS beyond
     * p_filesz stays zero from the anonymous mapping. */
    segwin_t wins[LDSO_SEG_MAX];
    unsigned nwin = 0;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        const elf64_phdr_t *l = (const elf64_phdr_t *)g_phdr_scratch + i;
        if (l->p_type != PT_LOAD || l->p_memsz == 0)
            continue;
        if (l->p_filesz > 0) {
            if (sysc3(SYS_lseek, fd, (long)l->p_offset, SEEK_SET) < 0 ||
                !read_full(fd, (void *)(d->bias + l->p_vaddr),
                           l->p_filesz)) {
                sysc3(SYS_close, fd, 0, 0);
                LDSO_ERR("ELF");
                return 99;
            }
        }
        uint64_t wlo = align_down4k(l->p_vaddr), whi;
        if (align_up4k(l->p_vaddr + l->p_memsz, &whi)) {
            sysc3(SYS_close, fd, 0, 0);
            LDSO_ERR("ELF");
            return 98;
        }
        /* Two segment windows may share a page only when they agree
         * on permissions (spec §4.5). */
        for (unsigned w = 0; w < nwin; w++) {
            if (wlo < wins[w].hi && wins[w].lo < whi &&
                wins[w].prot != prot_for(l->p_flags)) {
                sysc3(SYS_close, fd, 0, 0);
                LDSO_ERR("PROT");
                return 98;
            }
        }
        wins[nwin].lo = wlo;
        wins[nwin].hi = whi;
        wins[nwin].prot = prot_for(l->p_flags);
        nwin++;
    }
    sysc3(SYS_close, fd, 0, 0);
    for (unsigned w = 0; w < nwin; w++) {
        if (sysc3(SYS_mprotect, (long)(map_start + (wins[w].lo - lo)),
                  (long)(wins[w].hi - wins[w].lo), wins[w].prot) != 0) {
            LDSO_ERR("PROT");
            return 99;
        }
    }

    /* All tables are now in memory: bound the hash header. */
    if (!init_hash_header(d, (const elf64_phdr_t *)g_phdr_scratch,
                          eh.e_phnum)) {
        LDSO_ERR("ELF");
        return 98;
    }
    *out_ph = (const elf64_phdr_t *)g_phdr_scratch;
    *out_phnum = eh.e_phnum;
    return 0;
}

/* ── T2 driver ──────────────────────────────────────────────── */

#define LDSO_FAIL(rc, tag) \
    do { \
        LDSO_ERR(tag); \
        exit_raw(rc); \
    } while (0)

static void t2_run(const uint64_t *sp)
{
    uint64_t phdr_va = auxv_get(sp, AT_PHDR, 0);
    uint64_t phent   = auxv_get(sp, AT_PHENT, 0);
    uint64_t phnum   = auxv_get(sp, AT_PHNUM, 0);
    if (phdr_va == 0 || phent != sizeof(elf64_phdr_t) ||
        phnum == 0 || phnum > LDSO_PHDR_MAX)
        LDSO_FAIL(98, "ELF");
    const elf64_phdr_t *mph = (const elf64_phdr_t *)phdr_va;

    /* Bias derivation requires PT_PHDR (compatibility clause, spec
     * §3: kernel-side PT_PHDR stays optional and AT_PHDR is still
     * published, but T2 ld.so refuses mains without it — every lld
     * product in the tree carries one). */
    const elf64_phdr_t *ptp = find_phdr(mph, (unsigned)phnum, PT_PHDR);
    if (!ptp)
        LDSO_FAIL(98, "ELF");
    dso_img_t *m = &g_dso[0];
    lds_zero(m, sizeof(*m));
    m->bias = phdr_va - ptp->p_vaddr;
    g_path[0][0] = '\0';
    g_ndso = 1;

    const elf64_phdr_t *mdyn = find_phdr(mph, (unsigned)phnum, PT_DYNAMIC);
    if (!mdyn || mdyn->p_filesz == 0 || (mdyn->p_filesz & 15ULL) != 0 ||
        mdyn->p_filesz != mdyn->p_memsz ||
        mdyn->p_filesz > LDSO_DYN_MAX * 16)
        LDSO_FAIL(98, "ELF");
    m->dyn_raw = mdyn->p_vaddr;
    m->dyn_bytes = mdyn->p_filesz;
    if (!range_in_readable_load(mph, (unsigned)phnum, m->dyn_raw,
                                m->dyn_bytes))
        LDSO_FAIL(98, "ELF");
    /* The dynamic region must also be file-backed — an anonymous or
     * uncovered window would make every table pointer below bogus. */
    if (!scan_dynamic((const elf64_dyn_t *)(m->bias + m->dyn_raw),
                      m->dyn_bytes, false /* main may carry GNU hash */, m))
        LDSO_FAIL(98, "DYN");

    /* Main-image table bounds (used for its own relocations and its
     * NEEDED strings). */
    if (m->strsz > 0 &&
        !range_in_readable_load(mph, (unsigned)phnum, m->strtab_raw,
                                m->strsz))
        LDSO_FAIL(98, "ELF");
    if (m->rela_bytes > 0 &&
        (!range_in_readable_load(mph, (unsigned)phnum, m->rela_raw,
                                 m->rela_bytes) || (m->rela_bytes & 15ULL)))
        LDSO_FAIL(98, "ELF");
    if (m->jmprel_bytes > 0 &&
        (!range_in_readable_load(mph, (unsigned)phnum, m->jmprel_raw,
                                 m->jmprel_bytes) || (m->jmprel_bytes & 15ULL)))
        LDSO_FAIL(98, "ELF");

    /* Load the dependency graph breadth-first (acceptance: one
     * library; the loop is general and dedupes by path). */
    static char pending[LDSO_MAX_LIBS][LDSO_PATH_MAX];
    unsigned npending = 0;
    {
        dso_img_t tmp = *m;
        int rc = queue_needed(&tmp, &npending, pending);
        if (rc != 0)
            LDSO_FAIL(rc, "ELF");
    }
    for (unsigned pi = 0; pi < npending; pi++) {
        if (g_ndso > LDSO_MAX_LIBS)
            LDSO_FAIL(98, "ELF");
        const elf64_phdr_t *lph;
        unsigned lphnum;
        int rc = load_lib(pending[pi], &lph, &lphnum);
        if (rc != 0)
            exit_raw((long)rc);
        lds_copy(g_path[g_ndso], pending[pi],
                 (uint64_t)sizeof(pending[pi]));
        /* The scratch phdr buffer is reused by the next load — copy
         * this library's program headers out for the reloc pass. */
        lds_copy(g_lib_phdrs[g_ndso - 1], lph,
                 (uint64_t)lphnum * sizeof(elf64_phdr_t));
        g_lib_phnum[g_ndso - 1] = lphnum;
        g_ndso++;
        dso_img_t tmp = g_dso[g_ndso - 1];
        rc = queue_needed(&tmp, &npending, pending);
        if (rc != 0)
            LDSO_FAIL(rc, "ELF");
    }

    /* Apply relocations: main first (kernel did its RELATIVE pass),
     * then libraries in load order. */
    int rc = apply_image_relocs(&g_dso[0], mph, (unsigned)phnum, true);
    if (rc == 96)
        LDSO_FAIL(rc, "SYM");
    else if (rc == 97)
        LDSO_FAIL(rc, "RELOC");
    else if (rc != 0)
        LDSO_FAIL(rc, "ELF");
    for (unsigned i = 1; i < g_ndso; i++) {
        rc = apply_image_relocs(&g_dso[i], g_lib_phdrs[i - 1],
                                g_lib_phnum[i - 1], false);
        if (rc == 96)
            LDSO_FAIL(rc, "SYM");
        else if (rc == 97)
            LDSO_FAIL(rc, "RELOC");
        else if (rc != 0)
            LDSO_FAIL(rc, "ELF");
    }
}

/* ── C entry (T1 skeleton symbol; interp_entry.S calls it) ──── */

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

    /* T2: dependencies, symbols, relocations.  Everything from here
     * on may exit(95..99); [LDSO-RELOC] is printed only once the
     * main image's world is complete. */
    t2_run(sp);

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

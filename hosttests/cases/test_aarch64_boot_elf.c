/* @os01-test
 * id: test_aarch64_boot_elf
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: nightly
 * link: aarch64-boot-elf
 * scenarios: [boot.lma.validate_elf]
 */
/* AArch64 UEFI kernel-ELF validator, two layout instances (LMA Task 2).
 *
 * The production boot/uefi/arch/aarch64/elf.c is compiled TWICE from the
 * same source: once with the default layout (0x40000000, exporting
 * aarch64_elf_validate / aarch64_page_interval) and once with the
 * relocated layout (AARCH64_BOOT_BLOCK_BASE=0x42000000, compile-time
 * renamed to aarch64_elf_validate_alt / aarch64_page_interval_alt).
 * One fixture builder emits synthetic ET_EXEC images from explicit
 * expected addresses — the test never derives addresses from the
 * production macros, so a stale default in loader.h cannot satisfy it.
 *
 * The default instance only accepts the valid default ELF. The alt
 * instance accepts its valid relocated ELF and rejects one
 * representative per invalid class: entry mismatch (the only class
 * with a dedicated error category), a 3-byte file entry (incomplete
 * instruction), a non-executable boot segment, a segment below the
 * LMA, a broken high-half VMA/PA relation, and a segment whose
 * memsz rounds past the handoff window.
 */
#include "test_framework.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../boot/uefi/arch/aarch64/loader.h"

/* The relocated instance is the same production source compiled with
 * -DAARCH64_BOOT_BLOCK_BASE=0x42000000 and renamed exports. */
int aarch64_elf_validate_alt(const void *image, uint64_t image_size,
                             uint64_t *entry_out);
int aarch64_page_interval_alt(uint64_t paddr, uint64_t memsz,
                              uint64_t *start_out, uint64_t *pages_out);

#define FIXTURE_PAGE_OFFSET UINT64_C(0xffff000000000000)
#define FIXTURE_DEFAULT_BASE UINT64_C(0x40000000)
#define FIXTURE_ALT_BASE    UINT64_C(0x42000000)

#define FIX_EHDR_SIZE 64
#define FIX_PHOFF     FIX_EHDR_SIZE
#define FIX_PHENTSIZE 56
#define FIX_PAYLOAD   0x400
#define FIX_MAX_SEGS  6
#define FIX_BUF_SIZE  (FIX_PHOFF + FIX_MAX_SEGS * 0x1000 + FIX_PAYLOAD)

struct fixture_seg {
    uint32_t flags;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
};

struct elf_fixture {
    uint64_t entry;
    struct fixture_seg segs[FIX_MAX_SEGS];
    uint16_t seg_count;
};

static void fill_valid_fixture(struct elf_fixture *f, uint64_t base)
{
    uint64_t entry = base + UINT64_C(0x80000);
    uint64_t trampoline = base + UINT64_C(0x1ff000);

    memset(f, 0, sizeof(*f));
    f->entry = entry;
    f->seg_count = 4;
    /* Identity boot segment: PF_R|PF_W|PF_X, 4-byte entry instruction
     * fully covered by p_filesz. */
    f->segs[0].flags = UINT32_C(7);
    f->segs[0].vaddr = entry;
    f->segs[0].paddr = entry;
    f->segs[0].filesz = 8;
    f->segs[0].memsz = 0x1000;
    /* Two byte-disjoint segments sharing one allocation page. */
    f->segs[1].flags = UINT32_C(5);
    f->segs[1].vaddr = FIXTURE_PAGE_OFFSET + entry + UINT64_C(0x1000);
    f->segs[1].paddr = entry + UINT64_C(0x1000);
    f->segs[1].filesz = 0x800;
    f->segs[1].memsz = 0x800;
    f->segs[2].flags = UINT32_C(6);
    f->segs[2].vaddr = FIXTURE_PAGE_OFFSET + entry + UINT64_C(0x1800);
    f->segs[2].paddr = entry + UINT64_C(0x1800);
    f->segs[2].filesz = 0x800;
    f->segs[2].memsz = 0x800;
    /* Final page abuts the trampoline page exactly (legal boundary). */
    f->segs[3].flags = UINT32_C(6);
    f->segs[3].vaddr = FIXTURE_PAGE_OFFSET + (trampoline - UINT64_C(0x1000));
    f->segs[3].paddr = trampoline - UINT64_C(0x1000);
    f->segs[3].filesz = 0x1000;
    f->segs[3].memsz = 0x1000;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static size_t build_elf(const struct elf_fixture *f, uint8_t *buf)
{
    uint16_t i;

    memset(buf, 0, FIX_BUF_SIZE);
    /* ELF64 little-endian ET_EXEC AArch64 header. */
    buf[0] = 0x7f;
    buf[1] = 'E';
    buf[2] = 'L';
    buf[3] = 'F';
    buf[4] = 2;  /* ELFCLASS64 */
    buf[5] = 1;  /* ELFDATA2LSB */
    buf[6] = 1;  /* EV_CURRENT */
    put_le16(buf + 16, 2);   /* e_type = ET_EXEC */
    put_le16(buf + 18, 183); /* e_machine = EM_AARCH64 */
    put_le32(buf + 20, 1);   /* e_version */
    put_le64(buf + 24, f->entry);
    put_le64(buf + 32, FIX_PHOFF);
    put_le16(buf + 52, FIX_EHDR_SIZE);
    put_le16(buf + 54, FIX_PHENTSIZE);
    put_le16(buf + 56, f->seg_count);
    for (i = 0; i < f->seg_count; i++) {
        uint8_t *ph = buf + FIX_PHOFF + (size_t)i * FIX_PHENTSIZE;
        uint64_t off = FIX_PAYLOAD + (uint64_t)i * 0x1000;

        put_le32(ph + 0, 1);            /* p_type = PT_LOAD */
        put_le32(ph + 4, f->segs[i].flags);
        put_le64(ph + 8, off);          /* p_offset */
        put_le64(ph + 16, f->segs[i].vaddr);
        put_le64(ph + 24, f->segs[i].paddr);
        put_le64(ph + 32, f->segs[i].filesz);
        put_le64(ph + 40, f->segs[i].memsz);
        put_le64(ph + 48, 0x1000);      /* p_align */
    }
    return FIX_PAYLOAD + (size_t)f->seg_count * 0x1000;
}

/* Two call entries, one per linked production instance. */
static int validate_default(const void *image, uint64_t size, uint64_t *entry)
{
    return aarch64_elf_validate(image, size, entry);
}

static int validate_alt(const void *image, uint64_t size, uint64_t *entry)
{
    return aarch64_elf_validate_alt(image, size, entry);
}

static void expect_valid(int (*validate)(const void *, uint64_t, uint64_t *),
                         const struct elf_fixture *f, uint64_t expect_entry)
{
    uint8_t buf[FIX_BUF_SIZE];
    size_t size = build_elf(f, buf);
    uint64_t entry = 0;

    assert_eq(0, validate(buf, (uint64_t)size, &entry));
    assert_eq(expect_entry, entry);
}

static void expect_reject(int (*validate)(const void *, uint64_t, uint64_t *),
                          const struct elf_fixture *f)
{
    uint8_t buf[FIX_BUF_SIZE];
    size_t size = build_elf(f, buf);
    uint64_t entry = 0;

    assert_true(validate(buf, (uint64_t)size, &entry) != 0);
}

static void test_default_layout_valid_elf(void)
{
    struct elf_fixture f;

    fill_valid_fixture(&f, FIXTURE_DEFAULT_BASE);
    expect_valid(validate_default, &f, FIXTURE_DEFAULT_BASE + UINT64_C(0x80000));
}

static void test_alt_layout_valid_elf(void)
{
    struct elf_fixture f;

    fill_valid_fixture(&f, FIXTURE_ALT_BASE);
    expect_valid(validate_alt, &f, FIXTURE_ALT_BASE + UINT64_C(0x80000));
}

static void test_alt_layout_rejections(void)
{
    struct elf_fixture f;
    int rc;
    uint8_t buf[FIX_BUF_SIZE];
    size_t size;
    uint64_t entry;

    /* Entry mismatch: the only class with a dedicated error category. */
    fill_valid_fixture(&f, FIXTURE_ALT_BASE);
    f.entry = FIXTURE_DEFAULT_BASE + UINT64_C(0x80000);
    size = build_elf(&f, buf);
    rc = validate_alt(buf, (uint64_t)size, &entry);
    assert_true(rc != 0);
#ifdef AARCH64_ELF_ERR_ENTRY_MISMATCH
    assert_eq(AARCH64_ELF_ERR_ENTRY_MISMATCH, rc);
#endif

    /* The 4-byte entry instruction only partly provided by the file. */
    fill_valid_fixture(&f, FIXTURE_ALT_BASE);
    f.segs[0].filesz = 3;
    expect_reject(validate_alt, &f);

    /* Boot segment without PF_X. */
    fill_valid_fixture(&f, FIXTURE_ALT_BASE);
    f.segs[0].flags = UINT32_C(6);
    expect_reject(validate_alt, &f);

    /* A second identity segment below the LMA. */
    fill_valid_fixture(&f, FIXTURE_ALT_BASE);
    f.seg_count = 5;
    f.segs[4].flags = UINT32_C(7);
    f.segs[4].vaddr = FIXTURE_ALT_BASE;
    f.segs[4].paddr = FIXTURE_ALT_BASE;
    f.segs[4].filesz = 0x100;
    f.segs[4].memsz = 0x100;
    expect_reject(validate_alt, &f);

    /* High-half VMA/PA relation broken. */
    fill_valid_fixture(&f, FIXTURE_ALT_BASE);
    f.segs[1].vaddr += UINT64_C(0x1000);
    expect_reject(validate_alt, &f);

    /* p_memsz rounds past the handoff window. */
    fill_valid_fixture(&f, FIXTURE_ALT_BASE);
    f.segs[3].memsz = 0x2000;
    expect_reject(validate_alt, &f);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_default_layout_valid_elf),
    TEST_ENTRY(test_alt_layout_valid_elf),
    TEST_ENTRY(test_alt_layout_rejections),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("AArch64 UEFI boot ELF validator");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}

/*
 * test/cases/test_elf_validate.c — ELF64 validation unit tests
 * Tests elf_validate logic without requiring QEMU or filesystem.
 */
#include "test_framework.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── ELF constants ───────────────────────────────── */
#define EI_NIDENT 16
#define EI_MAG0   0
#define EI_MAG1   1
#define EI_MAG2   2
#define EI_MAG3   3
#define EI_CLASS  4
#define EI_DATA   5

#define ELFMAG0   0x7f
#define ELFMAG1   'E'
#define ELFMAG2   'L'
#define ELFMAG3   'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1

#define EM_X86_64 0x3e
#define ET_EXEC   2

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

/* ── Simulated VFS node with read callback ────────── */
typedef struct vfs_node {
    void    *data;
    uint64_t size;
    int      id;
} vfs_node_t;

static int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buf) {
    if (offset + size > node->size) return -1;
    memcpy(buf, (uint8_t*)node->data + offset, size);
    return (int)size;
}

/* ── elf_validate (translated from kernel code) ──── */
static int elf_validate(vfs_node_t *node) {
    unsigned char ident[EI_NIDENT];
    int ret = vfs_read(node, 0, EI_NIDENT, ident);
    if (ret != EI_NIDENT) return -1;

    if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1 ||
        ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3)
        return -1;
    if (ident[EI_CLASS] != ELFCLASS64 || ident[EI_DATA] != ELFDATA2LSB)
        return -1;
    return 0;
}

/* ── Build a valid ELF64 header ───────────────────── */
static void build_valid_elf64_header(elf64_ehdr_t *hdr) {
    memset(hdr, 0, sizeof(elf64_ehdr_t));
    hdr->e_ident[EI_MAG0] = ELFMAG0;
    hdr->e_ident[EI_MAG1] = ELFMAG1;
    hdr->e_ident[EI_MAG2] = ELFMAG2;
    hdr->e_ident[EI_MAG3] = ELFMAG3;
    hdr->e_ident[EI_CLASS] = ELFCLASS64;
    hdr->e_ident[EI_DATA] = ELFDATA2LSB;
    hdr->e_type = ET_EXEC;
    hdr->e_machine = EM_X86_64;
    hdr->e_version = 1;
    hdr->e_entry = 0x401000;
    hdr->e_phoff = sizeof(elf64_ehdr_t);
    hdr->e_ehsize = sizeof(elf64_ehdr_t);
    hdr->e_phentsize = sizeof(uint64_t) * 8; /* sizeof(elf64_phdr_t) approx */
    hdr->e_phnum = 1;
}

/* ── Tests ──────────────────────────────────────────── */

TEST_FUNC(test_elf_validate_valid) {
    elf64_ehdr_t hdr;
    build_valid_elf64_header(&hdr);

    vfs_node_t node;
    node.data = &hdr;
    node.size = sizeof(hdr);

    assert_eq(elf_validate(&node), 0);
}

TEST_FUNC(test_elf_validate_bad_magic) {
    unsigned char data[16];
    memset(data, 0, sizeof(data));
    data[0] = 0x7F; data[1] = 'E'; data[2] = 'L'; data[3] = 'X'; /* wrong */
    data[4] = ELFCLASS64;
    data[5] = ELFDATA2LSB;

    vfs_node_t node;
    node.data = data;
    node.size = sizeof(data);

    assert_eq(elf_validate(&node), -1);
}

TEST_FUNC(test_elf_validate_not_64bit) {
    elf64_ehdr_t hdr;
    build_valid_elf64_header(&hdr);
    hdr.e_ident[EI_CLASS] = 1; /* ELFCLASS32 */

    vfs_node_t node;
    node.data = &hdr;
    node.size = sizeof(hdr);

    assert_eq(elf_validate(&node), -1);
}

TEST_FUNC(test_elf_validate_big_endian) {
    elf64_ehdr_t hdr;
    build_valid_elf64_header(&hdr);
    hdr.e_ident[EI_DATA] = 2; /* ELFDATA2MSB */

    vfs_node_t node;
    node.data = &hdr;
    node.size = sizeof(hdr);

    assert_eq(elf_validate(&node), -1);
}

TEST_FUNC(test_elf_validate_truncated) {
    unsigned char data[4] = {0x7F, 'E', 'L', 'F'};

    vfs_node_t node;
    node.data = data;
    node.size = 4;

    assert_eq(elf_validate(&node), -1);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_elf_validate_valid),
    TEST_ENTRY(test_elf_validate_bad_magic),
    TEST_ENTRY(test_elf_validate_not_64bit),
    TEST_ENTRY(test_elf_validate_big_endian),
    TEST_ENTRY(test_elf_validate_truncated),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

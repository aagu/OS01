#include <fs/elf.h>
#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <stdint.h>
#include <string.h>
#include <driver/serial.h>

/* Deduplication tracker: which 2MB-aligned virtual bases have been
 * mapped so far during this elf_load() call.  Up to 16 pages covers
 * any realistic small ELF. */
#define MAX_LOAD_PAGES 16

int elf_validate(vfs_node_t *node)
{
    unsigned char ident[EI_NIDENT];
    int ret = vfs_read(node, 0, EI_NIDENT, ident);
    if (ret != EI_NIDENT)
        return -1;

    if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1 ||
        ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3)
        return -1;
    if (ident[EI_CLASS] != ELFCLASS64 || ident[EI_DATA] != ELFDATA2LSB)
        return -1;
    return 0;
}

int elf_load(vfs_node_t *node, mm_t *mm, uint64_t *entry_point)
{
    elf64_ehdr_t ehdr;
    int ret;

    /* 1. Read and validate ELF header */
    ret = vfs_read(node, 0, sizeof(ehdr), &ehdr);
    if (ret != (int)sizeof(ehdr)) {
        serial_printk("elf_load: failed to read ELF header (ret=%d)\n", ret);
        return -1;
    }

    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3) {
        serial_printk("elf_load: bad ELF magic\n");
        return -1;
    }
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        serial_printk("elf_load: not 64-bit ELF\n");
        return -1;
    }
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        serial_printk("elf_load: not little-endian\n");
        return -1;
    }
    if (ehdr.e_machine != EM_X86_64) {
        serial_printk("elf_load: not x86_64 (machine=%#x)\n", ehdr.e_machine);
        return -1;
    }
    if (ehdr.e_type != ET_EXEC) {
        serial_printk("elf_load: not an executable (type=%#x)\n", ehdr.e_type);
        return -1;
    }
    if (ehdr.e_phentsize != sizeof(elf64_phdr_t)) {
        serial_printk("elf_load: bad phentsize (%u)\n", ehdr.e_phentsize);
        return -1;
    }

    *entry_point = ehdr.e_entry;

    /* 2. Page base dedup table with per-page writable-or-not tracking */
    uint64_t mapped_base[MAX_LOAD_PAGES];
    int      mapped_writable[MAX_LOAD_PAGES];
    uint64_t *mapped_pml4 = NULL; /* virtual addr of mm->pml4 for cleanup */
    int      mapped_count = 0;

    /* Resolve the PML4 virtual address for vmm_map_page (mm->pml4 is physical) */
    mapped_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)mm->pml4);

    /* 3. Walk program headers */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        ret = vfs_read(node, ehdr.e_phoff + (uint64_t)i * ehdr.e_phentsize,
                       sizeof(phdr), &phdr);
        if (ret != (int)sizeof(phdr))
            goto cleanup;

        if (phdr.p_type != PT_LOAD)
            continue;

        /* Compute 2MB-aligned page base for this segment */
        uint64_t page_base = phdr.p_vaddr & PAGE_2M_MASK;
        uint64_t page_off  = phdr.p_vaddr - page_base;

        /* Check if this 2MB page has already been mapped */
        uint64_t phys = 0;
        int      slot = -1;
        for (int j = 0; j < mapped_count; j++) {
            if (mapped_base[j] == page_base) {
                slot = j;
                break;
            }
        }

        if (slot < 0) {
            /* Allocate a fresh 2MB physical page */
            struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
            if (!pg) {
                serial_printk("elf_load: out of memory for segment %u\n", i);
                goto cleanup;
            }
            phys = pg->phy_address;

            /* Zero the entire 2MB page (handles BSS implicitly) */
            memset((void *)Phy_To_Virt(phys), 0, PAGE_2M_SIZE);

            /* Track for dedup */
            if (mapped_count < MAX_LOAD_PAGES) {
                mapped_base[mapped_count]     = page_base;
                mapped_writable[mapped_count] = (phdr.p_flags & PF_W) ? 1 : 0;
                slot = mapped_count;
                mapped_count++;
            }

            /* Map the fresh page — always RWX for user space (2MB
             * pages can't enforce per-4KB permissions). */
            vmm_map_page(mapped_pml4, phys, page_base, PAGE_USER_Page);
        }

        /* Re-read phys from the page table for the data copy below.
         * This handles both new mappings and upgrades uniformly. */
        {
            uint64_t *entry_pml4 = mapped_pml4;
            size_t l4 = (page_base >> PAGE_GDT_SHIFT) & 0x1ff;
            size_t l3 = (page_base >> PAGE_1G_SHIFT) & 0x1ff;
            size_t l2 = (page_base >> PAGE_2M_SHIFT) & 0x1ff;

            uint64_t pml4e = entry_pml4[l4];
            uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4e & PAGE_4K_MASK);
            uint64_t pml3e = pml3[l3];
            uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3e & PAGE_4K_MASK);
            phys = pml2[l2] & PAGE_2M_MASK;
        }

        /* Copy segment data from file into the physical page */
        if (phdr.p_filesz > 0) {
            uint64_t dest = (uint64_t)Phy_To_Virt(phys) + page_off;
            ret = vfs_read(node, phdr.p_offset, phdr.p_filesz, (void *)dest);
            if (ret < 0 || (uint64_t)ret != phdr.p_filesz) {
                serial_printk("elf_load: read segment %u failed (ret=%d, expected=%lu)\n",
                              i, ret, phdr.p_filesz);
                goto cleanup;
            }
        }

        /* Track mm bounds — first segment's start address = start_code */
        if (mm->start_code == 0 || phdr.p_vaddr < mm->start_code)
            mm->start_code = phdr.p_vaddr;
        uint64_t seg_end = phdr.p_vaddr + phdr.p_memsz;
        if (seg_end > mm->end_code)
            mm->end_code = seg_end;

        /* BSS (p_memsz > p_filesz) is already zero — the 2MB page was
         * zeroed when allocated, and vfs_read only wrote p_filesz bytes. */
    }

    serial_printk("elf_load: entry=%p code=[%p,%p) pages=%d\n",
                  *entry_point, mm->start_code, mm->end_code, mapped_count);
    return 0;

cleanup:
    /* Unmap and free the pages we just mapped, but do NOT free the
     * PML4 itself — the caller owns and will free it. */
    for (int j = 0; j < mapped_count; j++) {
        uintptr_t phys = vmm_unmap_page(mapped_pml4, mapped_base[j]);
        if (phys) {
            struct Page *page = Phy_to_2M_Page(phys);
            page_clean(page);
            free_pages(page, 1);
        }
    }
    return -1;
}

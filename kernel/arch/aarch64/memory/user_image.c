/* kernel/arch/aarch64/memory/user_image.c
 *
 * M4 production user image constructor.
 *
 * Builds a private, mm-owned address space out of a memory-resident
 * ET_EXEC file: one mm_t with its own root, one 4 KiB leaf per loaded
 * page plus one RW/NX stack page, and a manifest that lets the
 * teardown prove which physical page it releases.
 *
 * Construction happens on an unpublished root: the new mm is created
 * first, the leaves are mapped into it, and only a later, explicit
 * install step (see the M4 task path) publishes the root and writes
 * TTBR0_EL1.  Nothing here touches the live translation state, so a
 * failure can roll the whole image back without the task ever seeing
 * a half-installed address space.
 */

#include <arch/mmu.h>                       /* Phy_To_Virt */
#include <arch/aarch64/user_elf.h>
#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_mm.h>
#include <arch/aarch64/vmm_gate.h>
#include <errno.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <memory/user_vm.h>
#include <memory/vmm.h>
#include <sched/task.h>                     /* mm_t layout */

#if AARCH64_EL0_TEST
#include <selftest/aarch64_user_task.h>

/* One-shot allocation failure injection.  The arming names the PID
 * whose next image construction fails; it survives a parse failure, so
 * the invalid catalog entry (which never reaches the mapping loop)
 * does not consume it. */
static int g_fail_pid = -1;

void aarch64_user_image_test_arm_fail(int pid)
{
    g_fail_pid = pid;
}
#endif

/* Loaded leaves plus the single stack page.  The load-page cap is
 * independent of the stack page (M4 design §映像构造与所有权). */
#define AARCH64_USER_IMAGE_MAX_MAPS (AARCH64_ELF_MAX_PAGES + 1)

struct aarch64_user_image {
    mm_t *mm;
    uint64_t root_pa;
    uint64_t entry;
    struct {
        uint64_t va;
        uint64_t pa;
    } maps[AARCH64_USER_IMAGE_MAX_MAPS];
    unsigned map_count;
};

/* Exact semantic permission masks accepted by the M3 user-root branch:
 * RO/X, RO/NX and RW/NX.  W+X was already rejected by the segment
 * check, so a writable segment is always NX. */
static uint32_t user_image_perm(uint32_t p_flags)
{
    if (p_flags & PF_W)
        return VM_USER_RW | VM_NO_EXEC;
    if (p_flags & PF_X)
        return VM_USER_RO;
    return VM_USER_RO | VM_NO_EXEC;
}

int aarch64_user_image_load(const void *bytes, size_t size,
                             struct aarch64_user_image **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!bytes || !size)
        return -EINVAL;

    struct aarch64_elf_view view;
    int rc = aarch64_elf_view_parse(bytes, size,
                                     true /* the production image has no
                                           * TLS support yet */,
                                     &view);
    if (rc) {
#if AARCH64_EL0_TEST
        aarch64_user_task_test_note_image(false);
#endif
        return rc;
    }

    struct aarch64_user_image *image = kmalloc(sizeof(*image));
    if (!image) {
        aarch64_elf_view_release(&view);
        return -ENOMEM;
    }
    __builtin_memset(image, 0, sizeof(*image));

    rc = aarch64_user_mm_create(&image->mm);
    if (rc) {
        kfree(image);
        aarch64_elf_view_release(&view);
        return rc;
    }
    image->root_pa = (uint64_t)(uintptr_t)image->mm->pgdir;
    image->entry = view.ehdr.e_entry;

    /* Install the empty heap VMA before the first ELF leaf exists:
     * the brk window is part of the image contract, and a failure
     * here must take the same full-image rollback as a bad segment. */
    rc = user_vm_init_heap(image->mm, view.heap_base);
    if (rc)
        goto fail;

    /* The M3 entries take the direct-map root pointer; mm->pgdir holds
     * the root PA. */
    uint64_t *root = (uint64_t *)Phy_To_Virt(image->root_pa);
    const unsigned char *src = (const unsigned char *)bytes;

    for (unsigned i = 0; i < view.phnum; i++) {
        const elf64_phdr_t *ph = &view.phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;
        bool executable = (ph->p_flags & PF_X) != 0;
        uint32_t perm = user_image_perm(ph->p_flags);

        for (uint64_t off = 0; off < ph->p_memsz; off += PAGE_4K_SIZE) {
            uint64_t pa = alloc_4k_page();
            if (!pa) {
                rc = -ENOMEM;
                goto fail;
            }
            unsigned char *page = (unsigned char *)Phy_To_Virt(pa);
            __builtin_memset(page, 0, PAGE_4K_SIZE);
            if (off < ph->p_filesz) {
                size_t len = ph->p_filesz - off;
                if (len > PAGE_4K_SIZE)
                    len = PAGE_4K_SIZE;
                __builtin_memcpy(page, src + ph->p_offset + off, len);
            }
            if (executable)
                aarch64_elf_clean_code_page(page);

            int map_rc = arch_vmm_map_4k_new(root, pa, ph->p_vaddr + off, perm);
            if (map_rc) {
                free_4k_page(pa);
                rc = (map_rc == -ENOMEM) ? -ENOMEM : -ENOEXEC;
                goto fail;
            }
            image->maps[image->map_count].va = ph->p_vaddr + off;
            image->maps[image->map_count].pa = pa;
            image->map_count++;
#if AARCH64_EL0_TEST
            /* The injected failure lands right after the first leaf is
             * mapped, so the rollback has a real page to release. */
            if (g_fail_pid >= 0 && image->map_count == 1 &&
                (int)current->pid == g_fail_pid) {
                g_fail_pid = -1;
                rc = -ENOMEM;
                goto fail;
            }
#endif
        }
    }

    /* One RW/NX stack page; the page below stays unmapped as the
     * guard. */
    uint64_t stack_pa = alloc_4k_page();
    if (!stack_pa) {
        rc = -ENOMEM;
        goto fail;
    }
    __builtin_memset((unsigned char *)Phy_To_Virt(stack_pa), 0, PAGE_4K_SIZE);
    int map_rc = arch_vmm_map_4k_new(root, stack_pa, AARCH64_USER_STACK_BASE,
                                     VM_USER_RW | VM_NO_EXEC);
    if (map_rc) {
        free_4k_page(stack_pa);
        rc = (map_rc == -ENOMEM) ? -ENOMEM : -ENOEXEC;
        goto fail;
    }
    image->maps[image->map_count].va = AARCH64_USER_STACK_BASE;
    image->maps[image->map_count].pa = stack_pa;
    image->map_count++;

    aarch64_elf_sync_icache();
    aarch64_elf_view_release(&view);
    *out = image;
#if AARCH64_EL0_TEST
    aarch64_user_task_test_note_image(true);
#endif
    return 0;

fail:
    aarch64_elf_view_release(&view);
#if AARCH64_EL0_TEST
    aarch64_user_task_test_note_image(false);
#endif
    aarch64_user_image_destroy(image);
    return rc;
}

void aarch64_user_image_destroy(struct aarch64_user_image *image)
{
    if (!image)
        return;

    mm_t *mm = image->mm;
    if (mm) {
        uint64_t *root = (uint64_t *)Phy_To_Virt(image->root_pa);

        /* The root must be detached before any page is released: a
         * published or still-installed root means the leaves may be
         * reachable and freeing them would corrupt a live mapping. */
        if (aarch64_user_mm_root_busy(root)) {
            vmm_gate_violation("user image root is still busy");
            return;
        }

        /* Anonymous pages and their VMA metadata go first: they are
         * policy-owned, while the ELF/stack leaves below belong to
         * this manifest.  The root is already detached. */
        user_vm_destroy(mm);

        for (unsigned i = image->map_count; i > 0; i--) {
            uint64_t pa = 0;
            uint32_t old_vm = 0;
            int rc = arch_vmm_unmap_4k(root, image->maps[i - 1].va,
                                       &pa, &old_vm);
            if (rc || pa != image->maps[i - 1].pa) {
                vmm_gate_violation("user image leaf does not match the manifest");
                return;
            }
            free_4k_page(pa);
        }
        image->map_count = 0;

        if (aarch64_user_mm_destroy(mm) != 0)
            vmm_gate_violation("user image mm destroy refused");
    }
    kfree(image);
}

mm_t *aarch64_user_image_mm(const struct aarch64_user_image *image)
{
    return image ? image->mm : NULL;
}

uint64_t aarch64_user_image_root_pa(const struct aarch64_user_image *image)
{
    return image ? image->root_pa : 0;
}

void aarch64_user_image_init_regs(const struct aarch64_user_image *image,
                                  pt_regs_t *regs)
{
    if (!image || !regs)
        return;
    __builtin_memset(regs, 0, sizeof(*regs));
    regs->elr_el1 = image->entry;
    regs->sp_el0 = AARCH64_USER_STACK_TOP;
    regs->spsr_el1 = AARCH64_USER_TASK_PSTATE;
}

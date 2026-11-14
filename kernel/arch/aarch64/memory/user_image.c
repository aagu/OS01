/* kernel/arch/aarch64/memory/user_image.c
 *
 * M4 production user image constructor.
 *
 * Builds a private, mm-owned address space out of a memory-resident
 * ET_EXEC file: one mm_t with its own root, one 4 KiB leaf per loaded
 * page plus one RW/NX stack page.  Every leaf, ELF and stack alike,
 * is handed to the mm's resident chain through
 * user_vm_map_image_page(); the mm owns the PA and the teardown
 * releases each page exactly once after verifying the leaf's PA.
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
#include <memory/uaccess.h>
#include <memory/vmm.h>
#include <sched/task.h>                     /* mm_t layout */
#include <string.h>
#include <arch/aarch64/user_vm_policy.h>

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

struct aarch64_user_image {
    mm_t *mm;
    uint64_t root_pa;
    uint64_t entry;
    uint64_t stack_pa;
    uint64_t initial_sp;
    uint64_t argc;
    uint64_t argv_ptr;
    uint64_t envp_ptr;
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

    const unsigned char *src = (const unsigned char *)bytes;
#if AARCH64_EL0_TEST
    /* The injected failure lands right after the first ELF leaf is
     * owned by the mm, so the rollback has a real page to release. */
    unsigned elf_pages_mapped = 0;
#endif

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

            int map_rc = user_vm_map_image_page(image->mm, ph->p_vaddr + off,
                                                pa, perm);
            if (map_rc) {
                /* The mm rejected ownership: the PA is still ours. */
                free_4k_page(pa);
                rc = (map_rc == -ENOMEM) ? -ENOMEM : -ENOEXEC;
                goto fail;
            }
#if AARCH64_EL0_TEST
            elf_pages_mapped++;
            if (g_fail_pid >= 0 && elf_pages_mapped == 1 &&
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
    int map_rc = user_vm_map_image_page(image->mm, AARCH64_USER_STACK_BASE,
                                        stack_pa, VM_USER_RW | VM_NO_EXEC);
    if (map_rc) {
        free_4k_page(stack_pa);
        rc = (map_rc == -ENOMEM) ? -ENOMEM : -ENOEXEC;
        goto fail;
    }
    image->stack_pa = stack_pa;
    image->initial_sp = AARCH64_USER_STACK_TOP;

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

int aarch64_user_image_fork_shell(const struct aarch64_user_image *parent,
                                  struct aarch64_user_image **out)
{
    if (!parent || !parent->mm || !out)
        return -EINVAL;
    *out = NULL;

    struct aarch64_user_image *image = kmalloc(sizeof(*image));
    if (!image)
        return -ENOMEM;
    __builtin_memset(image, 0, sizeof(*image));

    int rc = aarch64_user_mm_create(&image->mm);
    if (rc) {
        kfree(image);
        return rc;
    }
    image->root_pa = (uint64_t)(uintptr_t)image->mm->pgdir;
    image->entry = parent->entry;
    image->initial_sp = AARCH64_USER_STACK_TOP;
    *out = image;
    return 0;
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

        /* Every user page — anonymous, ELF and stack — is owned by
         * the mm's resident chain now; one pass removes each leaf,
         * verifies its PA and frees the page exactly once. */
        user_vm_destroy(mm);

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

static int compute_stack_layout(const char *const *argv,
                                const char *const *envp,
                                size_t *out_argc, size_t *out_envc,
                                uint64_t *out_initial_sp)
{
    size_t argc = 0;
    size_t envc = 0;
    if (argv) {
        while (argv[argc]) {
            if (argc >= MAX_ARGV)
                return -E2BIG;
            argc++;
        }
    }
    if (envp) {
        while (envp[envc]) {
            if (envc >= MAX_ARGV)
                return -E2BIG;
            envc++;
        }
    }
    if (argc + envc > MAX_ARGV)
        return -E2BIG;

    const size_t max_payload = PAGE_4K_SIZE - AARCH64_USER_STACK_HEADROOM;
    size_t str_bytes = 0;
    for (size_t i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        if (len > max_payload - str_bytes)
            return -E2BIG;
        str_bytes += len;
    }
    for (size_t j = 0; j < envc; j++) {
        size_t len = strlen(envp[j]) + 1;
        if (len > max_payload - str_bytes)
            return -E2BIG;
        str_bytes += len;
    }

    size_t str_aligned = (str_bytes + 7UL) & ~7UL;
    size_t meta_bytes = (argc + envc + 3) * sizeof(uint64_t);
    if (str_aligned > max_payload || meta_bytes > max_payload - str_aligned)
        return -E2BIG;

    uint64_t unaligned_sp = AARCH64_USER_STACK_TOP - str_aligned - meta_bytes;
    uint64_t initial_sp = unaligned_sp & ~UINT64_C(0xf);
    if (initial_sp < AARCH64_USER_STACK_BASE + AARCH64_USER_STACK_HEADROOM)
        return -E2BIG;

    *out_argc = argc;
    *out_envc = envc;
    *out_initial_sp = initial_sp;
    return 0;
}

int aarch64_user_image_check_stack_args(const char *const *argv,
                                        const char *const *envp)
{
    size_t argc = 0;
    size_t envc = 0;
    uint64_t initial_sp = 0;
    return compute_stack_layout(argv, envp, &argc, &envc, &initial_sp);
}

int aarch64_user_image_setup_stack(struct aarch64_user_image *image,
                                   const char *const *argv,
                                   const char *const *envp)
{
    if (!image || !image->stack_pa)
        return -EINVAL;

    size_t argc = 0;
    size_t envc = 0;
    uint64_t initial_sp = 0;
    int rc = compute_stack_layout(argv, envp, &argc, &envc, &initial_sp);
    if (rc)
        return rc;

    uint8_t *stack_kva = (uint8_t *)Phy_To_Virt(image->stack_pa);
    uint64_t *frame = (uint64_t *)(stack_kva +
                                   (size_t)(initial_sp - AARCH64_USER_STACK_BASE));
    size_t cursor = PAGE_4K_SIZE;

    frame[0] = (uint64_t)argc;
    frame[1 + argc] = 0;
    frame[argc + 2 + envc] = 0;

    for (size_t j = envc; j > 0; j--) {
        const char *s = envp[j - 1];
        size_t len = strlen(s) + 1;
        cursor -= len;
        __builtin_memcpy(stack_kva + cursor, s, len);
        frame[argc + 2 + (j - 1)] = AARCH64_USER_STACK_BASE + cursor;
    }
    for (size_t i = argc; i > 0; i--) {
        const char *s = argv[i - 1];
        size_t len = strlen(s) + 1;
        cursor -= len;
        __builtin_memcpy(stack_kva + cursor, s, len);
        frame[1 + (i - 1)] = AARCH64_USER_STACK_BASE + cursor;
    }

    image->initial_sp = initial_sp;
    image->argc = (uint64_t)argc;
    image->argv_ptr = initial_sp + sizeof(uint64_t);
    image->envp_ptr = initial_sp + (argc + 2) * sizeof(uint64_t);
    return 0;
}

void aarch64_user_image_init_regs(const struct aarch64_user_image *image,
                                  pt_regs_t *regs)
{
    if (!image || !regs)
        return;
    __builtin_memset(regs, 0, sizeof(*regs));
    regs->elr_el1 = image->entry;
    regs->sp_el0 = image->initial_sp ? image->initial_sp : AARCH64_USER_STACK_TOP;
    regs->spsr_el1 = AARCH64_USER_TASK_PSTATE;
    regs->x0 = image->argc;
    regs->x1 = image->argv_ptr;
    regs->x2 = image->envp_ptr;
}

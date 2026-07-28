// kernel/driver/fb.c — /dev/fb framebuffer device driver
//
// Provides mmap support for user-space direct framebuffer access,
// read for metadata query, write for raw data (with surrender support),
// and ioctl for framebuffer surrender to userspace.
//
// VM_IO guards in fork_mm_copy, do_page_fault, and vma_free_all protect
// the MMIO pages from COW, demand paging, and premature freeing.

#include <kernel/fb.h>
#include <kernel/printk.h>      // Pos, frame_buffer
#include <kernel/vma.h>         // vma_t, VM_IO, VM_SHARED
#include <kernel/vmm.h>         // vmm_map_4k_page, flush_tlb, PAGE_4K_SIZE
#include <kernel/task.h>        // current
#include <kernel/pmm.h>         // Phy_To_Virt
#include <kernel/memory.h>      // PAGE_OFFSET, Virt_To_Phy, Phy_To_Virt
#include <kernel/console.h>     // console_surrender_fb
#include <driver/serial.h>      // write_serial
#include <fs/vfs.h>             // vfs_node_t, vfs_node_put
#include <fs/devfs.h>           // devfs_ops
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

// ── Surrender flag ───────────────────────────────────────────
// When true, userspace (terminal.elf) has taken over the
// framebuffer.  Kernel fb_write diverts to serial only.
static bool fb_surrendered = false;

// ── fb_read: return fb_info metadata ─────────────────────────
// offset=0  -> return fb_info struct
// offset>0  -> return 0 (EOF)
static int fb_read(struct vfs_node *node, uint64_t offset,
                   uint64_t size, void *buffer)
{
    (void)node;
    if (offset > 0 || size == 0)
        return 0;

    struct fb_info info;
    info.width  = (uint32_t)Pos.XResolution;
    info.height = (uint32_t)Pos.YResolution;
    info.stride = (uint32_t)Pos.XResolution * 4; // 32 bpp = 4 bytes/pixel
    info.bpp    = 32;
    info.format = 0;  // raw RGB (no colour space info)

    uint64_t copy_size = size < sizeof(info) ? size : sizeof(info);
    memcpy(buffer, &info, (size_t)copy_size);
    return (int)copy_size;
}

// ── fb_write: raw data to framebuffer (or serial if surrendered)
// Returns size (discard semantics -- no buffering).
static int fb_write(struct vfs_node *node, uint64_t offset,
                    uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;

    if (fb_surrendered) {
        // FB surrendered: forward to serial only
        for (uint64_t i = 0; i < size; i++)
            write_serial(((char *)buffer)[i]);
    }
    // When not surrendered, discard writes silently.
    // (Before surrender, kernel printk uses color_printk/fb directly,
    //  not the /dev/fb device -- so fb_write is a no-op in that phase.)
    return (int)size;
}

// ── fb_mmap: map framebuffer into user space ─────────────────
// Validates SHARED and bounds, then eagerly fills all PTEs
// with uncacheable MMIO mappings.  Clears vma->vm_file to prevent
// do_page_fault from attempting demand paging on MMIO pages.
//
// After this call, fork_mm_copy (VM_IO guard) will skip PTEs
// for this VMA, preserving direct MMIO access across fork.
static int fb_mmap(struct vfs_node *node, struct vma *vma_)
{
    (void)node;
    vma_t *vma = (vma_t *)vma_;

    // Must be SHARED
    if (!(vma->vm_flags & VM_SHARED))
        return -EINVAL;

    // Must not exceed framebuffer size
    uint64_t fb_size = Pos.FB_length;
    uint64_t vma_size = vma->vm_end - vma->vm_start;
    if (vma_size > fb_size)
        return -EINVAL;

    // Eagerly fill PTEs with uncacheable MMIO attributes.
    // The physical framebuffer pages start at Pos.Phy_addr.
    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    uint64_t fb_phys = (uint64_t)Pos.Phy_addr;

    // Use PAGE_USER_4K (R/W, U/S, Present) for the MMIO pages.
    // Userspace needs write access to the framebuffer.
    uint64_t page_flags = PAGE_USER_4K | PAGE_PCD | PAGE_PWT;
    // Preserve write-combining or other attributes by using the VMA's
    // page_prot if it already has PCD/PWT set, otherwise use defaults.

    for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_4K_SIZE) {
        uint64_t phys = fb_phys + (va - vma->vm_start);
        vmm_map_4k_page(user_pml4, phys, va, page_flags);
    }

    flush_tlb();

    // Safety: clear vm_file to prevent do_page_fault from calling
    // vfs_read on this VMA.  Fork will skip these PTEs (VM_IO guard),
    // so page fault on an MMIO page should never happen.
    if (vma->vm_file) {
        vfs_node_put(vma->vm_file);
        vma->vm_file = NULL;
    }

    return 0;
}

// ── fb_ioctl: device control ─────────────────────────────────
static int fb_ioctl(struct vfs_node *node, int cmd, void *arg)
{
    (void)node; (void)arg;

    switch (cmd) {
    case FBIOSURRENDER:
        // Userspace (terminal.elf) is taking over the framebuffer.
        // Stop kernel console rendering to the framebuffer, and
        // divert fb_write to serial only.
        console_surrender_fb();
        fb_surrendered = true;
        return 0;
    default:
        return -ENOTTY;
    }
}

// ── Public devfs_ops table ───────────────────────────────────
// mmap macro (uint64_t*) conflicts with the .mmap designated initializer
#undef mmap
const struct devfs_ops fb_ops = {
    .read  = fb_read,
    .write = fb_write,
    .mmap  = fb_mmap,
    .ioctl = fb_ioctl,
};
#define mmap uint64_t*

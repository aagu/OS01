// kernel/driver/fb.c — /dev/fb framebuffer device driver
//
// Provides mmap support for user-space direct framebuffer access,
// read for metadata query, write for raw data (with surrender support),
// and ioctl for framebuffer surrender to userspace.
//
// VM_IO guards in fork_mm_copy, do_page_fault, and vma_free_all protect
// the MMIO pages from COW, demand paging, and premature freeing.

#include <driver/fb.h>
#include <core/printk.h>      // Pos, frame_buffer
#include <memory/vma.h>         // vma_t, VM_IO, VM_SHARED
#include <memory/vmm.h>         // vmm_map_4k_page, flush_tlb, PAGE_4K_SIZE
#include <sched/task.h>        // current
#include <memory/pmm.h>         // Phy_To_Virt
#include <memory/memory.h>      // PAGE_OFFSET, Virt_To_Phy, Phy_To_Virt
#include <tty/console.h>     // console_surrender_fb
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
    uint64_t *user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pgdir);
    uint64_t fb_phys = (uint64_t)Pos.Phy_addr;

    // Use PAGE_USER_PTE (R/W, U/S, Present) for the MMIO pages.
    // Userspace needs write access to the framebuffer.
    uint64_t page_flags = PAGE_USER_PTE | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH;
    // Preserve write-combining or other attributes by using the VMA's
    // page_prot if it already has PCD/PWT set, otherwise use defaults.

    for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_4K_SIZE) {
        uint64_t phys = fb_phys + (va - vma->vm_start);
        vmm_map_4k_page(user_pgd, phys, va, page_flags);
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

// ── fb_get_info: snapshot live framebuffer metadata ─────────
// Used by /dev/gfx0 (kernel/driver/gfx.c) to validate view
// dimensions against the real framebuffer before allocating a
// view slot.  Reads Pos.*; does NOT acquire Pos.lock — that lock
// guards the cursor position / print state, not the fb metadata
// (which is set once at boot and never mutated afterwards).
int fb_get_info(struct fb_info *out)
{
    if (!out) return -EINVAL;
    out->width  = (uint32_t)Pos.XResolution;
    out->height = (uint32_t)Pos.YResolution;
    out->stride = (uint32_t)Pos.XResolution * 4u;
    out->bpp    = 32;
    // GFX_FORMAT_RGB32 == 0 (kernel/include/uapi/gfx.h); fb.c does
    // NOT depend on the UAPI header so we use the literal here and
    // keep this file's includes unchanged.
    out->format = 0u;
    return 0;
}

// ── fb_write_row: copy row_bytes from a kernel pointer into the fb ──
// Kernel-only helper for /dev/gfx0's per-row blit.  Validates the
// destination rectangle against the live framebuffer (XResolution,
// YResolution, FB_length) using overflow-safe subtraction, then
// memcpy's row_bytes of pixel data into Pos.FB_addr at the matching
// (x, y) offset.  No fault-tolerant copy: the caller (gfx.c) has
// already staged the row into kernel RAM via copy_from_user_ft.
//
// Returns 0 on success, -EINVAL on a NULL pixels or out-of-range
// rectangle.  Does NOT acquire Pos.lock — the caller serializes
// present per-view (the gfx device's per-row contract is one writer
// at a time per view) and the fb is MMIO without Volatile semantics
// for our use case.
int fb_write_row(uint32_t x, uint32_t y, const void *pixels,
                 uint32_t row_bytes)
{
    if (!pixels) return -EINVAL;
    if (!Pos.FB_addr) return -EINVAL;

    uint32_t fb_w = (uint32_t)Pos.XResolution;
    uint32_t fb_h = (uint32_t)Pos.YResolution;
    if (fb_w == 0 || fb_h == 0) return -EINVAL;

    // Overflow-safe rectangle checks (subtraction form, same
    // discipline as gfx_ioctl_create_view):
    //   x <= fb_w && row_bytes/4 <= fb_w - x
    //   y <  fb_h
    //   byte_offset + row_bytes <= Pos.FB_length
    if (x > fb_w) return -EINVAL;
    if (row_bytes / 4u > fb_w - x) return -EINVAL;
    if (y >= fb_h) return -EINVAL;
    uint64_t byte_offset = (uint64_t)y * (uint64_t)fb_w * 4u
                           + (uint64_t)x * 4u;
    if (byte_offset + (uint64_t)row_bytes > Pos.FB_length) return -EINVAL;

    uint8_t *dst = (uint8_t *)Pos.FB_addr + byte_offset;
    memcpy(dst, pixels, row_bytes);
    return 0;
}

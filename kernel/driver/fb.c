// kernel/driver/fb.c — /dev/fb framebuffer device driver
//
// Provides mmap support for user-space direct framebuffer access,
// read for metadata query, write for raw data (with surrender support),
// and ioctl for framebuffer surrender to userspace.
//
// VMA_IO guards in fork_mm_copy, do_page_fault, and vma_free_all protect
// the MMIO pages from COW, demand paging, and premature freeing.

#include <driver/fb.h>
#include <core/printk.h>      // Pos, frame_buffer
#include <memory/vma.h>         // vma_t, VMA_IO, VMA_SHARED
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)         // vmm_map_4k_page, flush_tlb, PAGE_4K_SIZE
#include <sched/task.h>        // current
#include <memory/pmm.h>         // Phy_To_Virt
#include <memory/memory.h>      // PAGE_OFFSET, Virt_To_Phy, Phy_To_Virt
#include <tty/console.h>     // console_surrender_fb
#include <driver/serial.h>      // write_serial
#include <fs/vfs.h>             // vfs_node_t, vfs_node_put
#include <fs/devfs.h>           // devfs_ops
#include <memory/uaccess.h>     // syscall_check_user_range, copy_*_ft
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
    fb_snapshot_t snap;
    int rc = fb_snapshot_read(&snap);
    if (rc < 0) {
        return rc;
    }
    if (snap.state.info.width > 0) {
        info = snap.state.info;
    } else {
        info.width  = (uint32_t)Pos.XResolution;
        info.height = (uint32_t)Pos.YResolution;
        info.stride = (uint32_t)Pos.XResolution * 4; // 32 bpp = 4 bytes/pixel
        info.bpp    = 32;
        info.format = 0;  // raw RGB (no colour space info)
    }

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

    bool surrendered;
    uint64_t flags = spin_lock_irqsave(&Pos.lock);
    surrendered = fb_surrendered;
    spin_unlock_irqrestore(&Pos.lock, flags);

    if (surrendered) {
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
// Validates SHARED and bounds under control mutex, then eagerly fills
// PTEs with uncacheable MMIO mappings. If an intermediate mapping fails,
// rollbacks installed PTEs, syncs TLB, and sets sticky if unmap failed.
static int fb_mmap(struct vfs_node *node, struct vma *vma_)
{
    (void)node;
    vma_t *vma = (vma_t *)vma_;

    fb_control_lock();

    fb_snapshot_t snap;
    int rc = fb_snapshot_read(&snap);
    if (rc < 0) {
        fb_control_unlock();
        return rc;
    }

    // Must be SHARED
    if (!(vma->vm_flags & VMA_SHARED)) {
        fb_control_unlock();
        return -EINVAL;
    }

    // Must not exceed mapped framebuffer size
    uint64_t vma_size = vma->vm_end - vma->vm_start;
    if (snap.mapped_size == 0 || vma_size > snap.mapped_size) {
        fb_control_unlock();
        return -EINVAL;
    }

    // Eagerly fill PTEs with uncacheable MMIO attributes.
    // The physical framebuffer pages start at Pos.Phy_addr.
    uint64_t *user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pgdir);
    uint64_t fb_phys = (uint64_t)Pos.Phy_addr;

    uint64_t page_flags = PAGE_USER_PTE | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH;

    uint64_t installed_end = vma->vm_start;
    bool map_failed = false;

    for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_4K_SIZE) {
        uint64_t phys = fb_phys + (va - vma->vm_start);
        int mrc = vmm_map_4k_page(user_pgd, phys, va, page_flags);
        if (mrc < 0) {
            map_failed = true;
            break;
        }
        installed_end = va + PAGE_4K_SIZE;
    }

    if (map_failed) {
        for (uint64_t va = vma->vm_start; va < installed_end; va += PAGE_4K_SIZE) {
            vmm_unmap_4k_page(user_pgd, va);
        }
        flush_tlb();
        bool has_residual = false;
        for (uint64_t va = vma->vm_start; va < installed_end; va += PAGE_4K_SIZE) {
            if (arch_vmm_query_4k(user_pgd, va, NULL, NULL) == 0) {
                has_residual = true;
                break;
            }
        }
        if (has_residual) {
            // Cannot reliably unmap: mark sticky for safety
            fb_mark_raw_mmap_seen();
        }
        fb_control_unlock();
        return -ENOMEM;
    }

    flush_tlb();
    fb_mark_raw_mmap_seen();

    // Safety: clear vm_file to prevent do_page_fault from calling
    // vfs_read on this VMA.  Fork will skip these PTEs (VMA_IO guard),
    // so page fault on an MMIO page should never happen.
    if (vma->vm_file) {
        vfs_node_put(vma->vm_file);
        vma->vm_file = NULL;
    }

    fb_control_unlock();
    return 0;
}

// ── fb_ioctl: device control ─────────────────────────────────
static int fb_ioctl(struct vfs_node *node, int cmd, void *arg)
{
    (void)node;

    switch (cmd) {
    case FBIOSURRENDER:
        // Userspace (terminal.elf) is taking over the framebuffer.
        // Stop kernel console rendering to the framebuffer, and
        // divert fb_write to serial only.
        console_surrender_fb();
        {
            uint64_t flags = spin_lock_irqsave(&Pos.lock);
            fb_surrendered = true;
            spin_unlock_irqrestore(&Pos.lock, flags);
        }
        return 0;

    case FBIOGET_CURR_MODE: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(struct fb_info), true))
            return -EFAULT;
        struct fb_info kinfo;
        int rc = fb_get_info(&kinfo);
        if (rc < 0) return rc;
        if (copy_to_user_ft(arg, &kinfo, sizeof(kinfo)) < 0)
            return -EFAULT;
        return 0;
    }

    case FBIOGET_STATE: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(struct fb_state), true))
            return -EFAULT;
        struct fb_state kstate;
        int rc = fb_get_state(&kstate);
        if (rc < 0) return rc;
        if (copy_to_user_ft(arg, &kstate, sizeof(kstate)) < 0)
            return -EFAULT;
        return 0;
    }

    case FBIOGET_MODES: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(struct fb_modes_req), true))
            return -EFAULT;
        uint32_t capacity = 0;
        if (copy_from_user_ft(&capacity, &((struct fb_modes_req *)arg)->capacity, sizeof(uint32_t)) < 0)
            return -EFAULT;
        struct fb_modes_req kmodes;
        int rc = fb_get_modes(capacity, &kmodes);
        if (rc < 0) return rc;
        if (copy_to_user_ft(arg, &kmodes, sizeof(kmodes)) < 0)
            return -EFAULT;
        return 0;
    }

    case FBIOSET_MODE: {
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(struct fb_set_mode_req), false))
            return -EFAULT;
        struct fb_set_mode_req kreq;
        if (copy_from_user_ft(&kreq, arg, sizeof(kreq)) < 0)
            return -EFAULT;
        return fb_set_mode(&kreq);
    }

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
int fb_get_info(struct fb_info *out)
{
    if (!out) return -EINVAL;
    fb_snapshot_t snap;
    int rc = fb_snapshot_read(&snap);
    if (rc < 0) {
        return rc;
    }
    if (snap.state.info.width > 0) {
        *out = snap.state.info;
        return 0;
    }
    out->width  = (uint32_t)Pos.XResolution;
    out->height = (uint32_t)Pos.YResolution;
    out->stride = (uint32_t)Pos.XResolution * 4u;
    out->bpp    = 32;
    out->format = 0u;
    return 0;
}

// ── fb_write_row_leased: copy row under an active lease ─────
int fb_write_row_leased(const fb_lease_t *lease, uint32_t x, uint32_t y,
                        const void *pixels, uint32_t bytes)
{
    if (!lease || !lease->held) return -EINVAL;
    if (!pixels) return -EINVAL;
    if (!lease->snapshot.addr) return -EINVAL;

    uint32_t fb_w = lease->snapshot.state.info.width;
    uint32_t fb_h = lease->snapshot.state.info.height;
    if (fb_w == 0 || fb_h == 0) return -EINVAL;

    // Overflow-safe rectangle checks:
    if (x > fb_w) return -EINVAL;
    if (bytes / 4u > fb_w - x) return -EINVAL;
    if (y >= fb_h) return -EINVAL;

    uint64_t stride = (uint64_t)lease->snapshot.state.info.stride;
    if (stride == 0) stride = (uint64_t)fb_w * 4u;

    uint64_t byte_offset = (uint64_t)y * stride + (uint64_t)x * 4u;
    if (byte_offset + (uint64_t)bytes > lease->snapshot.mapped_size) return -EINVAL;

    uint8_t *dst = (uint8_t *)lease->snapshot.addr + byte_offset;
    memcpy(dst, pixels, bytes);
    return 0;
}

// ── fb_write_row: single-lease wrapper for legacy callers ───
int fb_write_row(uint32_t x, uint32_t y, const void *pixels,
                 uint32_t row_bytes)
{
    fb_lease_t lease;
    int rc = fb_writer_begin(&lease, 0);
    if (rc < 0) return rc;

    rc = fb_write_row_leased(&lease, x, y, pixels, row_bytes);
    fb_writer_end(&lease);
    return rc;
}


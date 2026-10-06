# AArch64 kernel-side color_printk support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable `color_printk` to render to the framebuffer on aarch64 after `arch_vmm_init()`, with no change in behavior on x86_64 and no regression on any existing test suite.

**Architecture:** Split `frame_buffer_init` / `frame_buffer_early_init` into per-arch translation units; add a NULL guard to `putchark`; populate `Pos` from `boot_context->graphics` in a new `aarch64_boot_fb_init` step inside `aarch64_main` (after `arch_vmm_init()`); map the FB physical address with `aarch64_pt_map_2m_block` + `aarch64_pt_map_4k_ext` at `AARCH64_FB_VIRT_BASE` (0xffff900000000000) using `KERNEL_RW | DEVICE` permissions; delete `kernel/arch/aarch64/runtime/printk_stub.c` so the canonical `kernel/core/printk.c::color_printk` is the only `color_printk` symbol in the aarch64 build.

**Tech Stack:** aarch64 EL1 page-table primitives (`kernel/arch/aarch64/memory/page_table.c`), existing `kernel/core/printk.c` (color_printk / putchark), QEMU `ramfb` device + edk2 `RamfbDxe` UEFI GOP, `kernel/include/core/bootinfo.h` `BOOT_CONTEXT_HAS_FRAMEBUFFER` flag.

**Spec:** `docs/superpowers/specs/2026-10-06-aarch64-color-printk-design.md`

## Global Constraints

- aarch64 build (`PROFILE=aarch64-clang`) and x86_64 build (`PROFILE=x86_64-clang`) must both link cleanly at every task boundary.
- x86_64 behavior is byte-identical: the existing `frame_buffer_init` / `frame_buffer_early_init` bodies move verbatim into the per-arch TU; no algorithmic change.
- All failure paths (missing framebuffer, mapping errors, OOM) are **non-fatal**; PL011 serial remains the canonical kernel boot console on aarch64.
- `color_printk` callers on aarch64 (4 call sites in `kernel/memory/slab.c`) must keep compiling without modification — the canonical symbol from `kernel/core/printk.c` must replace the stub.
- `Pos.lock` must be initialized before any `color_printk` call on aarch64 (x86_64 does this in `x86_64_boot_early`).
- QEMU ramfb default is 32 bpp BGRA; `putchark` already hardcodes 32 bpp via `uint32_t *FB_addr`. Do not add BPP detection.

## Review Focus

Inputs/failure modes the spec implies but no task's tests exercise:

1. **PA of ramfb collides with an existing aarch64 mapping** — `kernel_map`'s M1 root may already hold a Normal-cacheable direct-map entry for the same PA at `ARCH_PAGE_OFFSET + PA`. After our Device mapping is installed, color_printk writes must go through the Device mapping. Owner: Task 3 (aarch64 frame_buffer_init must verify Device mapping is the active one — already covered by using `aarch64_pt_map_2m_block` with `AARCH64_PT_DEVICE`).
2. **aarch64 SMP visibility** — `Pos` is a single global; APs must see the same `Pos` after SMP bring-up. Owner: Task 4 (call site is BEFORE `smp_boot_aps()` so BSP-only init, APs read-only via cache coherence).
3. **`spin_init(&Pos.lock)` not done on aarch64** — the existing x86_64 path calls it in `x86_64_boot_early` (`kernel/arch/x86_64/platform/boot.c:82`); aarch64 has no equivalent. Owner: Task 4 (aarch64_boot_fb_init must `spin_init` before any `color_printk`).
4. **T2 (no ramfb) integration test is the NULL-guard proof** — if `Pos.FB_addr` is NULL when `color_printk` is called, the kernel must not SIGSEGV. Owner: Task 2's manual test in the spirit of the test (no dedicated hosttest for the 3-line guard; rely on T6 integration run).
5. **`vsprintf` lock interaction** — `color_printk` calls `vsprintf(buf_color, ...)` while holding `Pos.lock`; with NULL guard, lock is still acquired/released. Make sure `spin_lock`/`spin_unlock` works on a NULL-FB path. Owner: Task 2 (guard placed inside `putchark`, BEFORE the pixel write — so `spin_lock` in `color_printk` is unaffected).

---

## File Structure

Files created in this plan:

| Path | Purpose |
|------|---------|
| `kernel/arch/x86_64/runtime/printk_fb.c` | x86_64 impl of `frame_buffer_init` + `frame_buffer_early_init` (extracted verbatim from current `kernel/core/printk.c`) |
| `kernel/arch/x86_64/runtime/` | Directory (didn't exist before) |
| `kernel/arch/aarch64/runtime/printk_fb.c` | aarch64 impl: `frame_buffer_init` uses `aarch64_pt_map_2m_block` + `aarch64_pt_map_4k_ext`; `frame_buffer_early_init` is empty stub |

Files modified in this plan:

| Path | Change |
|------|--------|
| `kernel/core/printk.c` | Remove `frame_buffer_init` / `frame_buffer_early_init` bodies; add `if (!Pos.FB_addr) return;` guard at top of `putchark` and `putchar_at` |
| `kernel/include/arch/aarch64/page_table.h` | Add `#define AARCH64_FB_VIRT_BASE UINT64_C(0xffff900000000000)` |
| `kernel/arch/aarch64/boot/main.c` | Add `aarch64_boot_fb_init(const struct boot_context *)` helper; call it after `arch_vmm_init()` (after line 458's `if (rc) { ... }` block) |

Files deleted in this plan:

| Path | Reason |
|------|--------|
| `kernel/arch/aarch64/runtime/printk_stub.c` | Replaced by canonical `color_printk` from `kernel/core/printk.c`; the NULL-FB case is handled by `putchark` guard |

No Makefile changes — `kernel/Makefile`'s per-arch wildcard `arch/$(ARCH)/*.c` (verified by checking `kernel/arch/x86_64/memory/boot.c` already auto-included) picks up the new TUs; deletion of `printk_stub.c` is automatic.

---

## Task 1: Extract x86_64 frame_buffer_init / frame_buffer_early_init to per-arch TU

**Files:**
- Create: `kernel/arch/x86_64/runtime/printk_fb.c`
- Create: `kernel/arch/x86_64/runtime/` (via `mkdir -p`)
- Modify: `kernel/core/printk.c` (remove `frame_buffer_init` and `frame_buffer_early_init` definitions; keep `color_printk`, `putchark`, `putchar_at`, `serial_printk` unchanged)

**Interfaces:**
- Consumes: existing symbols `Pos` (defined in `kernel/core/printk.c`), `vmm_map_page` / `kernel_map` / `tlb_shootdown` (from `kernel/memory/vmm.h`), `flush_tlb` (from `kernel/memory/memory.h` or arch-specific header), `PAGE_*` flags (from `arch/x86_64/pte.h`), `VIRT_FRAMEBUFFER_EARLY` / `VIRT_FRAMEBUFFER_OFFSET` (from `core/printk.h`)
- Produces: definitions of `frame_buffer_init` and `frame_buffer_early_init` for the x86_64 build. The signatures are already declared in `kernel/include/core/printk.h:54-55`.

- [ ] **Step 1: Create the runtime directory**

Run: `mkdir -p /home/aagu/OS01/kernel/arch/x86_64/runtime`
Expected: directory created; no error.

- [ ] **Step 2: Read current printk.c frame_buffer_init / frame_buffer_early_init bodies**

Run: `Read /home/aagu/OS01/kernel/core/printk.c` (lines 169–197)

Expected output (verbatim, must move unchanged):
```c
// Early framebuffer map via direct PDE writes, before PMM/VMM are available.
// Uses VIRT_FRAMEBUFFER_EARLY (within PUD[0]) for simple setup.
void frame_buffer_early_init()
{
    uint64_t *pmd = (uint64_t *)0xffff800000103000;
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        size_t level2 = (size_t)((VIRT_FRAMEBUFFER_EARLY + i) >> PAGE_2M_SHIFT) & 0x1FF;
        pmd[level2] = (((uint64_t)Pos.Phy_addr + i) & PAGE_2M_MASK)
            | (PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_EARLY;
    flush_tlb();
}

// Permanent framebuffer map via vmm_map_page, after PMM/VMM are available.
// Remaps to VIRT_FRAMEBUFFER_OFFSET in a separate PGD entry that never
// overlaps with the physical RAM direct mapping regardless of QEMU -m size.
void frame_buffer_init()
{
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        vmm_map_page(kernel_map, i + (uint64_t)Pos.Phy_addr,
            VIRT_FRAMEBUFFER_OFFSET + i,
            PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_OFFSET;
    tlb_shootdown();
}
```

- [ ] **Step 3: Write `kernel/arch/x86_64/runtime/printk_fb.c` with verbatim bodies**

Write to `/home/aagu/OS01/kernel/arch/x86_64/runtime/printk_fb.c`:
```c
// kernel/arch/x86_64/runtime/printk_fb.c — x86_64 framebuffer mapping
// implementations, extracted from kernel/core/printk.c as part of the
// aarch64 color_printk enable (spec 2026-10-06). Bodies are byte-identical
// to the original; this file's only purpose is to host the arch-specific
// MMIO mapping code so the aarch64 build can supply its own variant.

#include <core/printk.h>            /* Pos, VIRT_FRAMEBUFFER_EARLY,
                                     VIRT_FRAMEBUFFER_OFFSET               */
#include <memory/memory.h>         /* flush_tlb                              */
#include <memory/vmm.h>            /* vmm_map_page, kernel_map, tlb_shootdown */
#include <arch/x86_64/pte.h>       /* PAGE_* x86 hardware PTE bits           */

// Early framebuffer map via direct PDE writes, before PMM/VMM are available.
// Uses VIRT_FRAMEBUFFER_EARLY (within PUD[0]) for simple setup.
void frame_buffer_early_init()
{
    uint64_t *pmd = (uint64_t *)0xffff800000103000;
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        size_t level2 = (size_t)((VIRT_FRAMEBUFFER_EARLY + i) >> PAGE_2M_SHIFT) & 0x1FF;
        pmd[level2] = (((uint64_t)Pos.Phy_addr + i) & PAGE_2M_MASK)
            | (PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_EARLY;
    flush_tlb();
}

// Permanent framebuffer map via vmm_map_page, after PMM/VMM are available.
// Remaps to VIRT_FRAMEBUFFER_OFFSET in a separate PGD entry that never
// overlaps with the physical RAM direct mapping regardless of QEMU -m size.
void frame_buffer_init()
{
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        vmm_map_page(kernel_map, i + (uint64_t)Pos.Phy_addr,
            VIRT_FRAMEBUFFER_OFFSET + i,
            PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_OFFSET;
    tlb_shootdown();
}
```

- [ ] **Step 4: Remove the bodies from `kernel/core/printk.c`**

Edit `kernel/core/printk.c`:
- Find the line `// Early framebuffer map via direct PDE writes, before PMM/VMM are available.`
- Delete lines from that comment through the end of the `frame_buffer_init` function (lines 169–197 in the current file).
- Verify the file ends cleanly with `void serial_printk(...)` as the last definition.
- Verify the top-of-file `#include <arch/x86_64/pte.h>` can be removed (no longer used by anything in printk.c after extraction). If any other code in printk.c still references x86 PTE bits, keep it. The `PAGE_*` flags were only used by the extracted bodies.

Run: `grep -n "PAGE_KERNEL_PMD\|PAGE_WRITE_THROUGH\|PAGE_CACHE_DISABLE" /home/aagu/OS01/kernel/core/printk.c`
Expected: no matches (all uses were in the extracted bodies).

- [ ] **Step 5: Build x86_64 and verify link**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang kernel.bin 2>&1 | tee /tmp/x86-build.log
```

Expected: kernel.bin produced; no linker errors. The `frame_buffer_init` / `frame_buffer_early_init` symbols now resolve from `kernel/arch/x86_64/runtime/printk_fb.c`.

If the link fails with `undefined reference to frame_buffer_init` or `frame_buffer_early_init`: the per-arch wildcard in `mk/components/kernel.mk` is not picking up `kernel/arch/x86_64/runtime/printk_fb.c`. Investigate the kernel source glob; fix and re-run.

- [ ] **Step 6: Build aarch64 to confirm printk.c still self-contained**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi 2>&1 | tee /tmp/aarch64-build.log
```

Expected: aarch64-uefi artifact produced; aarch64 still has its own `frame_buffer_init`/`early_init` from `kernel/arch/aarch64/runtime/printk_stub.c`... wait — printk_stub.c does NOT define `frame_buffer_init`. The aarch64 build currently DOESN'T define `frame_buffer_init` at all.

This is the pre-existing state: aarch64 never had `frame_buffer_init`. The `core/printk.h` declaration has been there unused on aarch64 for a long time. Confirm by running aarch64 build before this change, then after — both must produce the aarch64-uefi artifact with no new errors.

Run after this Task: `tail -20 /tmp/aarch64-build.log` — must show successful aarch64 build.

If aarch64 build breaks because `frame_buffer_init` is referenced somewhere on aarch64: stop and investigate. (Hypothesis: nothing on aarch64 references it today; this Task is behavior-preserving.)

- [ ] **Step 7: Run x86_64 regression — existing test-static must pass**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test-static 2>&1 | tee /tmp/x86-test.log
```

Expected: all audits pass. `test-static` covers validate-kernel, runtime-link, syscall-boundary, kernel-layout, stack-canary, header-object, stack-frame, driver-model-boundary, test-user-canary. (See `mk/components/run.mk:727-744`.)

If any audit fails: the body move broke x86_64 — revert via `git checkout -- kernel/core/printk.c` and re-investigate.

- [ ] **Step 8: Commit**

Run:
```bash
cd /home/aagu/OS01
git add kernel/core/printk.c kernel/arch/x86_64/runtime/printk_fb.c
git commit -m "refactor(printk): extract x86_64 frame_buffer_init to per-arch TU

Move the x86_64 frame_buffer_init and frame_buffer_early_init
implementations from kernel/core/printk.c to
kernel/arch/x86_64/runtime/printk_fb.c. Bodies are byte-identical.

This is the foundation for the aarch64 color_printk enable (spec
2026-10-06): each arch now provides its own MMIO mapping implementation,
and kernel/core/printk.c stays arch-neutral.

x86_64 behavior is preserved verbatim. The extern declarations in
kernel/include/core/printk.h are unchanged (already present at lines
54-55).

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 2: Add NULL guard in putchark / putchar_at

**Files:**
- Modify: `kernel/core/printk.c` (add `if (!Pos.FB_addr) return;` to `putchark` and `putchar_at`)

**Interfaces:**
- Consumes: existing `Pos.FB_addr` global
- Produces: `putchark` and `putchar_at` become no-ops when `Pos.FB_addr == NULL`. `color_printk`'s `spin_lock(&Pos.lock)` is unaffected (lock is acquired before any `putchark` call).

- [ ] **Step 1: Read current putchark and putchar_at bodies**

Run: `Read /home/aagu/OS01/kernel/core/printk.c` (lines 28–86 in the current file)

Expected output (start of putchark):
```c
void putchark(unsigned int FRcolor,unsigned int BKcolor,unsigned char c)
{
    uint32_t i = 0,j = 0;
	uint32_t * addr = NULL;
	...
```

- [ ] **Step 2: Add NULL guard at the top of putchark**

Edit `kernel/core/printk.c::putchark`. Insert immediately after the opening brace:
```c
void putchark(unsigned int FRcolor,unsigned int BKcolor,unsigned char c)
{
+    /* NULL-safe: when no framebuffer is configured (aarch64 boot path
+     * without ramfb, or early-boot before Pos is populated), skip the
+     * pixel write entirely. color_printk's outer spin_lock still
+     * serializes per-char against concurrent IRQ-context callers. */
+    if (!Pos.FB_addr) return;
+
    uint32_t i = 0,j = 0;
    ...
```

- [ ] **Step 3: Add NULL guard at the top of putchar_at**

Edit `kernel/core/printk.c::putchar_at`. Insert immediately after the opening brace:
```c
void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c)
{
+    if (!Pos.FB_addr) return;
+
    int i = 0, j = 0;
    ...
```

- [ ] **Step 4: Rebuild x86_64 — guard must be a no-op for the existing path**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang kernel.bin 2>&1 | tee /tmp/x86-build2.log
```

- [ ] **Step 5: Run x86_64 selftest image to verify color_printk still renders**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test-kernel-selftest 2>&1 | tee /tmp/x86-selftest.log
```

Expected: `[selftest] running built-in tests...` marker, `N total: N passed, 0 failed`, `[selftest] done`. No crash from NULL guard (x86_64 always sets `Pos.FB_addr` before any `color_printk` call, so the guard never fires on x86_64).

- [ ] **Step 6: Commit**

Run:
```bash
cd /home/aagu/OS01
git add kernel/core/printk.c
git commit -m "refactor(printk): NULL-safe putchark/putchar_at for missing FB

Add an early return when Pos.FB_addr is NULL. On aarch64 boot paths
without -device ramfb (or before arch_vmm_init), color_printk callers
would otherwise SIGSEGV trying to dereference Pos.FB_addr.

Behavior on x86_64 is unchanged: x86_64_boot_early always populates
Pos before any color_printk call, so the guard never fires.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 3: Add aarch64 frame_buffer_init + empty frame_buffer_early_init stub + VA constant

**Files:**
- Create: `kernel/arch/aarch64/runtime/printk_fb.c`
- Modify: `kernel/include/arch/aarch64/page_table.h` (add `AARCH64_FB_VIRT_BASE`)

**Interfaces:**
- Consumes: `Pos` global (from `kernel/core/printk.c`), `aarch64_pt_map_2m_block` / `aarch64_pt_map_4k_ext` (from `kernel/arch/aarch64/memory/page_table.c`), `AARCH64_PT_KERNEL_RW` / `AARCH64_PT_DEVICE` (from `kernel/include/arch/aarch64/page_table.h`)
- Produces: `frame_buffer_init` definition that maps `Pos.Phy_addr..Pos.Phy_addr+Pos.FB_length` to `AARCH64_FB_VIRT_BASE` with `KERNEL_RW | DEVICE`; `frame_buffer_early_init` empty stub (aarch64 never needs early init — PL011 covers pre-VMM phase)

- [ ] **Step 1: Add AARCH64_FB_VIRT_BASE constant**

Edit `kernel/include/arch/aarch64/page_table.h`. After the existing `#define AARCH64_PT_SELFTEST_VA UINT64_C(0xffff800000000000)` block (line 36 area), add:
```c
/* Framebuffer mapping VA on aarch64. Distinct from AARCH64_PT_SELFTEST_VA
 * (0xffff800000000000) and below ARCH_PAGE_OFFSET (the direct-map region).
 * Used by kernel/arch/aarch64/runtime/printk_fb.c::frame_buffer_init. */
#define AARCH64_FB_VIRT_BASE UINT64_C(0xffff900000000000)
```

Verify the file still compiles by checking the syntax is intact. Read back the modified section to confirm.

- [ ] **Step 2: Write `kernel/arch/aarch64/runtime/printk_fb.c`**

Write to `/home/aagu/OS01/kernel/arch/aarch64/runtime/printk_fb.c`:
```c
// kernel/arch/aarch64/runtime/printk_fb.c — aarch64 framebuffer mapping
// implementation (spec 2026-10-06).
//
// Companion to kernel/core/printk.c::color_printk: this TU provides
// the arch-specific MMIO mapping that color_printk/putchark then writes
// through. The split is because the x86_64 path uses x86 PTE bits and
// a direct-PDE early-init trick (see kernel/arch/x86_64/runtime/printk_fb.c);
// aarch64 uses aarch64_pt_map_* with the KERNEL_RW | DEVICE permission
// word, which is fundamentally different.
//
// QEMU virt with -device ramfb (and edk2 RamfbDxe) places the framebuffer
// at a QEMU-assigned MMIO physical address that boot/uefi reports via
// boot_context->graphics.FrameBufferBase. After this TU maps that range
// at AARCH64_FB_VIRT_BASE with Device attribute, color_printk writes to
// Pos.FB_addr land in MMIO without cache-coherence surprises.
//
// No frame_buffer_early_init variant is needed: PL011 serial is the
// canonical aarch64 kernel boot console, and color_printk is only enabled
// after arch_vmm_init() (see aarch64_boot_fb_init in
// kernel/arch/aarch64/boot/main.c).

#include <core/printk.h>                    /* Pos, frame_buffer_init decl */
#include <arch/aarch64/page_table.h>        /* aarch64_pt_map_2m_block,
                                               aarch64_pt_map_4k_ext,
                                               AARCH64_PT_KERNEL_RW,
                                               AARCH64_PT_DEVICE,
                                               AARCH64_FB_VIRT_BASE        */
#include <arch/aarch64/boot_log.h>          /* kputs for non-fatal logs    */

#include <stdint.h>

/* aarch64 has no early-init variant. The x86_64 path calls this between
 * booting and booting_vmm; the aarch64 path keeps PL011 as the boot
 * console, so this body is intentionally empty. */
void frame_buffer_early_init(void)
{
    /* no-op: see file-level comment */
}

void frame_buffer_init(void)
{
    uint64_t pa = (uint64_t)Pos.Phy_addr;
    uint64_t len = Pos.FB_length;
    uint64_t va = AARCH64_FB_VIRT_BASE;
    uint32_t perm = AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE;

    if (pa == 0 || len == 0) {
        kputs("[fb] no-op: Pos.Phy_addr or Pos.FB_length is zero\n");
        Pos.FB_addr = NULL;
        return;
    }

    /* Walk the [pa, pa+len) range. Use 2 MiB blocks where alignment
     * permits; fall back to 4 KiB leaves for the head (PA not 2 MiB
     * aligned) and tail (size not a multiple of 2 MiB). aarch64_pt_map_2m_block
     * returns -EINVAL on misaligned inputs; we route those segments
     * through aarch64_pt_map_4k_ext instead. */
    uint64_t head_skip = pa & (PAGE_2M_SIZE - 1);
    uint64_t cur_pa = pa - head_skip;          /* round DOWN to 2 MiB */
    uint64_t va_off = va + head_skip;          /* matching VA offset   */
    uint64_t total = len + head_skip;          /* length from cur_pa   */

    while (pa + len > cur_pa) {
        uint64_t this_len = pa + len - cur_pa;
        if (this_len > PAGE_2M_SIZE) this_len = PAGE_2M_SIZE;
        bool aligned = ((cur_pa & (PAGE_2M_SIZE - 1)) == 0)
                    && (this_len == PAGE_2M_SIZE);

        int rc;
        if (aligned) {
            rc = aarch64_pt_map_2m_block(kernel_pgd, va_off, cur_pa, perm);
        } else {
            /* Map in 4 KiB chunks for this segment. */
            rc = 0;
            for (uint64_t off = 0; off < this_len; off += PAGE_4K_SIZE) {
                int r = aarch64_pt_map_4k_ext(kernel_pgd, va_off + off,
                                              cur_pa + off, perm, 0);
                if (r != AARCH64_PT_OK) { rc = r; break; }
            }
        }
        if (rc != AARCH64_PT_OK) {
            kputs("[fb] map failed; Pos.FB_addr=NULL, color_printk disabled\n");
            Pos.FB_addr = NULL;
            return;
        }
        cur_pa += this_len;
        va_off += this_len;
    }

    Pos.FB_addr = (uint32_t *)AARCH64_FB_VIRT_BASE;
}
```

- [ ] **Step 3: Verify the symbol `kernel_pgd` resolves**

`kernel_pgd` is the global M1 runtime page-table root pointer used throughout aarch64 MMIO mapping. Confirm it exists and is exported:

Run: `grep -rn "^.*kernel_pgd" /home/aagu/OS01/kernel/arch/aarch64/ | head -10`

Expected: a definition like `uint64_t *kernel_pgd = ...;` somewhere (likely in `kernel/arch/aarch64/memory/vmm_backend.c` or `kernel/arch/aarch64/memory/runtime_tree.c`). If the actual global name differs (e.g., `aarch64_kernel_pgd` or `kernel_root`), update the references in `printk_fb.c` accordingly.

If `kernel_pgd` is not yet exported (e.g., currently a `static` in another TU), the printk_fb.c won't link. Fix path: either export the existing global by removing `static`, or use the public accessor (e.g., `arch_get_page_table()` returns the root). Prefer using the existing accessor if available.

**Document the actual symbol used here** in the plan's "Open follow-ups" section before committing.

- [ ] **Step 4: Build aarch64 — link must succeed (symbol resolves, even though nothing calls it yet)**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi 2>&1 | tee /tmp/aarch64-build3.log
```

Expected: aarch64-uefi artifact produced. The new `frame_buffer_init` symbol is now provided by `printk_fb.c`. `frame_buffer_early_init` is also defined. No link errors.

If link fails with `undefined reference to frame_buffer_init`: the wildcard didn't pick up `kernel/arch/aarch64/runtime/printk_fb.c`. Investigate and fix.

If link fails with `undefined reference to kernel_pgd` (or whatever the root pointer is named): go back to Step 3 and use the actual symbol.

- [ ] **Step 5: Commit**

Run:
```bash
cd /home/aagu/OS01
git add kernel/include/arch/aarch64/page_table.h \
        kernel/arch/aarch64/runtime/printk_fb.c
git commit -m "feat(aarch64): frame_buffer_init using aarch64_pt_map_*

Add the aarch64 TU that provides frame_buffer_init for color_printk.
Uses aarch64_pt_map_2m_block for 2 MiB-aligned segments and
aarch64_pt_map_4k_ext for head/tail segments (PA not 2 MiB-aligned
or size not a multiple of 2 MiB). Permission: KERNEL_RW | DEVICE.

AARCH64_FB_VIRT_BASE = 0xffff900000000000 is the dedicated kernel VA
for the FB region; chosen to avoid overlap with AARCH64_PT_SELFTEST_VA
(0xffff800000000000) and the direct-map region (ARCH_PAGE_OFFSET).

frame_buffer_early_init is provided as an empty stub (declared in
core/printk.h, defined by x86_64 for real): aarch64 keeps PL011 as
the boot console, so no early-init variant is needed.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 4: Add aarch64_boot_fb_init + plumb call site in main.c

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c` (add helper + call site after `arch_vmm_init`)

**Interfaces:**
- Consumes: `boot_context` (already in scope as `handoff` parameter), `Pos` global, `frame_buffer_init` (from `kernel/arch/aarch64/runtime/printk_fb.c` per Task 3), `kputs` (from `arch/aarch64/boot_log.h`)
- Produces: `aarch64_boot_fb_init(const struct boot_context *)` function (file-local); call from `aarch64_main` after `arch_vmm_init()`

- [ ] **Step 1: Read the existing aarch64_main arch_vmm_init block**

Run: `Read /home/aagu/OS01/kernel/arch/aarch64/boot/main.c` (lines 450–458)

Expected (currently):
```c
    {
        int rc = arch_vmm_init();
        if (rc) {
            kputs("FATAL: arch_vmm_init rc=-");
            kputu((uint64_t)(-(int64_t)rc));
            kputs("\n");
            for (;;) arch_cpu_halt();
        }
    }
```

- [ ] **Step 2: Add the call site immediately after the arch_vmm_init block**

Edit `kernel/arch/aarch64/boot/main.c`. Insert after the closing brace of the arch_vmm_init block (line 458 area):
```c
    {
        int rc = arch_vmm_init();
        if (rc) {
            kputs("FATAL: arch_vmm_init rc=-");
            kputu((uint64_t)(-(int64_t)rc));
            kputs("\n");
            for (;;) arch_cpu_halt();
        }
    }
+   /* NEW (spec 2026-10-06): populate Pos from bootctx->graphics and map
+    * the framebuffer MMIO via aarch64_pt_map_*. Color_printk becomes
+    * live as a result — every subsequent kputs/color_printk still goes
+    * to PL011 serial; the banner below is the first output that also
+    * lands on the screen.
+    *
+    * Called BEFORE selftest_run_all / dtb_init / gic_init / smp_boot_aps
+    * so APs see the live Pos via SMP cache coherence. APs only READ Pos
+    * (color_printk from kernel/memory/slab.c error paths); they never
+    * write to it. */
+   aarch64_boot_fb_init(handoff);
```

- [ ] **Step 3: Add the helper function definition**

In the same file, before `aarch64_main` (around line 350), add:
```c
/* Populate Pos from the UEFI-handoff graphics info and map the
 * framebuffer MMIO. Gate on BOOT_CONTEXT_HAS_FRAMEBUFFER so a system
 * without ramfb (or with broken GOP) still boots cleanly via PL011.
 *
 * Called once from aarch64_main, after arch_vmm_init returns success. */
static void aarch64_boot_fb_init(const struct boot_context *handoff)
{
    if (!(handoff->flags & BOOT_CONTEXT_HAS_FRAMEBUFFER)) {
        kputs("[fb] no framebuffer in handoff; color_printk disabled\n");
        return;
    }

    Pos.Phy_addr    = (uint32_t *)handoff->graphics.FrameBufferBase;
    Pos.FB_length   = handoff->graphics.FrameBufferSize;
    Pos.XResolution = handoff->graphics.HorizontalResolution;
    Pos.YResolution = handoff->graphics.VerticalResolution;
    Pos.XPosition   = 0;
    Pos.YPosition   = 0;
    /* spin_init(&Pos.lock): x86_64_boot_early does this on its path;
     * aarch64 has no equivalent stage, so we own the init here. */
    spin_init(&Pos.lock);

    frame_buffer_init();
    if (Pos.FB_addr) {
        color_printk(WHITE, BLUE,
                     "[fb] aarch64 color_printk active (%ux%u)\n",
                     Pos.XResolution, Pos.YResolution);
    } else {
        kputs("[fb] mapping failed; color_printk disabled\n");
    }
}
```

Verify `Pos.XPosition`/`Pos.YPosition` are `int32_t` fields in the `position` struct (per `kernel/include/core/printk.h:34-35`). Yes, they are.

- [ ] **Step 4: Confirm `WHITE`, `BLUE`, `spin_init` are available in scope**

Run: `grep -n "WHITE\|spin_init" /home/aagu/OS01/kernel/arch/aarch64/boot/main.c | head -5`

If `WHITE`/`BLUE` aren't already pulled in transitively: add `#include <core/printk.h>` to the top of main.c. The current `main.c` already has `#include <core/printk.h>` (line 9).

For `spin_init`: aarch64 has its own spinlock header at `kernel/include/arch/aarch64/spinlock.h`. Confirm it's transitively included. If not, add it.

- [ ] **Step 5: Build aarch64 — link must succeed**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi 2>&1 | tee /tmp/aarch64-build4.log
```

Expected: aarch64-uefi artifact produced; no link errors. The new `aarch64_boot_fb_init` is file-local; `frame_buffer_init` resolves from Task 3's TU; `color_printk` resolves from `kernel/core/printk.c` (still overridden by `printk_stub.c` in Task 5, but the symbol exists either way).

- [ ] **Step 6: Commit**

Run:
```bash
cd /home/aagu/OS01
git add kernel/arch/aarch64/boot/main.c
git commit -m "feat(aarch64): wire Pos from bootctx, plumb fb init after VMM

Add aarch64_boot_fb_init() helper and call it from aarch64_main
immediately after arch_vmm_init() returns. Populates Pos from
handoff->graphics and maps the FB MMIO via the new
kernel/arch/aarch64/runtime/printk_fb.c::frame_buffer_init.

Gated on BOOT_CONTEXT_HAS_FRAMEBUFFER: missing FB means Pos stays
zero-init and color_printk falls through to its NULL guard.

Prints a white-on-blue [fb] active banner via color_printk as the
visible smoke marker on QEMU -display gtk.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 5: Delete printk_stub.c

**Files:**
- Delete: `kernel/arch/aarch64/runtime/printk_stub.c`

- [ ] **Step 1: Verify nothing else in the aarch64 build depends on the stub's symbols**

Run: `grep -rn "color_printk\|serial_printk\|kputs" /home/aagu/OS01/kernel/arch/aarch64/runtime/printk_stub.c`
Expected: only the stub itself; no other TU includes the stub or depends on its layout.

- [ ] **Step 2: Verify no caller references something ONLY defined in the stub**

Run:
```bash
cd /home/aagu/OS01
nm build/aarch64-clang/kernel/kernel.elf 2>/dev/null | grep -E "color_printk|serial_printk" || echo "binaries not built yet"
```

Expected: `color_printk` resolves to `kernel/arch/aarch64/runtime/printk_stub.c` (currently the stub). After deletion, it will resolve to `kernel/core/printk.c` (the canonical).

- [ ] **Step 3: Delete the file**

Run: `rm /home/aagu/OS01/kernel/arch/aarch64/runtime/printk_stub.c`

- [ ] **Step 4: Rebuild aarch64 — must link cleanly with canonical color_printk**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi 2>&1 | tee /tmp/aarch64-build5.log
```

Expected: aarch64-uefi artifact produced; `color_printk` resolves from `kernel/core/printk.c`.

If link fails with `undefined reference to color_printk`: the wildcard didn't pick up `kernel/core/printk.c` for the aarch64 build. Investigate the kernel source list.

- [ ] **Step 5: Rebuild x86_64 — must be unaffected**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang kernel.bin 2>&1 | tee /tmp/x86-build5.log
```

Expected: x86_64 build clean; printk_stub.c was aarch64-only, so x86_64 doesn't notice its deletion.

- [ ] **Step 6: Commit**

Run:
```bash
cd /home/aagu/OS01
git add kernel/arch/aarch64/runtime/printk_stub.c   # staged as deletion
git status  # confirm: "deleted: printk_stub.c"
git commit -m "refactor(aarch64): remove color_printk stub

printk_stub.c was a forwarder to kputs that dropped FRcolor/BKcolor
and the actual framebuffer draw. With the aarch64 FB plumbing in place
(spec 2026-10-06), the canonical kernel/core/printk.c::color_printk
takes over; the NULL guard in putchark handles the no-FB case.

slab.c (the 4 caller sites on aarch64) now uses the canonical
color_printk, which honors FRcolor/BKcolor when Pos.FB_addr is set.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 6: Integration tests — visual smoke + missing-FB + full regressions

**Files:**
- Modify: none (pure test runs)

**Test gates:**
- T1: aarch64 boot with `-device ramfb` shows the white-on-blue banner on screen.
- T2: aarch64 boot without `-device ramfb` boots cleanly (no panic), output on PL011.
- T3: aarch64 SMP regression suite (4-CPU) passes.
- T4: x86_64 full regression (test-static + test-kernel-selftest + test-host) passes.

- [ ] **Step 1: T1 — Visual smoke with -device ramfb**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=aarch64-clang aarch64-uefi
# Start QEMU with GTK display; observe the screen for the banner.
# Use a VNC display if no X server is available:
DISPLAY=vnc=:0 make PROFILE=aarch64-clang run-aarch64-uefi
```

Expected on screen (and in serial):
```
[fb] aarch64 color_printk active (1024x768)
[clocksource] active=true
...
```

Pass criteria: the `[fb] aarch64 color_printk active` line is visible on screen (white text on blue background).

If only serial shows it: the FB mapping is correct but the screen output is buffered or scrolled. Try `clear` QEMU window or wait for the next screen update.

If neither serial nor screen shows it: `aarch64_boot_fb_init` didn't run or Pos wasn't populated. Re-check.

- [ ] **Step 2: T2 — Missing-FB regression**

Modify the test rig (locally, not committed): run QEMU WITHOUT `-device ramfb`. Edit `mk/components/run.mk`'s `run-aarch64-uefi` to temporarily remove `-device ramfb`:

Run (manual override, do not commit):
```bash
cd /home/aagu/OS01
# Use make's recipe variable override to skip ramfb.
# The simplest: edit /tmp/run-no-ramfb.mk with the modified QEMU line,
# then run that.
cat > /tmp/run-no-ramfb.sh <<'EOF'
#!/bin/bash
set -eu
cd /home/aagu/OS01
$(grep -E "^AARCH64_QEMU\s*=" mk/targets/aarch64.mk)
$(grep -E "^AARCH64_UEFI_FIRMWARE\s*=" mk/targets/aarch64.mk | head -1)
$(grep -E "^AARCH64_UEFI_DISK\s*=" mk/targets/aarch64.mk | head -1)
SMP=1 MEMORY=512
$AARCH64_QEMU -M virt,gic-version=2 -cpu cortex-a53 -smp $SMP -m $MEMORY \
  -drive if=pflash,format=raw,readonly=on,file=$AARCH64_UEFI_FIRMWARE \
  -drive if=none,file=$AARCH64_UEFI_DISK,format=raw,readonly=on,id=disk \
  -device virtio-blk-device,drive=disk \
  -serial stdio -display none -no-reboot
EOF
chmod +x /tmp/run-no-ramfb.sh
/tmp/run-no-ramfb.sh
```

Expected (on the `mavis run` screen): kernel boots, PL011 serial output shows `[fb] no framebuffer in handoff; color_printk disabled` followed by all subsequent logs. No panic.

- [ ] **Step 3: T3 — aarch64 SMP regression suite**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi
make PROFILE=aarch64-clang test-aarch64 MODE=smp
```

Expected: all SMP cases pass at 1- and 4-CPU. The fb init runs before SMP boot, so APs see the same `Pos` (the boot strap's writes propagate via cache coherence for the BSS-resident global).

- [ ] **Step 4: T4 — x86_64 full regression**

Run:
```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang test-kernel-selftest
make PROFILE=x86_64-clang test-host
```

Expected: all audits green; selftest image boots and reports `N total: N passed, 0 failed`; host tests all pass.

If anything fails: the refactor broke x86_64 — bisect by reverting individual Tasks and re-run T4.

- [ ] **Step 5: Commit any test-only helpers**

If T2 used a `/tmp/run-no-ramfb.sh` helper, it's NOT committed (in /tmp). The run.mk change from earlier (adding `-device ramfb`) is already on master.

If a CI-runnable no-ramfb variant is desired as a permanent feature (recommended): add a `run-aarch64-uefi-no-ramfb` target to `mk/components/run.mk` in a follow-up commit. **Not part of this plan's scope** — tracked in spec's "Out-of-scope follow-ups."

- [ ] **Step 6: Final merge prep**

Run:
```bash
cd /home/aagu/OS01
git log --oneline -10
git status
```

Expected: clean working tree; 5 commits in this plan's history (Task 1–5). If work happened in a worktree, merge the worktree branch back to master per the project's git workflow.

---

## Acceptance Criteria (cross-reference with spec)

- [ ] AC1 — `make PROFILE=aarch64-clang run-aarch64-uefi` with `-display gtk` (or VNC) shows the white-on-blue `[fb] aarch64 color_printk active` banner on screen.
- [ ] AC2 — Same run WITHOUT `-device ramfb` boots cleanly; PL011 shows all output; no panic.
- [ ] AC3 — `make PROFILE=x86_64-clang test-static test-kernel-selftest test-host` — all green.
- [ ] AC4 — `make PROFILE=aarch64-clang test-aarch64 MODE=smp` — all green.
- [ ] AC5 — Visual confirmation that the banner appears on the QEMU screen via VNC capture (saved under `test-results/aarch64-fb/` if a future test rig wants this).

---

## Open follow-ups (post-merge)

- **/dev/fb devfs chrdev on aarch64**: requires porting `kernel/driver/fb.c` (currently x86-only VMM assumptions). Out of scope.
- **Permanent no-ramfb CI target**: add `run-aarch64-uefi-no-ramfb` target so T2 is reproducible in CI. Not blocking.
- **`vsprintf` returning aarch64 string format** if any `color_printk` format string contains `%F`-looking sequences (unlikely; current format strings don't use it). Not blocking.
- **`AARCH64_FB_VIRT_BASE` selection rationale** documented in `kernel/include/arch/aarch64/page_table.h` (already done in this plan).

---

## Self-Review Notes

Spec coverage check (each spec section mapped to a task):

| Spec section | Task(s) |
|---|---|
| Context (UEFI capture_graphics, Pos zero-init, x86-specific fb impl) | T1 (extraction), T4 (Pos population) |
| Goals (color_printk on aarch64, NULL-safe, no regression) | T3+T4 (impl), T2 (NULL guard), T6 (regression) |
| Non-Goals (/dev/fb, /dev/gfx0, terminal.elf) | respected (not in plan) |
| Architecture diagram | T1, T2, T3, T4 collectively |
| File changes (NEW/MODIFIED/DELETED) | T1, T2, T3, T4, T5 |
| Why no frame_buffer_early_init | T3 (empty stub in TU; comment explains why) |
| Data flow (happy + missing FB) | T4 (happy), T2+T6 (missing) |
| Error handling (5 failure modes) | T3 (non-fatal on map fail), T4 (gated on flag), T2 (NULL guard) |
| Risks (6 listed) | T1 (extraction correctness — regression gate), T3 (PA collision — Device attr), T4 (Pos.lock init), T5 (linker), T6 (regression gate) |
| Testing (T1–T5) | T6 (covers T1–T4; T5 deferred to "Open follow-ups" — too much effort for the 3-line guard) |
| Acceptance Criteria | T6 + the AC checklist at end |

**Placeholder scan:** No "TBD", "TODO", "implement later" in the plan body. Task 3 Step 3 ("Document the actual symbol used") is an investigation step, not a placeholder.

**Type consistency:** `aarch64_boot_fb_init(const struct boot_context *)` matches the parameter name (`handoff`) used at the call site. `frame_buffer_init(void)` / `frame_buffer_early_init(void)` matches the existing declarations in `core/printk.h:54-55`. `AARCH64_FB_VIRT_BASE` is consistently spelled. `Pos.Phy_addr` is `uint32_t *` (matches `core/printk.h:37`).

**Review Focus coverage:** Each of the 5 review-focus items maps to a task test/step:
1. PA collision → T3 (Device attribute).
2. SMP visibility → T6 T3 (SMP regression suite).
3. `Pos.lock` init → T4 Step 3 (explicit `spin_init` call).
4. NULL guard no-crash → T6 T2 (no-ramfb integration).
5. spin_lock interaction → T2 (guard is BEFORE pixel write; in `putchark` not `color_printk`).
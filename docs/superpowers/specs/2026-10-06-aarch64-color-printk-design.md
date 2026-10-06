# aarch64 kernel-side color_printk support

**Date:** 2026-10-06
**Status:** Draft → Review
**Scope:** kernel-side only (no `/dev/fb`, no `/dev/gfx0`, no userspace terminal)

## Context

The aarch64 UEFI run recipe was changed (commit uncommitted; `run.mk` now has `-device ramfb` + `-display $(DISPLAY)` instead of `-display none`). The QEMU `virt` machine now exposes a `ramfb` MMIO region, and edk2's `RamfbDxe` picks it up via GOP. The UEFI bootloader already populates `boot_context->graphics` via `capture_graphics()` in `boot/uefi/main.c:72` (no change needed there).

What's missing is the **kernel-side plumbing**:

1. `kernel/arch/aarch64/runtime/printk_stub.c` overrides `color_printk` to forward to `kputs`, dropping `FRcolor`/`BKcolor` and the actual framebuffer draw.
2. `aarch64_main` (`kernel/arch/aarch64/boot/main.c:351`) never reads `bootctx->graphics`, so `Pos` (the global `position` in `kernel/core/printk.c`) stays zero-init.
3. `frame_buffer_init` / `frame_buffer_early_init` are x86_64-specific (`kernel/core/printk.c:171, 187`) — they directly write to PUD[0] PMD entries at `0xffff800000103000` and use x86 PTE bit flags. No aarch64 equivalent.

After this change, kernel-side `color_printk(FR, BK, fmt, ...)` will render to the screen on aarch64 (when a framebuffer is present in the handoff).

## Goals

1. `color_printk` (the canonical `kernel/core/printk.c` impl) renders to screen on aarch64.
2. NULL-safe when `BOOT_CONTEXT_HAS_FRAMEBUFFER` is absent (current code would SIGSEGV).
3. No regression on x86_64 boot path, selftest, or audit suites.
4. No regression on aarch64 SMP / GIC / sync-fault / M1-M3 selftest suites.

## Non-Goals

- `/dev/fb` devfs chrdev registration on aarch64 (`kernel/driver/fb.c` is x86-only structure).
- `/dev/gfx0` device or libgfx userspace.
- `terminal.elf` running on aarch64 (would need full devfs + gfx0 + PTY stack).
- Multi-framebuffer support, mode switching, GOP re-entry from kernel.

## Architecture

```
UEFI bootloader
  └─ capture_graphics() → bootctx->graphics.{FrameBufferBase, FrameBufferSize, HRes, VRes}
       └─ flags |= BOOT_CONTEXT_HAS_FRAMEBUFFER

aarch64_main
  └─ [arch_vmm_init]    ← M1 runtime page tables pinned (kernel_map)
       └─ [new] aarch64_boot_fb_init(bootctx)
            ├─ gate on flags & HAS_FRAMEBUFFER
            ├─ Pos.Phy_addr    = bootctx->graphics.FrameBufferBase
            ├─ Pos.FB_length   = bootctx->graphics.FrameBufferSize
            ├─ Pos.XResolution = bootctx->graphics.HorizontalResolution
            ├─ Pos.YResolution = bootctx->graphics.VerticalResolution
            └─ frame_buffer_init()           ← aarch64 per-arch TU

kernel/core/printk.c
  ├─ putchark() / putchar_at() : NULL guard at entry
  └─ color_printk() : spin_lock → putchark per char

kernel/arch/aarch64/runtime/printk_fb.c        [NEW]
  └─ frame_buffer_init() : aarch64_pt_map_2m_block / aarch64_pt_map_4k_ext
       → AARCH64_FB_VIRT_BASE | Pos.FB_addr = (uint32_t*)AARCH64_FB_VIRT_BASE

kernel/arch/x86_64/runtime/printk_fb.c        [NEW — extracted from printk.c]
  └─ frame_buffer_init / frame_buffer_early_init : x86_64 bodies verbatim
```

## File changes

### NEW: `kernel/arch/aarch64/runtime/printk_fb.c`

- Implements `frame_buffer_init(void)` only (no `frame_buffer_early_init` — see "no early-init" below).
- Uses `aarch64_pt_map_2m_block` for 2 MiB-aligned segments, falls back to `aarch64_pt_map_4k_ext` for the head (PA not 2 MiB-aligned) and tail (size not multiple of 2 MiB).
- Permission: `AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE` (no EXEC).
- After successful mapping, `Pos.FB_addr = (uint32_t *)AARCH64_FB_VIRT_BASE`.
- Non-fatal on any `-EINVAL` / `-ENOMEM` / `-EEXIST` from `aarch64_pt_map_*`: log via `kputs` and leave `Pos.FB_addr = NULL` (NULL guard takes over).

### NEW: `kernel/arch/x86_64/runtime/printk_fb.c`

- **Body extracted verbatim** from the existing `frame_buffer_init` / `frame_buffer_early_init` in `kernel/core/printk.c`. No behavior change.
- Headers it needs: `<arch/x86_64/pte.h>` for `PAGE_*` flags, `<memory/vmm.h>` for `vmm_map_page` / `kernel_map`, `<core/printk.h>` for `Pos` / `VIRT_FRAMEBUFFER_*`.

### MODIFIED: `kernel/core/printk.c`

- **Remove** the definitions of `frame_buffer_init` and `frame_buffer_early_init` (now provided by per-arch TUs).
- **Add** NULL guard at the top of `putchark` and `putchar_at`:

  ```c
  if (!Pos.FB_addr) return;   /* NULL-safe: caller has no fb */
  ```

- `color_printk` itself stays unchanged — the guard inside `putchark` is sufficient.

### MODIFIED: `kernel/include/core/printk.h`

- Add extern declarations (arch-neutral symbol, arch-specific body):

  ```c
  void frame_buffer_init(void);
  void frame_buffer_early_init(void);
  ```

  (Note: `frame_buffer_early_init` is x86_64-only today; aarch64 may declare it as a stub returning immediately to keep the linker happy, OR we make it arch-conditional. See "Open question 1" below.)

### MODIFIED: `kernel/arch/aarch64/boot/main.c`

- Insert the new `aarch64_boot_fb_init` call **after** `arch_vmm_init()` returns (line 458, just after the `if (rc)` fatal block):

  ```c
  aarch64_boot_fb_init(handoff);
  ```

- New helper `aarch64_boot_fb_init(const struct boot_context *)` defined in this same TU (or split out — see "Open question 2"). Body:

  ```c
  void aarch64_boot_fb_init(const struct boot_context *handoff)
  {
      if (!(handoff->flags & BOOT_CONTEXT_HAS_FRAMEBUFFER)) {
          kputs("[fb] no framebuffer in handoff; color_printk disabled\n");
          return;
      }
      Pos.Phy_addr    = (uint32_t *)handoff->graphics.FrameBufferBase;
      Pos.FB_length   = handoff->graphics.FrameBufferSize;
      Pos.XResolution = handoff->graphics.HorizontalResolution;
      Pos.YResolution = handoff->graphics.VerticalResolution;
      /* spin_init(&Pos.lock): x86_64_boot_early does this on its path;
       * aarch64 has no equivalent stage, so we own the init on this side. */
      spin_init(&Pos.lock);
      frame_buffer_init();
      color_printk(WHITE, BLUE, "[fb] aarch64 color_printk active (%ux%u)\n",
                   Pos.XResolution, Pos.YResolution);
  }
  ```

  The `color_printk` line is the visible smoke marker (uses white-on-blue per AGENTS.md color palette). If the run is via `-display gtk`, the operator sees this banner on screen.

### DELETED: `kernel/arch/aarch64/runtime/printk_stub.c`

- The `color_printk` stub becomes redundant: `kernel/core/printk.c` now provides the canonical version, and the NULL guard handles the no-FB case.

### NEW header symbol: `AARCH64_FB_VIRT_BASE`

Add to `kernel/include/arch/aarch64/page_table.h` (or a new `kernel/include/arch/aarch64/printk_fb.h` if we want to keep the printk-related stuff separate):

```c
#define AARCH64_FB_VIRT_BASE UINT64_C(0xffff900000000000)
```

This is below `ARCH_PAGE_OFFSET` (the direct-map region) and distinct from `AARCH64_PT_SELFTEST_VA` (0xffff800000000000) and the kernel text/data region. Verifies as canonical high-half kernel VA in the 0xffff_0000_0000_0000..0xffff_ffff_ffff_ffff range.

### Build wiring

- Per-arch `printk_fb.c` files are picked up by the existing per-arch wildcard in `kernel/Makefile` (already includes `arch/$(ARCH)/*.c` per `mk/components/kernel.mk`). No Makefile change needed.
- Removing `printk_stub.c`: same wildcard drops it automatically.

## Why no `frame_buffer_early_init` on aarch64

The x86_64 path needs `frame_buffer_early_init` because `boot_logo_show()` runs **before** `pmm_init`/`vmm_init` (`kernel/arch/x86_64/memory/boot.c:50`) — and `boot_logo_show` writes to the framebuffer. To support that, x86_64 hacks a PMD entry at `0xffff800000103000` (PUD[0] → PMD slot for VIRT_FRAMEBUFFER_EARLY) before any allocator is up.

On aarch64:

1. Early boot uses **PL011 serial** exclusively (`kputs`, `serial_printk`, `log_err` in `kernel/arch/aarch64/boot/main.c`).
2. The head.S identity map marks PA 0x40000000..0x80000000 as Normal cacheable. ramfb lives in QEMU-assigned MMIO range that overlaps this region. Reusing the identity map as-is for FB MMIO would have cache-coherence bugs (MMIO must be Device).
3. No early kernel consumer writes to `Pos.FB_addr` before `arch_vmm_init()`. Adding an early-init variant would mean either:
   - Re-encoding head.S to use Device attribute for ramfb PA (touches the most-delicate code), or
   - Writing a 2 MiB block to M1 root via aarch64_pt_map_2m_block before pmm — but `aarch64_pt_map_2m_block` requires `vmm_gate_check()` and `kernel_map` to be valid (set by `arch_vmm_init`), so this doesn't actually work pre-VMM.

**Conclusion**: enabling color_printk post-`arch_vmm_init` is sufficient for every existing aarch64 call site (`kernel/memory/slab.c` error paths — all run well after PMM/VMM are up).

## Data flow

### Happy path (UEFI GOP succeeds, ramfb present)

```
QEMU virt + -device ramfb
  → edk2 RamfbDxe installs GOP
  → capture_graphics() → bootctx->graphics
       FrameBufferBase = 0x4???_????  (QEMU-assigned; 16 MiB)
       FrameBufferSize = 0x0100_0000  (16 MiB default)
       HRes = 1024, VRes = 768        (or whatever QEMU sizes)
       flags |= HAS_FRAMEBUFFER
  → aarch64_main:
       arch_vmm_init() → kernel_map
       aarch64_boot_fb_init(handoff):
         Pos.Phy_addr/FB_length/XResolution/YResolution ← bootctx->graphics
         frame_buffer_init() →
           for each 2 MiB segment in (PA, PA+Size):
             aarch64_pt_map_2m_block(kernel_map, FB_VA+i, PA+i,
                                     KERNEL_RW | DEVICE)
           Pos.FB_addr = (uint32_t*)AARCH64_FB_VIRT_BASE
         color_printk(WHITE, BLUE, "[fb] aarch64 color_printk active ...\n")
            → spin_lock(Pos.lock)
            → putchark(W, B, '[') → *Pos.FB_addr = W or B per glyph pixel
            → ... per char
            → spin_unlock
  → screen shows the banner in white-on-blue
```

### Missing FB path (no ramfb, or GOP query failed)

```
capture_graphics() → EFI_DEVICE_ERROR or no GOP
  → flags has no HAS_FRAMEBUFFER
  → aarch64_main:
       aarch64_boot_fb_init(handoff):
         logs "[fb] no framebuffer in handoff; color_printk disabled"
         returns immediately; Pos untouched (zero-init)
  → later code that calls color_printk(FR, BK, fmt, ...):
       vsprintf works (no dep on Pos)
       putchark guard: if (!Pos.FB_addr) return;
       spin_lock/unlock still happen; no screen write
  → all output goes through PL011 serial — operator sees everything via -serial stdio
```

### x86_64 unchanged path

x86_64 boot stays exactly as today. The `frame_buffer_init` / `frame_buffer_early_init` bodies move from `kernel/core/printk.c` to `kernel/arch/x86_64/runtime/printk_fb.c` byte-identically. The call sites (`kernel/arch/x86_64/memory/boot.c:49, 60`) still find the symbols.

## Error handling

| Failure | Handling |
|---------|---------|
| `bootctx->flags` lacks `HAS_FRAMEBUFFER` | log via `kputs`, return; color_printk continues to no-op via NULL guard |
| `aarch64_pt_map_2m_block` returns `-EEXIST` | log "already mapped", continue (idempotent) |
| `aarch64_pt_map_2m_block` returns `-EINVAL` | log specific reason, skip that segment, continue |
| `aarch64_pt_map_2m_block` returns `-ENOMEM` | log "OOM during FB map", give up; Pos.FB_addr = NULL |
| `Pos.Phy_addr` not 4 KiB-aligned (shouldn't, but guard anyway) | bail with log; Pos.FB_addr = NULL |
| Any path leaves Pos.FB_addr = NULL | NULL guard in putchark absorbs subsequent calls |

All error paths are **non-fatal** — aarch64 boot continues, kernel log still emerges via PL011 serial.

## Testing

### T1: aarch64 visual smoke (the main thing)

```
PROFILE=aarch64-clang DISPLAY=gtk make run-aarch64-uefi
```

Expected: QEMU window opens. The screen shows (via ramfb) the white-on-blue banner:
```
[fb] aarch64 color_printk active (1024x768)
```
(or whatever dimensions QEMU picks). Followed by all subsequent `color_printk` / `log_err` / `kputs` output.

Pass criteria: banner appears on screen; kernel completes through `arch_vmm_init` and into the timer/SMP setup.

### T2: aarch64 missing-FB regression

```
# Run with -display gtk but no -device ramfb (force removal in test rig)
```

Pass criteria: PL011 serial shows all expected logs in the terminal; no panic; no banner on screen; color_printk calls don't crash.

### T3: x86_64 regression (gate on this)

```
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang test-kernel-selftest
make PROFILE=x86_64-clang test-host
```

Pass criteria: ALL existing audits + selftests pass. This proves the per-arch split + NULL guard didn't disturb the x86_64 path.

### T4: aarch64 SMP regression

```
make PROFILE=aarch64-clang test-aarch64 MODE=smp
```

Pass criteria: 4-CPU SMP selftest passes (existing baseline). The fb init runs before SMP boot so APs see the same `Pos`.

### T5: NULL guard hosttest (lightweight)

New `hosttests/cases/test_printk_null_fb.c`:
- Includes the real `kernel/core/printk.c` TU
- Stubs: `vsprintf`, `spin_lock`, `spin_unlock`, `write_serial_unlocked`
- Sets `Pos.FB_addr = NULL`, `Pos.XResolution = 1024`, `Pos.YResolution = 768`
- Calls `color_printk(WHITE, BLUE, "hello")`
- Verifies: no crash, returns `strlen("hello")` (or vsprintf's count)

Pass criteria: test exits 0; no SIGSEGV.

### T6: NULL guard unit assertion (when redactr asserts)

The `color_printk(WHITE, BLUE, "...")` call in `aarch64_boot_fb_init` itself acts as a forward-mode assertion — if Pos wasn't set up correctly, the call would either render garbage, SIGSEGV, or hang on Pos.lock contention. Any of those = fail.

## Risks

| Risk | Mitigation |
|------|-----------|
| `arm_frame_buffer_init` calls `aarch64_pt_map_2m_block` against `kernel_map` (M1 runtime root) but `Pos.FB_addr` is read by code paths that may still execute under the head.S identity map | All kernel code at this stage runs under TTBR1 (kernel_map); head.S identity map in TTBR0 is unused. Confirmed by reading `kernel/arch/aarch64/boot/main.c:383-386` which requires `(uint64_t)&_text_start >= ARCH_PAGE_OFFSET`. |
| ramfb PA collides with existing kernel image mapping (PA 0x40000000 area) | QEMU does not place ramfb at 0x40000000 — that's kernel image PA. ramfb is QEMU-assigned in the MMIO range. UEFI's capture_graphics reports whatever QEMU assigned; we map it. |
| TLB has stale entries for the new FB VA from prior speculative loads | `aarch64_pt_map_2m_block` already issues `tlb_invalidate_local(va)` after each install (verified at `kernel/arch/aarch64/memory/page_table.c:793, 1093`). |
| Removing `printk_stub.c` breaks the aarch64 build if other code path depends on the stub symbol | Only 4 callers of `color_printk` on aarch64 (`kernel/memory/slab.c`) — they'll now get the real impl. They're OK with NULL guard. |
| `Pos.lock` not initialized on aarch64 (x86_64 does `spin_init` in `x86_64_boot_early`) | New `aarch64_boot_fb_init` initializes `Pos.lock` before any `color_printk` call. |
| The split leaves two copies of `frame_buffer_init` (one in each per-arch TU) and the linker is unhappy | Each profile compiles only its own arch's TUs; x86_64 build doesn't see `kernel/arch/aarch64/runtime/printk_fb.c` and vice versa. Verified by `mk/components/kernel.mk`'s arch-scoped wildcard pattern (already used by `kernel/arch/x86_64/memory/boot.c`, etc.). |
| hosttest mocks (`test_gfx_device.c`, `test_pmm_boot_reservation.c`) need updates after the split | `test_gfx_device.c:123-124` already provides `void frame_buffer_init(void) {}` and `void frame_buffer_early_init(void) {}` stubs. These remain valid for the new symbol layout. No hosttest changes needed. |

## Decisions locked during self-review

1. **`frame_buffer_early_init` is declared for aarch64 as an empty stub** in `kernel/arch/aarch64/runtime/printk_fb.c`. The declaration lives in `kernel/include/core/printk.h` so any TU can reference it; both archs provide a body (x86_64 the real impl, aarch64 a one-liner `{ (void)Pos; }`). This is defensive: prevents surprise link errors if a future refactor accidentally tries to call it on aarch64, costs ~3 lines.

2. **`aarch64_boot_fb_init` lives in `kernel/arch/aarch64/boot/main.c` itself**, right next to the call site. main.c is the canonical boot-path orchestration TU; the helper is ~20 lines.

3. **Test-rig concern (`-display gtk` vs no X server)** — not a design blocker. The spec's T1 test rig chooses `DISPLAY=vnc=:0` or `DISPLAY=curses` per environment; `gtk` for local workstation dev only.

## Out-of-scope follow-ups (parked)

- `/dev/fb` chrdev registration on aarch64 (requires porting `kernel/driver/fb.c` aarch64 VMM assumptions).
- `libgfx` rendering test (`user/aarch64/test_lvgl`) — needs `/dev/fb` + `/dev/gfx0`.
- 64-bit pixel format (bpp > 32) detection from GOP `PixelFormat` — current code assumes 32bpp; if ramfb is ever 16bpp, characters will look wrong. Not a problem today (QEMU ramfb default is 32bpp BGRA).

## Acceptance criteria

This design is complete when:

1. `make PROFILE=aarch64-clang run-aarch64-uefi` with `-display gtk` shows the white-on-blue `[fb] aarch64 color_printk active` banner on screen.
2. The same run WITHOUT `-device ramfb` boots cleanly with all output on PL011 serial.
3. `make PROFILE=x86_64-clang test-static test-kernel-selftest test-host` — all green.
4. `make PROFILE=aarch64-clang test-aarch64 MODE=smp` — all green.
5. New hosttest T5 passes (NULL guard doesn't crash).
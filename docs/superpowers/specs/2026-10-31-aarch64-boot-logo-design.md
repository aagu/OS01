# aarch64 boot logo — draw the OS01 wordmark on the aarch64 framebuffer

**Date:** 2026-10-31
**Status:** Approved (design review passed 2026-10-31)
**Scope:** kernel-side only — boot-time drawing on the framebuffer. No `/dev/fb`, no
`/dev/gfx0`, no userspace terminal, no mode switching.
**Branch / worktree:** `feature/aarch64-boot-logo` @ `/home/aagu/OS01/.worktrees/aarch64-boot-logo`
**Base:** `fa7d51ac` (master, post-merge of `feature/aarch64-el1-sched`)

---

## Context

`kernel/driver/logo.c` owns the only copy of the OS01 "OS01" wordmark geometry. It
draws on the x86_64 framebuffer and has never run on aarch64, for two independent
reasons:

1. **It cannot link on aarch64.** The whole file depends on the display
   coordinator's writer lease (`fb_writer_begin` / `fb_writer_end`,
   `kernel/driver/fb_state.c`), and that TU is x86_64-only — it pulls in
   `driver/fb.h`, `driver/bga.h`, `tty/console.h`, `sync/mutex.h` and
   `sched/task.h`, none of which exist in the aarch64 minimal build.
2. **It is never called on aarch64.** The only call site is
   `kernel/arch/x86_64/memory/boot.c:50`.

Meanwhile aarch64 already owns a working framebuffer path (added by the
2026-10-06 color_printk spec): the UEFI bootloader captures GOP info into
`boot_context->graphics`, `aarch64_boot_fb_init()` fills `Pos`, and
`frame_buffer_init()` (`kernel/arch/aarch64/runtime/printk_fb.c`) maps the
ramfb MMIO range as Device memory at `AARCH64_FB_VIRT_BASE`. So the screen is
already a valid drawing surface on aarch64 — only the wordmark is missing.

### Verified facts this design rests on

Each item was checked in the worktree at `fa7d51ac`, not assumed.

| Fact | Evidence |
|---|---|
| The x86_64 logo path is healthy today and must not regress | `kernel/core/main.c:82` `x86_64_boot_early()` calls `fb_bootstrap_state()` (`kernel/arch/x86_64/platform/boot.c:94`); `main.c:85` `x86_64_boot_memory()` calls `frame_buffer_early_init()` (which calls `fb_publish_initial_mapping()`, `kernel/arch/x86_64/runtime/printk_fb.c:28`) and then `boot_logo_show()` (`kernel/arch/x86_64/memory/boot.c:50`). The lease is therefore initialised and the mapping published before the logo draws. |
| The drawing primitives are already arch-neutral | `kernel/core/printk.c` is linked by BOTH arches (aarch64 explicitly: `kernel/Makefile:81`). `putchar_at_snap()` falls back to `Pos.FB_addr` / `Pos.XResolution` / `Pos.YResolution` when `snap == NULL` (`kernel/core/printk.c:102-138`). The PSF font symbol and the `font.psf → font.o` rule are shared (`kernel/Makefile:529`, `kernel/core/printk.c:46`). |
| The aarch64 build links no `driver/*.c` | `kernel/Makefile:70-99` — the aarch64 `KERNEL_C_SOURCES` is an explicit list. |
| aarch64 has no display coordinator at all | `driver/fb_state.c` is absent from the aarch64 build; `color_printk` on aarch64 compiles with `PRINTK_FB_WRITER_LEASE 0` (`kernel/core/printk.c:35-39`) and draws straight to `Pos.FB_addr`. |
| `log_info` works on aarch64 | `kernel/arch/aarch64/runtime/log_impl.c` implements `_log_info_impl` (mini-formatter: `%s`, `%u`, `%lu`, `%p`) and defines `g_log_level = LOG_INFO`. Confirmed present in the built ELF. |
| ramfb + headless screendump works on aarch64 | Measured: QEMU 11.1.3, `-device ramfb -display none`, QMP `screendump` → `P6 800x600` PPM containing 8203 white px (UEFI banner) and 4282 blue px (the existing `[fb] aarch64 color_printk active` banner). |
| The existing aarch64 harness never creates a framebuffer | `qemutests/aarch64_uefi_smp.py:841` builds its QEMU argv WITHOUT `-device ramfb` (the `make run-aarch64-uefi` recipe in `mk/components/run.mk:178` does pass it). |

### Wordmark geometry (unchanged from the x86_64 implementation)

PSF2 font: 8x16 px, 128 glyphs, 16 bytes/glyph, header 32 bytes.
At the 800x600 resolution the aarch64 run produces (edk2 `RamfbDxe` default):

```
scale = 14, left = 32, top = 24, advance = 98, wordmark width = 364
wordmark bbox = (32, 24) .. (396, 122)
underline      = y 136
label row      = 10  (pixel y 160), label col = 4 (pixel x 32)
```

Colours (32-bit pixel values written by the kernel, read back as RGB by the
QEMU screendump): ink `0x00e8f0f7` → RGB(232,240,247); accent `0x003bd6c6` →
RGB(59,214,198); muted `0x00687886` → RGB(104,120,134).

---

## Goals

1. The aarch64 kernel draws the OS01 boot logo on the framebuffer during boot,
   using the SAME wordmark geometry and the SAME glyph table as x86_64 — one
   source of truth, no per-arch copy of the glyph bitmaps.
2. The arch label under the wordmark reads `aarch64  /  KERNEL` (x86_64 keeps
   `x86_64  /  KERNEL`).
3. The logo is the FIRST thing on the screen; the existing
   `[fb] aarch64 color_printk active` banner renders directly beneath it.
4. Zero regression on the x86_64 boot path: the same geometry code, the same
   call order, the same framebuffer lease semantics, and a byte-identical
   serial stream. (Pixel equivalence follows from the geometry code and the
   view values being untouched — see V5 for what is actually asserted.)
5. Automatic, reproducible verification: a QMP `screendump` pixel assertion
   plus a serial marker, both runnable headless in CI.

## Non-Goals

- Bringing the display coordinator (`driver/fb_state.c`, `driver/bga.c`,
  `tty/console.c`) into the aarch64 build. aarch64 stays a minimal build.
- `/dev/fb`, `/dev/gfx0`, or any userspace consumer of the logo.
- Resolution switching, mode setting, GUI, or pointer/keyboard integration.
- Changing the x86_64 logo pixels, scale rules, or call order.
- A second, per-arch copy of the glyph table (explicitly rejected: it would
  violate the repo's "no per-arch duplication" rule for `core/printk.c`-class
  code, see `docs/arch.md`).

---

## Architecture

### 1. New arch facade: `arch/fb_write.h`

The logo needs three things the two arches do not provide the same way:
(a) exclusive access to the framebuffer, (b) the framebuffer address and
geometry, (c) an arch identity string. (a) differs — x86_64 has a writer
lease, aarch64 has only `Pos.lock`; (b)/(c) are uniform.

Following the repo's documented **weak-default + strong-override** pattern
(`docs/arch.md` §"Facade / override 矩阵"), the facade is:

```c
/* kernel/include/arch/fb_write.h */
#include <driver/fb_state.h>   /* fb_lease_t — TYPE ONLY, no link dependency */
#include <arch/identity.h>     /* ARCH_NAME, ARCH_BOOT_LOGO_LABEL */

struct fb_write_view {
    /* Arch-private storage OWNED BY THE VIEW.
     * The x86_64 override keeps the display-coordinator lease here; the
     * aarch64 weak default leaves it untouched. Embedding the lease instead
     * of returning a pointer to it is what keeps `snap` valid for the whole
     * begin→draw→end sequence — a lease held in the implementation's stack
     * frame would dangle the moment begin() returned. */
    fb_lease_t lease;

    const struct fb_snapshot *snap;  /* NULL when the arch has no display coordinator */
    uint32_t *addr;                  /* framebuffer base to draw into          */
    uint32_t xres;                   /* pixels                                  */
    uint32_t yres;                   /* pixels                                  */
};

int  arch_fb_write_begin(struct fb_write_view *view);  /* <0 = no exclusive access */
void arch_fb_write_end(struct fb_write_view *view);
```

**Lease lifetime contract (binding on every implementation):**

1. The view is caller-owned. It MUST outlive the whole `begin → draw → end`
   sequence.
2. `arch_fb_write_begin()` MUST NOT hand back a pointer to storage whose
   lifetime ends when it returns. The x86_64 override sets
   `view->snap = &view->lease.snapshot` — the lease lives inside the view, so
   the pointer is valid until `arch_fb_write_end()`.
3. `begin` and `end` are strictly paired: one `begin` → at most one `end`, in
   the same scope, no nesting, no `end` without `begin`.
4. The caller serialises the pair (the logo holds `Pos.lock` across it), so no
   implementation may rely on being re-entered concurrently.

`driver/fb_state.h` is already included by `kernel/core/printk.c`, which is
compiled for BOTH arches — so embedding `fb_lease_t` adds no new include
burden and no new link dependency on the aarch64 side (the coordinator
functions are never called there).

The facade header itself holds no arch knowledge. The arch identity macros live in
a second, compile-time-only facade, `kernel/include/arch/identity.h`, which
selects the per-arch header with the same `#ifdef __x86_64__ /
#elif defined(__aarch64__)` idiom used by `arch/spinlock.h` and `arch/mmu.h`:

| Symbol | x86_64 | aarch64 |
|---|---|---|
| `ARCH_NAME` | `"x86_64"` | `"aarch64"` |
| `ARCH_BOOT_LOGO_LABEL` | `"x86_64  /  KERNEL"` | `"aarch64  /  KERNEL"` |

`kernel/include/arch/identity.h` → `arch/x86_64/identity.h` /
`arch/aarch64/identity.h`. Header-only: the identity is a compile-time
constant, so there is no `.c` and no new link-time symbol.

**Implementations:**

- **Weak default — `kernel/driver/arch_fb_write.c`** (arch-neutral driver set,
  `__attribute__((weak))`): no coordinator, no lease. Fills the view from
  `Pos.FB_addr` / `Pos.XResolution` / `Pos.YResolution`, sets `snap = NULL`,
  and leaves `view->lease` untouched. **This is what aarch64 links.** It
  touches no x86-only header, so the aarch64 build stays minimal.
- **Strong override — `kernel/arch/x86_64/platform/fb_write.c`**: calls
  `fb_writer_begin(&view->lease, 0)` / `fb_writer_end(&view->lease)` on the
  lease EMBEDDED IN THE VIEW, then sets `view->snap = &view->lease.snapshot`
  and fills `view->addr` / `view->xres` / `view->yres` from it. The linker
  resolves the strong symbol over the weak one.

### 2. `kernel/driver/logo.c` refactor

The drawing becomes a pure function of the view; the arch-specific access
lives behind the facade.

```c
/* kernel/include/driver/logo.h */
int boot_logo_show(void);   /* 0 = drawn, <0 = skipped (no framebuffer) */
```

```c
int boot_logo_show(void)
{
    struct fb_write_view view;
    if (arch_fb_write_begin(&view) < 0)
        return -EAGAIN;              /* skipped: no exclusive FB access */
    logo_draw(&view, ARCH_BOOT_LOGO_LABEL);
    arch_fb_write_end(&view);
    return 0;
}
```

Return value: `0` = the wordmark was drawn, `<0` = skipped (no framebuffer, or
no exclusive access on x86_64). The x86_64 call site ignores the return value
(exactly as today); the aarch64 call site uses it to pick the marker.

`logo_draw()` keeps the existing geometry byte-for-byte (scale rule, left/top,
advance, underline, label placement) and the existing `Pos.YPosition` /
`Pos.XPosition` advance so that later `color_printk` output continues beneath
the wordmark. The `fill_rect()` helper is re-pointed at `view->addr/xres/yres`
instead of `Pos.*` + snapshot, which makes the drawing path independent of
whether a snapshot exists.

**Invariant:** `view->addr` must equal `snap->addr` when `snap != NULL`, and
`Pos.FB_addr` when `snap == NULL`. Both implementations satisfy this, and the
label text is rendered through `putchar_at_snap(view->snap, ...)`, which
resolves the base the same way.

**Locking:** `boot_logo_show()` holds `Pos.lock` across begin → draw → end, as
today. The lock order is `Pos.lock` → coordinator lock on x86_64, matching
`color_printk` (`kernel/core/printk.c:152` → `fb_writer_begin`), so no AB-BA
deadlock is introduced.

**Logging:** `boot_logo_show()` stays silent — it is a drawing primitive, and
keeping it silent means the x86_64 serial output is byte-identical to today.
The boot path reports the outcome (see §3).

### 3. aarch64 call site

`kernel/arch/aarch64/boot/main.c`, function `aarch64_boot_fb_init()`:

```
spin_init(&Pos.lock)
if (!BOOT_CONTEXT_HAS_FRAMEBUFFER)  -> kputs("[fb] no framebuffer in handoff; ...")
else:
    fill Pos from handoff->graphics
    frame_buffer_init()             -> maps ramfb MMIO as Device, sets Pos.FB_addr
    if (!Pos.FB_addr) kputs("[fb] mapping failed; color_printk disabled")

if (boot_logo_show() == 0)          -> NEW: draws the wordmark
    log_info("[logo] boot logo drawn (%s, %ux%u)\n", ARCH_NAME, xres, yres)   -> NEW
else
    log_info("[logo] skipped: no framebuffer\n")                              -> NEW

if (Pos.FB_addr):
    color_printk(WHITE, BLUE, "[fb] aarch64 color_printk active (%ux%u)\n", ...)
    kputs("[fb] frame buffer remap succeed (banner on screen)\n")
```

The logo is drawn BEFORE the existing banner, so the banner lands under it
(the logo advances `Pos.YPosition` to `label_row + 2`).

Note the ORDER of the two serial records: the `[fb] frame buffer remap
succeed` line is emitted AFTER the logo call — inside the `if (Pos.FB_addr)`
block that also emits the `color_printk` banner — so the aarch64 serial
stream reads `[logo] boot logo drawn` → `[fb] frame buffer remap succeed`.
(The `[fb] aarch64 color_printk active` banner is written to the
FRAMEBUFFER by `color_printk`; on aarch64 `PRINTK_FB_WRITER_LEASE` is 0, so
`color_printk` never falls back to the serial port and that banner does not
appear in the serial stream.) The drawn marker is logged only when
`boot_logo_show()` returns 0; otherwise the skip marker is logged instead.

Marker strings (new, aarch64 only):

- success: `[logo] boot logo drawn (aarch64, 800x600)`
- skipped: `[logo] skipped: no framebuffer`

`log_info` is used (not a bare `serial_printk`) per `AGENTS.md` §"Kernel logging
policy"; the aarch64 mini-formatter supports `%s` and `%u`.

The existing aarch64 `kputs("[fb] frame buffer remap succeed (banner above on
screen)\n")` wording becomes inaccurate once the logo sits above the banner;
it is reworded to `("[fb] frame buffer remap succeed (banner on screen)\n")`.
The x86_64 message `"frame buffer remap succeed\n"`
(`kernel/arch/x86_64/memory/boot.c:61`) is a different string and is NOT
touched — `qemutests/test_x86_intr_fault.py:72` greps for it.

### 4. Build wiring

- `kernel/Makefile`: the aarch64 `KERNEL_C_SOURCES` gains
  `driver/logo.c` and `driver/arch_fb_write.c` (explicit list — aarch64 does
  not use the `driver/*.c` wildcard).
- x86_64 needs **no Makefile change**: the `$(wildcard driver/*.c)` list picks
  up the weak default, and `ARCH_SOURCE_DIRS` already contains `platform`, so
  `$(wildcard arch/x86_64/platform/*.c)` picks up the strong override.

---

## Data flow

```
UEFI (edk2 + RamfbDxe)
  └─ capture_graphics() -> boot_context->graphics{HRes,VRes,FrameBufferBase,FrameBufferSize}
       └─ BOOT_CONTEXT_HAS_FRAMEBUFFER

aarch64_main
  └─ aarch64_boot_fb_init(handoff)
       ├─ spin_init(&Pos.lock)                 [Pos.lock must be live before any printk]
       ├─ Pos.{Phy_addr,FB_length,XResolution,YResolution} = handoff->graphics
       ├─ frame_buffer_init()                  [map ramfb MMIO as Device @ AARCH64_FB_VIRT_BASE]
       │     └─ Pos.FB_addr = AARCH64_FB_VIRT_BASE
       ├─ boot_logo_show()
       │     ├─ arch_fb_write_begin(&view)     [weak default: view = {NULL, Pos.FB_addr, Pos.XResolution, Pos.YResolution}]
       │     ├─ logo_draw(&view, "aarch64  /  KERNEL")
       │     │     ├─ fill_rect() x 4 glyphs   [ink 0x00e8f0f7 / accent 0x003bd6c6]
       │     │     ├─ underline rects          [accent / muted]
       │     │     ├─ putchar_at_snap(NULL)    [label text, 8x16 PSF2 glyphs]
       │     │     └─ Pos.YPosition = label_row + 2; Pos.XPosition = 0
       │     └─ arch_fb_write_end(&view)
       └─ color_printk(WHITE, BLUE, "[fb] aarch64 color_printk active ...")
```

---

## Error handling

| Condition | Behaviour |
|---|---|
| No `BOOT_CONTEXT_HAS_FRAMEBUFFER` (no ramfb / GOP failed) | `aarch64_boot_fb_init()` keeps its current behaviour (serial-only boot). `boot_logo_show()` finds `Pos.FB_addr == NULL`, skips the draw and returns `<0`; the caller logs `[logo] skipped: no framebuffer`. No crash, no MMIO access. |
| `frame_buffer_init()` mapping failure | `Pos.FB_addr = NULL` (existing behaviour, `kernel/arch/aarch64/runtime/printk_fb.c:120`) → same skip path as above. |
| `arch_fb_write_begin()` fails on x86_64 (coordinator not initialised, transition in flight, or an owner holds the display) | `boot_logo_show()` returns `<0` and draws nothing. The x86_64 boot path already tolerates this — today the same failure simply returns early. |
| Locking | `Pos.lock` is held for the whole begin→draw→end sequence; interrupts are disabled via `spin_lock_irqsave`. |

No new failure mode is introduced on x86_64: the lease-acquisition contract and
its early-return behaviour are unchanged.

---

## Verification

### V1 — Build gates (no regression)

| Command | Expected |
|---|---|
| `make PROFILE=aarch64-clang aarch64-uefi-kernel` | exit 0 (baseline verified green at `fa7d51ac`) |
| `make PROFILE=aarch64-clang aarch64-uefi` | exit 0 (image + firmware) |
| `make PROFILE=x86_64-clang kernel.bin` | exit 0 — x86_64 still links both the weak default and the strong override without duplicate-symbol errors |
| `make PROFILE=x86_64-clang test-host` | exit 0 |
| `make PROFILE=x86_64-clang test-static` | exit 0 |

The aarch64 product ELF MUST contain `boot_logo_show` and MUST NOT contain
`fb_writer_begin` (the coordinator stays out of the aarch64 build) — asserted
with `nm`.

### V2 — Serial marker (aarch64)

The boot log of the ramfb run contains `[logo] boot logo drawn (aarch64, 800x600)`.
The run WITHOUT `-device ramfb` contains `[logo] skipped: no framebuffer` and
still reaches the idle loop (no crash, no hang).

### V3 — Pixel assertion (aarch64, headless)

New test `qemutests/aarch64_boot_logo.py`:

1. Launch QEMU exactly like `qemutests/aarch64_uefi_smp.py` PLUS
   `-device ramfb` and `-qmp unix:<sock>,server,nowait`, keeping
   `-display none` (headless; verified to work with ramfb).
2. Wait FIRST for the serial marker `[fb] frame buffer remap succeed` — the
   marker the kernel ALWAYS prints — and only then do a BOUNDED secondary
   wait (5 s) for `[logo] boot logo drawn`. The primary wait deliberately
   targets an always-present marker, so a missing logo is reported as a
   FAILED ASSERTION with the screenshot still captured, never as a timeout
   with no evidence; the bounded secondary wait only gives the kernel a
   moment to finish the draw before the screendump, and a logo marker that
   never arrives stays a failed assertion too — not a timeout.
3. QMP `screendump` → PPM (`P6`, 800x600).
4. Derive the expected geometry from the screendump dimensions using the SAME
   scale rule the kernel uses (`xres >= 800 → 14`, `>= 480 → 11`, else `8`;
   `yres < 300 → 7`), so the assertions hold at any GOP resolution:

   ```
   left    = 32 (xres >= 400, else 16)      top     = 24
   advance = 7 * scale                       width   = 3*advance + 5*scale
   ```

5. Assert EXACT pixel counts (the glyph bitmaps are deterministic, so the
   counts are computable, not thresholds):

   | Colour | Derivation | Expected at 800x600 (scale 14) |
   |---|---|---|
   | ink `RGB(232,240,247)` | glyphs "O" (20 set bits) + "S" (17) = 37 × scale² | **7252** |
   | accent `RGB(59,214,198)` | glyphs "0" (23) + "1" (10) = 33 × scale² + underline `3*scale*2` | **6552** |
   | muted `RGB(104,120,134)` | the `aarch64  /  KERNEL` label glyphs | > 0 |

   The per-glyph bit counts come from the bitmap table in `kernel/driver/logo.c`
   (`O`=20, `S`=17, `0`=23, `1`=10 set bits) and are re-derived by the test, not
   hard-coded.

   These colours are unique to the logo — the measured edk2 banner uses
   (255,255,255), (152,152,152), (0,0,255), (190,195,208), (126,135,161),
   (255,85,0) and (64,77,115), none of which collide.

6. Assert the ink pixels are confined to the two ink glyph cells
   (`x ∈ [left, left + 2*advance)`, `y ∈ [top, top + 7*scale)`) — the wordmark
   is not smeared over the screen.

7. Assert muted pixels exist in the label CHARACTER CELL BAND
   `y ∈ [label_row*16, label_row*16 + 16)` = `y ∈ [160, 176)` at 800x600,
   within `x ∈ [left, left + 18*8)` — the `aarch64  /  KERNEL` text was
   rendered.

   The band, not a single scanline: in the shipped PSF2 font EVERY glyph used
   by the label has a zero first row (`row0 == 0x00` for `a a r c h 6 4 / K E R
   N E L` and the spaces), so the scanline at `y == 160` is legitimately all
   black. The first non-zero glyph row is 2 (uppercase) / 4 (`/`) / 5
   (lowercase), i.e. muted pixels start at `y == 162`.

### V4 — Negative case (aarch64, no framebuffer)

Same launch without `-device ramfb`: assert the serial log contains
`[logo] skipped: no framebuffer` and that the kernel still reaches
`[sched] idle-ready`.

### V5 — x86_64 no-regression

The x86_64 logo path must be unchanged. Four acceptance criteria, all
required:

1. **Serial stream unchanged** — the x86_64 boot log contains no new `[logo]`
   line (the driver is silent on x86_64 by design) and still contains
   `frame buffer remap succeed`.

2. **Weak/strong resolution on x86_64** — `nm` on the x86_64 ELF shows
   `arch_fb_write_begin` defined EXACTLY ONCE and as a STRONG symbol (`T`),
   never as a weak one (`W`): the strong override in
   `kernel/arch/x86_64/platform/fb_write.c` won over the weak default.

3. **Override really delegates to the coordinator** — a source-level check
   (grep, not `nm`): `kernel/arch/x86_64/platform/fb_write.c` contains a call
   to `fb_writer_begin(`. The x86_64 ELF must still contain the
   `fb_writer_begin` symbol (defined once, in `kernel/driver/fb_state.c`).

   The NUMBER of `fb_writer_begin` callers is deliberately NOT a criterion:
   `nm` lists symbols, not call sites, so no link-time check can count them.
   The pre-existing call sites are `kernel/driver/fb.c:364`,
   `kernel/driver/fb_test.c:374`, `kernel/tty/console.c:58` and
   `kernel/core/printk.c:161`; this change adds one more (the override) and
   REMOVES one (`kernel/driver/logo.c:54`, whose lease use moves into the
   override), so the set is expected to stay the same size.

4. **aarch64 stays coordinator-free** — `nm` on the aarch64 ELF shows
   `arch_fb_write_begin` as a weak symbol (`W`, the default) and shows NO
   `fb_writer_begin`, `fb_bootstrap_state` or `fb_publish_initial_mapping`
   symbol at all.

The pixel-level x86_64 comparison is explicitly NOT part of this change: the
geometry code is untouched and the x86_64 view values are the same lease
snapshot as before, so a screendump diff would only re-verify code that this
change does not modify.

### V6 — Make target

`make PROFILE=aarch64-clang test-aarch64-boot-logo` runs V2–V4 and exits
non-zero on failure.

---

## Change list

| File | Change |
|---|---|
| `kernel/include/arch/fb_write.h` | NEW — facade: `struct fb_write_view`, `arch_fb_write_begin/end`; includes `arch/identity.h` so consumers get the arch macros |
| `kernel/include/arch/identity.h` | NEW — facade selecting the per-arch identity macros |
| `kernel/include/arch/x86_64/identity.h` | NEW — `ARCH_NAME`, `ARCH_BOOT_LOGO_LABEL` |
| `kernel/include/arch/aarch64/identity.h` | NEW — `ARCH_NAME`, `ARCH_BOOT_LOGO_LABEL` |
| `kernel/driver/arch_fb_write.c` | NEW — weak default: direct `Pos` draw, `snap = NULL` |
| `kernel/arch/x86_64/platform/fb_write.c` | NEW — strong override: display-coordinator lease |
| `kernel/driver/logo.c` | REFACTOR — `logo_draw()` pure drawing; `boot_logo_show()` uses the facade; arch label |
| `kernel/include/driver/logo.h` | UPDATE — `int boot_logo_show(void)` |
| `kernel/arch/aarch64/boot/main.c` | UPDATE — call `boot_logo_show()`, emit the `[logo]` markers, reword the `[fb]` message |
| `kernel/Makefile` | UPDATE — aarch64 `KERNEL_C_SOURCES` += `driver/logo.c`, `driver/arch_fb_write.c` |
| `qemutests/aarch64_boot_logo.py` | NEW — QMP screendump pixel assertions (V2–V4) |
| `mk/components/run.mk` | UPDATE — `test-aarch64-boot-logo` target (V6) |
| `docs/arch.md` | UPDATE — add the `fb_write` row to the facade/override matrix |

---

## Risks

| Risk | Mitigation |
|---|---|
| Weak/strong symbol collision on x86_64 (both TUs define `arch_fb_write_begin`) | The default carries `__attribute__((weak))`; the x86_64 override does not. The linker prefers the strong symbol. Verified by `nm` on the linked ELF (V1). |
| The aarch64 weak default accidentally pulls the coordinator in | The weak default includes only `core/printk.h` and `arch/fb_write.h` — no `driver/fb_state.h` symbol is referenced. `nm` asserts `fb_writer_begin` is absent from the aarch64 ELF. |
| `Pos.lock` held across the lease acquisition on x86_64 | Same order as `color_printk` (`Pos.lock` → coordinator lock). No inversion. |
| The logo overwrites the UEFI banner | The logo is drawn at the top-left (`left = 32`, `top = 24`), exactly as on x86_64. The edk2 boot banner is drawn by the firmware before the kernel starts; the existing `[fb]` banner already overwrites part of it today. Behaviour is unchanged in kind. |
| Device-memory writes to ramfb | Already the case for the existing `color_printk` banner; the logo uses the same 32-bit store pattern. |
| x86_64 serial output changes | The driver stays silent; the markers are emitted only by the aarch64 boot path. The x86_64 serial stream is byte-identical. |

## Rollback

The change is additive on aarch64 and a pure refactor on x86_64. Reverting the
branch restores the previous behaviour exactly; no data-format or ABI change is
involved (no structure in `boot_context`, no UAPI change).

---

## Out of scope (explicitly not done here)

- Drawing the logo anywhere other than the boot framebuffer.
- Making the logo configurable, animated, or resolution-independent beyond the
  existing scale rule.
- Any user-space tooling to display or verify the logo.
- Changing the `qemutests/aarch64_uefi_smp.py` harness to always add
  `-device ramfb` (the new logo test carries it instead; changing the shared
  harness would alter the SMP/gic/sync-fault baselines).
